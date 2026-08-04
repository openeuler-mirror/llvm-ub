// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <atomic>
#include <optional>
#include <cassert>

#include "packed.hpp"
#include "thread.hpp"
#include "size.hpp"
#include "data.hpp"
#include "bitset.hpp"
#include "slab.hpp"
#include "cas.hpp"
#include "cache.hpp"
#include "log.hpp"
#include "shm_provider.hpp"

namespace uballoc {

enum HugeStateEnum : uint8_t {
    LIVE = 0,
    FREE = 1,
    SAFE = 2
};

struct HugeDescriptor {
    uint64_t index;
    uint64_t offset_raw;
    size_t size_val;
    std::atomic<uint8_t> state;
};

struct HugeAllocator {
    uint64_t next_index = 0;
};

struct HugeSharedDynamicLayout {
    size_t slots_count = 0;
    size_t slots_offset = 0;
    size_t hint_offset = 0;
    size_t total_size = 0;

    static HugeSharedDynamicLayout compute(size_t slot_count) {
        (void)slot_count;
        HugeSharedDynamicLayout layout;
        layout.slots_count = MAX_HUGE_SLOTS;
        layout.slots_offset = 0;
        layout.hint_offset = MAX_HUGE_SLOTS * sizeof(std::atomic<uint64_t>);
        layout.total_size = layout.hint_offset + sizeof(std::atomic<uint64_t>);
        return layout;
    }

    std::atomic<uint64_t>* slots_ptr(char* base) const {
        return reinterpret_cast<std::atomic<uint64_t>*>(base + slots_offset);
    }

    std::atomic<uint64_t>* hint_ptr(char* base) const {
        return reinterpret_cast<std::atomic<uint64_t>*>(base + hint_offset);
    }
};

struct HugeShared {
    std::atomic<uint64_t> slots[1024];
    std::atomic<uint64_t> hint;
    std::atomic<uint32_t> growing_;
    std::atomic<uint32_t> segment_count_;
    char _pad[56];

    HugeShared() {
        for (auto& slot : slots) slot.store(0, std::memory_order_relaxed);
        hint.store(0, std::memory_order_relaxed);
        growing_.store(0, std::memory_order_relaxed);
        segment_count_.store(1, std::memory_order_relaxed);
    }

    void init_growth(uint32_t seg_count) {
        growing_.store(0, std::memory_order_relaxed);
        segment_count_.store(seg_count, std::memory_order_relaxed);
    }

    bool try_grow(GrowFn fn) {
        uint32_t state = growing_.load(std::memory_order_acquire);
        if (state == 2) return false;

        uint32_t expected = 0;
        if (growing_.compare_exchange_strong(expected, 1,
                std::memory_order_acq_rel, std::memory_order_acquire)) {
            bool success = fn && fn();
            growing_.store(success ? 0u : 2u, std::memory_order_release);
            return success;
        } else {
            while (growing_.load(std::memory_order_acquire) == 1) {
                __asm__ __volatile__("" ::: "memory");
            }
            return growing_.load(std::memory_order_acquire) == 0;
        }
    }
};

struct HugeCheckedResult {
    int owner_pid;
    uint64_t offset;
};

struct Huge {
    HugeAllocator allocator;
    HugeShared* shared;
    HugeSharedDynamicLayout shared_layout;
    Data<HugeSize> data;
    char* region_base;
    ThreadId thread_id;

    HugeShared** cross_shared_ptrs = nullptr;
    char** cross_region_bases = nullptr;
    int total_processes = 1;
    int rank = 0;

