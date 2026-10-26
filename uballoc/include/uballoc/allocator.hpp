// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <cstddef>
#include <optional>
#include <functional>
#include <cassert>
#include <cstring>

#include "packed.hpp"
#include "thread.hpp"
#include "size.hpp"
#include "data.hpp"
#include "bitset.hpp"
#include "slab.hpp"
#include "cas.hpp"
#include "heap.hpp"
#include "huge.hpp"
#include "recover.hpp"
#include "cache.hpp"
#include "allocator_types.hpp"

namespace uballoc {

// Fallback policy for node-targeted allocation (malloc_on) when the
// requested node cannot serve the allocation — invalid/unrouted node,
// growth failure (MAX_SEGMENTS, quota, provider node down, single-node
// provider), or the v1 head-only owned-list miss. C mirror:
// uballoc_node_fallback_t (uballoc.h). See node_targeted_allocation.md §5.
enum class NodeFallback : uint32_t {
    Fail = 0,      // return nullptr
    Local = 1,     // plain malloc (local-node-first)
    AnyNode = 2,   // plain malloc (v1: identical to Local)
};

struct AllocatorContext {
    ThreadId id;
    AllocatorOwned* owned;
    DistributedHelpArray* distributed_help;

    AllocatorContext() : owned(nullptr), distributed_help(nullptr) {}
    AllocatorContext(ThreadId tid, AllocatorOwned* ow, DistributedHelpArray* dh)
        : id(tid), owned(ow), distributed_help(dh) {}

    Version load_version(ThreadId target) {
        if (!distributed_help) return Version();
        return distributed_help->load(id, target);
    }

    void store_version(ThreadId target, Version ver) {
        if (!distributed_help) return;
        distributed_help->store(id, target, ver);
    }

    Version next_version() {
        Version current = load_version(id);
        Version next = current.next();
        store_version(id, next);
        return next;
    }

    DistributedHelpArray* get_distributed_help() { return distributed_help; }
};

template<typename S = void, typename O = void>
struct Allocator {
    DistributedAllocatorShared* distributed_shared;
    ThreadArray<AllocatorOwned>* owned_array;
    AllocatorOwned* owned;
    AllocatorContext context;

    Heap<Small> small;
    Heap<Large> large;
    Huge huge;

    ThreadId thread_id;
    int rank;
    int total_processes;
    size_t num_cores;
    bool initialized;

    Allocator() : distributed_shared(nullptr), owned_array(nullptr), owned(nullptr),
                  context(), small(), large(), huge(),
                  thread_id(ThreadId(0)), rank(0), total_processes(1),
                  num_cores(MAX_NUM_CORES), initialized(false) {}

    void init_distributed(DistributedAllocatorShared* dsh, ThreadArray<AllocatorOwned>* ow,
                          Heap<Small> sm, Heap<Large> lg, Huge h,
                          int rank, int nprocs, size_t ncores) {
        distributed_shared = dsh;
        owned_array = ow;
        small = std::move(sm);
        large = std::move(lg);
        huge = std::move(h);
        this->rank = rank;
        total_processes = nprocs;
        num_cores = ncores;
        initialized = true;
    }

    void focus(ThreadId id) {
        thread_id = id;
        if (owned_array) {
            size_t local_idx = id.get() - rank * num_cores;
            // Thread ids are GLOBAL: rank r's threads occupy
            // [r*num_cores, (r+1)*num_cores). An id below this rank's
            // block (e.g. a locally-numbered id like init_thread(0) on a
            // rank>0 process) underflows and would index the per-rank
            // owned array far out of bounds.
            assert(local_idx < num_cores
                   && "thread id outside this rank's block — use global ids "
                      "(rank*num_cores + local)");
            owned = &(*owned_array)[ThreadId(local_idx)];
        }
        if (distributed_shared) {
            context = AllocatorContext(id, owned, &distributed_shared->help);
            // Wire help array to Heap<B> so push/pop/free_remote can call
			// update() with do_help.
            small.set_help(context.get_distributed_help());
            large.set_help(context.get_distributed_help());
        }
    }

    void recover() {
        if (owned) {
            recover_allocator(thread_id, small, large, owned->state, context.get_distributed_help());
        }
    }

    void* allocate(size_t size) {
        auto class_ = Small::new_from_size(size);
        if (!class_) {
            return allocate_large(size);
        }

        // Reclaim race retry: pop() returns nullptr when the (idx, block)
        // peeked earlier was invalidated by segment reclaim between the
        // two calls (possible if this thread was preempted). Retry with a
        // fresh peek. Bounded to avoid pathological live-lock.
        for (int retry = 0; retry < 64; ++retry) {
            auto peek_result = small.peek(thread_id, *class_);
            if (!peek_result) return nullptr;

            auto [idx, block] = *peek_result;
            void* ptr = small.pop(thread_id, *class_, idx, block);
            if (ptr) {
                if (owned) owned->state.clear();
                return ptr;
            }
        }
        return nullptr;
    }

