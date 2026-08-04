// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <cstddef>
#include <atomic>
#include <cstring>
#include <algorithm>

#include "size.hpp"
#include "cache.hpp"
#include "slab.hpp"
#include "log.hpp"

namespace uballoc {

inline size_t segment_align_up(size_t val, size_t align) {
    return (val + align - 1) & ~(align - 1);
}

struct SegmentHeader {
    static constexpr uint64_t MAGIC = 0x5E6354000001ULL;

    uint64_t magic;
    uint32_t segment_index;
    uint32_t region_type;
    size_t slab_size;
    size_t slab_count;
    size_t slab_local_offset;
    size_t slab_remote_offset;
    size_t data_offset;
    size_t total_size;
    std::atomic<uint32_t> ready;
    std::atomic<uint32_t> live_slab_count;
    std::atomic<uint64_t> free_since_ns;

    SegmentHeader() : magic(0), segment_index(0), region_type(0), slab_size(0),
                      slab_count(0), slab_local_offset(0), slab_remote_offset(0),
                      data_offset(0), total_size(0), ready(0),
                      live_slab_count(0), free_since_ns(0) {}
};

struct SegmentDescriptor {
    char name[64];
    size_t total_size;
    size_t slab_count;
    std::atomic<uint32_t> ready;
    // attach_count moved to BootstrapBlock.data_refcount (always accessible,
    // not dependent on lender's metadata being mapped). See get_refcount_ptr
    // in distributed_backend.hpp.

    SegmentDescriptor() : total_size(0), slab_count(0), ready(0) {
        name[0] = '\0';
    }
};

struct SegmentDirectory {
    std::atomic<uint32_t> count;
    char _padding[60];
    SegmentDescriptor descs[MAX_SEGMENTS];

    void init() {
        count.store(0, std::memory_order_relaxed);
        for (size_t i = 0; i < MAX_SEGMENTS; ++i) {
            descs[i].name[0] = '\0';
            descs[i].total_size = 0;
            descs[i].slab_count = 0;
            descs[i].ready.store(0, std::memory_order_relaxed);
        }
    }
};

template<typename B>
struct SegmentLayout {
    size_t header_offset;
    size_t slab_local_offset;
    size_t slab_remote_offset;
    size_t data_offset;
    size_t total_size;
    size_t slab_count;

    static SegmentLayout compute(size_t requested_slabs) {
        SegmentLayout layout;
        layout.header_offset = 0;

        size_t off = sizeof(SegmentHeader);
        off = segment_align_up(off, alignof(SlabLocal<B>));
        layout.slab_local_offset = off;

        size_t per_slab_meta = sizeof(SlabLocal<B>) + sizeof(Detectable<Remote>);
        size_t available = SEGMENT_VA_SIZE - sizeof(SegmentHeader) - 128;
        size_t max_slabs = available / (per_slab_meta + B::SLAB_SIZE);

        layout.slab_count = std::min(requested_slabs, max_slabs);

        layout.slab_remote_offset = segment_align_up(
            layout.slab_local_offset + max_slabs * sizeof(SlabLocal<B>),
            alignof(Detectable<Remote>));
        layout.data_offset = segment_align_up(
            layout.slab_remote_offset + max_slabs * sizeof(Detectable<Remote>),
            SIZE_PAGE);
        layout.total_size = segment_align_up(
            layout.data_offset + layout.slab_count * B::SLAB_SIZE,
            SIZE_PAGE);

        return layout;
    }

    SlabLocal<B>* slab_local_ptr(char* base) const {
        return reinterpret_cast<SlabLocal<B>*>(base + slab_local_offset);
    }

    Detectable<Remote>* slab_remote_ptr(char* base) const {
        return reinterpret_cast<Detectable<Remote>*>(base + slab_remote_offset);
    }

    char* data_ptr(char* base) const {
        return base + data_offset;
    }

    static constexpr size_t virtual_slabs_per_segment() {
        return SEGMENT_VA_SIZE / B::SLAB_SIZE;
    }
};

template<typename B>
inline SegmentHeader* slab_segment_header(Slab<B>& slabs, SlabIndex<B> global_idx) {
    int pid = slabs.find_process(global_idx.get());
    if (pid < 0 || pid >= static_cast<int>(MAX_PROCESSES)) return nullptr;
    size_t local_idx = global_idx.get() - slabs.cumulative[pid];
    size_t seg = (slabs.slabs_per_segment_ > 0)
        ? local_idx / slabs.slabs_per_segment_ : 0;
    if (seg >= MAX_SEGMENTS) return nullptr;
    SlabSlice<B, SlabLocal<B>>& slice = slabs.local_slices[pid][seg];
    SlabLocal<B>* base = slice.data();
    if (!base) return nullptr;
    size_t slab_local_off = segment_align_up(sizeof(SegmentHeader), alignof(SlabLocal<B>));
    char* seg_base = reinterpret_cast<char*>(base) - slab_local_off;
    return reinterpret_cast<SegmentHeader*>(seg_base);
}

}