    Huge() : shared(nullptr), region_base(nullptr), thread_id(ThreadId(0)) {}
    Huge(HugeShared* sh, Data<HugeSize> dt, char* rb, ThreadId tid)
        : shared(sh), data(dt), region_base(rb), thread_id(tid) {}
    Huge(HugeShared* sh, Data<HugeSize> dt, char* rb, ThreadId tid,
         HugeShared** cross_sp, char** cross_rb, int tp, int r)
        : shared(sh), data(dt), region_base(rb), thread_id(tid),
          cross_shared_ptrs(cross_sp), cross_region_bases(cross_rb),
          total_processes(tp), rank(r) {}
    Huge(Huge&& other) noexcept
        : allocator(std::move(other.allocator)), shared(other.shared),
          shared_layout(other.shared_layout), data(std::move(other.data)),
          region_base(other.region_base), thread_id(other.thread_id),
          cross_shared_ptrs(other.cross_shared_ptrs),
          cross_region_bases(other.cross_region_bases),
          total_processes(other.total_processes), rank(other.rank) {
        other.shared = nullptr;
        other.region_base = nullptr;
        other.cross_shared_ptrs = nullptr;
        other.cross_region_bases = nullptr;
    }
    Huge& operator=(Huge&& other) noexcept {
        allocator = std::move(other.allocator);
        shared = other.shared;
        shared_layout = other.shared_layout;
        data = std::move(other.data);
        region_base = other.region_base;
        thread_id = other.thread_id;
        cross_shared_ptrs = other.cross_shared_ptrs;
        cross_region_bases = other.cross_region_bases;
        total_processes = other.total_processes;
        rank = other.rank;
        other.shared = nullptr;
        other.region_base = nullptr;
        other.cross_shared_ptrs = nullptr;
        other.cross_region_bases = nullptr;
        return *this;
    }

    static constexpr size_t MAX_SLOTS = 1024;

    static uint64_t make_slot_value(ThreadId id, size_t slot_count) {
        return (static_cast<uint64_t>(id.internal()) << 32) |
               (static_cast<uint64_t>(slot_count) << 16) | 1;
    }

    static size_t get_slot_count(uint64_t val) {
        return static_cast<size_t>((val >> 16) & 0xFFFF);
    }

    static ThreadId get_owner(uint64_t val) {
        uint16_t tid = static_cast<uint16_t>((val >> 32) & 0xFFFF);
        return ThreadId(tid);
    }

    std::atomic<uint64_t>* get_slots(HugeShared* sh) {
        return shared_layout.slots_count > 0
            ? shared_layout.slots_ptr(reinterpret_cast<char*>(sh))
            : sh->slots;
    }

    std::atomic<uint64_t>* get_hint(HugeShared* sh) {
        return shared_layout.slots_count > 0
            ? shared_layout.hint_ptr(reinterpret_cast<char*>(sh))
            : &sh->hint;
    }

    size_t active_slot_count(HugeShared* sh) {
        size_t max_slots = shared_layout.slots_count > 0
            ? shared_layout.slots_count : MAX_SLOTS;
        uint32_t seg_count = sh->segment_count_.load(std::memory_order_acquire);
        size_t seg_active = static_cast<size_t>(seg_count) * HUGE_SLOTS_PER_SEGMENT;
        return std::min(max_slots, seg_active);
    }