    void* allocate_large(size_t size) {
        auto class_ = Large::new_from_size(size);
        if (!class_) {
            return allocate_huge(size);
        }

        // Reclaim race retry (same as allocate() for Small).
        for (int retry = 0; retry < 64; ++retry) {
            auto peek_result = large.peek(thread_id, *class_);
            if (!peek_result) return nullptr;

            auto [idx, block] = *peek_result;
            void* ptr = large.pop(thread_id, *class_, idx, block);
            if (ptr) return ptr;
        }
        return nullptr;
    }

    void* allocate_huge(size_t size) {
        size_t page_size = SIZE_PAGE;
        size_t aligned_size = ((size + page_size - 1) / page_size) * page_size;

        HugeDescriptor desc;
        void* ptr = huge.allocate(thread_id, aligned_size, desc);
        return ptr;
    }

    // Node-targeted allocation (malloc_on, node_targeted_allocation.md §5).
    // Routes Small/Large through the backing-filtered owned lists and
    // node-targeted growth; Huge has no node targeting in v1 and applies
    // the fallback directly. The pop-retry loop mirrors allocate()'s
    // reclaim-race handling.
    void* allocate_on(size_t size, uint32_t node, NodeFallback fallback) {
        auto apply_fallback = [&]() -> void* {
            if (fallback == NodeFallback::Fail) return nullptr;
            return allocate(size);  // Local / AnyNode → plain malloc (v1: same)
        };

        if (node >= MAX_PROCESSES) return apply_fallback();

        auto class_small = Small::new_from_size(size);
        if (class_small) {
            for (int retry = 0; retry < 64; ++retry) {
                auto blk = small.allocate_on(thread_id, *class_small, node);
                if (!blk) return apply_fallback();
                void* ptr = small.pop(thread_id, *class_small, blk->first, blk->second);
                if (ptr) {
                    if (owned) owned->state.clear();
                    return ptr;
                }
                // peeked (slab, block) went stale (reclaim race) — retry
            }
            return apply_fallback();
        }

        auto class_large = Large::new_from_size(size);
        if (class_large) {
            for (int retry = 0; retry < 64; ++retry) {
                auto blk = large.allocate_on(thread_id, *class_large, node);
                if (!blk) return apply_fallback();
                void* ptr = large.pop(thread_id, *class_large, blk->first, blk->second);
                if (ptr) {
                    if (owned) owned->state.clear();
                    return ptr;
                }
            }
            return apply_fallback();
        }

        // Huge: no node targeting in v1 — apply the fallback.
        return apply_fallback();
    }

    void free(void* ptr) {
        if (!ptr) return;

        auto small_offset = small.checked_pointer_to_offset(ptr);
        if (small_offset) {
            small.free_offset(thread_id, *small_offset);
            if (owned) owned->state.clear();
            return;
        }

        auto large_offset = large.checked_pointer_to_offset(ptr);
        if (large_offset) {
            large.free_offset(thread_id, *large_offset);
            return;
        }

        auto huge_result = huge.checked_pointer_to_offset(ptr);
        if (huge_result) {
            huge.free_offset(thread_id, huge_result->offset, huge_result->owner_pid);
            return;
        }

        LOG_WARN("Free: pointer not in any bracket range");
    }

    void* realloc(void* old_ptr, size_t new_size) {
        if (!old_ptr) return allocate(new_size);

        size_t old_size = class_size(old_ptr);
        if (old_size >= new_size) return old_ptr;

        void* new_ptr = allocate(new_size);
        if (!new_ptr) return nullptr;

        std::memcpy(new_ptr, old_ptr, old_size);
        free(old_ptr);
        return new_ptr;
    }

    size_t class_size(void* ptr) {
        if (!ptr) return 0;

        auto small_offset = small.checked_pointer_to_offset(ptr);
        if (small_offset) {
            return small.get_class(*small_offset).size();
        }

        auto large_offset = large.checked_pointer_to_offset(ptr);
        if (large_offset) {
            return large.get_class(*large_offset).size();
        }

        auto huge_result = huge.checked_pointer_to_offset(ptr);
        if (huge_result) {
            return huge.class_size_from_offset(huge_result->owner_pid,
                                               huge_result->offset);
        }

        return 0;
    }
};

}