    void* scan_and_claim(HugeShared* sh, char* base,
                         ThreadId id, size_t size, size_t slot_count,
                         HugeDescriptor& out) {
        std::atomic<uint64_t>* slots = get_slots(sh);
        std::atomic<uint64_t>* hint = get_hint(sh);
        size_t active = active_slot_count(sh);

        if (active == 0 || slot_count > active) return nullptr;

        size_t start_hint = hint->load(std::memory_order_acquire);
        if (start_hint >= active) start_hint = 0;

        for (size_t pass = 0; pass < 2; ++pass) {
            size_t i = (pass == 0) ? start_hint : 0;
            size_t end = (pass == 0) ? active : start_hint;

            while (i + slot_count <= end) {
                bool all_free = true;
                size_t next_i = i;
                for (size_t j = i; j < i + slot_count; ++j) {
                    uint64_t val = slots[j].load(std::memory_order_acquire);
                    if (val != 0) {
                        all_free = false;
                        size_t existing_count = get_slot_count(val);
                        next_i = (existing_count > 0) ? j + existing_count : j + 1;
                        break;
                    }
                }

                if (!all_free) {
                    i = next_i;
                    continue;
                }

                uint64_t desired = make_slot_value(id, slot_count);
                uint64_t expected = 0;
                if (slots[i].compare_exchange_strong(expected, desired,
                        std::memory_order_acq_rel, std::memory_order_acquire)) {
                    for (size_t j = i + 1; j < i + slot_count; ++j) {
                        slots[j].store(desired, std::memory_order_release);
                    }
                    std::atomic_thread_fence(std::memory_order_release);
                    hint->store(i + slot_count, std::memory_order_release);

                    out.index = allocator.next_index++;
                    out.offset_raw = i * HugeSize::SLAB_SIZE;
                    out.size_val = size;
                    out.state.store(HugeStateEnum::LIVE, std::memory_order_relaxed);
                    return base + out.offset_raw;
                }

                i += 1;
            }
        }

        return nullptr;
    }

    void* allocate(ThreadId id, size_t size, HugeDescriptor& out) {
        // FRAGMENTATION: slot_count = ceil(size / 4MB) rounds up to the
        // nearest 4MB. For 5MB this means 2 slots = 8MB (37.5% waste).
        // See HugeSize::SLAB_SIZE comment for the recommended 2MB-slot
        // optimization and trade-offs.
        size_t slot_count = (size + HugeSize::SLAB_SIZE - 1) / HugeSize::SLAB_SIZE;

        void* p = scan_and_claim(shared, region_base, id, size, slot_count, out);
        if (p) return p;

        GrowFn fn = grow_fn_huge().load(std::memory_order_acquire);
        if (fn && shared->try_grow(fn)) {
            p = scan_and_claim(shared, region_base, id, size, slot_count, out);
            if (p) return p;
        }

        bool borrow = (affinity_mode().load(std::memory_order_acquire) ==
                       static_cast<size_t>(AffinityMode::ReuseFirst));
        if (borrow && cross_shared_ptrs && cross_region_bases) {
            for (int k = 0; k < total_processes; ++k) {
                if (k == rank || !cross_shared_ptrs[k] || !cross_region_bases[k])
                    continue;
                p = scan_and_claim(cross_shared_ptrs[k], cross_region_bases[k],
                                    id, size, slot_count, out);
                if (p) {
                    LOG_WARN("Cross-node borrow: local pool exhausted, "
                             << "borrowed from process " << k
                             << " (remote-node access, 2x latency)");
                    return p;
                }
            }
        }

        LOG_ERROR("allocate: out of huge slots (local pool exhausted, growth failed)");
        return nullptr;
    }

    void free_offset([[maybe_unused]] ThreadId id, uint64_t offset, int owner_pid) {
        HugeShared* sh = (owner_pid >= 0 && owner_pid < total_processes && cross_shared_ptrs)
            ? cross_shared_ptrs[owner_pid] : shared;
        if (!sh) {
            LOG_WARN("free_offset: no HugeShared for owner_pid=" << owner_pid);
            return;
        }

        std::atomic<uint64_t>* slots = get_slots(sh);
        size_t slot_index = offset / HugeSize::SLAB_SIZE;

        uint64_t val = slots[slot_index].load(std::memory_order_acquire);
        if (val == 0) {
            LOG_WARN("free_offset: slot " << slot_index << " of pid "
                     << owner_pid << " is not claimed");
            return;
        }

        size_t slot_count = get_slot_count(val);
        if (slot_count == 0) {
            LOG_WARN("free_offset: slot_count == 0");
            return;
        }

        size_t start = slot_index;
        for (size_t back = 0; back + 1 < slot_count && start > 0; ++back) {
            uint64_t prev = slots[start - 1].load(std::memory_order_acquire);
            if (prev != val) break;
            --start;
        }

        for (size_t j = start; j < start + slot_count; ++j) {
            uint64_t expected = val;
            if (!slots[j].compare_exchange_strong(expected, 0,
                    std::memory_order_acq_rel, std::memory_order_acquire)) {
                LOG_WARN("free_offset: CAS failed at slot " << j
                         << " of pid " << owner_pid
                         << " (already freed or reused)");
                break;
            }
        }

        std::atomic_thread_fence(std::memory_order_release);
    }

    std::optional<HugeCheckedResult> checked_pointer_to_offset(void* ptr) {
        if (!ptr) return std::nullopt;
        uintptr_t p = reinterpret_cast<uintptr_t>(ptr);

        for (int k = 0; k < total_processes; ++k) {
            char* base = (cross_region_bases && cross_region_bases[k])
                ? cross_region_bases[k]
                : (k == rank ? region_base : nullptr);
            if (!base) continue;

            uintptr_t lo = reinterpret_cast<uintptr_t>(base);
            uintptr_t hi = lo + 0x18000000000ULL;
            if (p < lo || p >= hi) continue;

            uint64_t offset = p - lo;
            size_t slot_index = offset / HugeSize::SLAB_SIZE;

            HugeShared* sh = (cross_shared_ptrs && cross_shared_ptrs[k])
                ? cross_shared_ptrs[k]
                : (k == rank ? shared : nullptr);
            if (!sh) continue;

            std::atomic<uint64_t>* slots = get_slots(sh);
            uint64_t val = slots[slot_index].load(std::memory_order_acquire);
            if (val == 0) return std::nullopt;

            size_t sc = get_slot_count(val);
            size_t start = slot_index;
            for (size_t back = 0; back + 1 < sc && start > 0; ++back) {
                uint64_t prev = slots[start - 1].load(std::memory_order_acquire);
                if (prev != val) break;
                --start;
            }

            uint64_t alloc_start = start * HugeSize::SLAB_SIZE;
            uint64_t alloc_end = alloc_start + sc * HugeSize::SLAB_SIZE;
            if (offset < alloc_start || offset >= alloc_end) return std::nullopt;

            return HugeCheckedResult{k, offset};
        }

        if (region_base) {
            uintptr_t lo = reinterpret_cast<uintptr_t>(region_base);
            uintptr_t hi = lo + 0x18000000000ULL;
            if (p >= lo && p < hi) {
                uint64_t offset = p - lo;
                size_t slot_index = offset / HugeSize::SLAB_SIZE;
                uint64_t val = shared ? get_slots(shared)[slot_index].load(std::memory_order_acquire) : 0;
                if (val == 0) return std::nullopt;
                return HugeCheckedResult{rank, offset};
            }
        }

        return std::nullopt;
    }

    // NOTE: returns slot_count * SLAB_SIZE, which is inflated for non-round
    // allocations (e.g., 5MB alloc → class_size = 8MB). This affects realloc:
    // realloc(p, new_size) where new_size <= class_size shrinks in place
    // (even if new_size > original requested size). With a 2MB slot size
    // (recommended optimization, see HugeSize::SLAB_SIZE), class_size would
    // be more accurate and realloc-shrink would trigger later.
    size_t class_size_from_offset(int owner_pid, uint64_t offset) {
        HugeShared* sh = (owner_pid >= 0 && owner_pid < total_processes && cross_shared_ptrs)
            ? cross_shared_ptrs[owner_pid] : shared;
        if (!sh) return SIZE_PAGE;

        std::atomic<uint64_t>* slots = get_slots(sh);
        size_t slot_index = offset / HugeSize::SLAB_SIZE;
        uint64_t val = slots[slot_index].load(std::memory_order_acquire);
        if (val == 0) return SIZE_PAGE;
        size_t sc = get_slot_count(val);
        return sc * HugeSize::SLAB_SIZE;
    }
};

}