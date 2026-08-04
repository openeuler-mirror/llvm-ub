// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <array>

#include "cache.hpp"
#include "thread.hpp"
#include "cas.hpp"
#include "allocator_types.hpp"
#include "heap.hpp"
#include "huge.hpp"
#include "published.hpp"
#include "segment.hpp"
#include "log.hpp"

namespace uballoc {

inline size_t align_up(size_t val, size_t align) {
    return (val + align - 1) & ~(align - 1);
}

struct SharedLayout {
    static constexpr uint64_t MAGIC = 0x55BA110C00001ULL;
    static constexpr uint32_t LAYOUT_VERSION = 6;

    uint64_t magic;
    uint32_t layout_version;
    uint32_t _padding0;

    size_t slab_count_small;
    size_t slab_count_large;
    size_t slab_capacity_small;
    size_t slab_capacity_large;

    size_t help_array_offset;
    size_t allocator_shared_offset;
    size_t small_shared_offset;
    size_t large_shared_offset;
    size_t huge_shared_offset;
    size_t owned_array_offset;
    size_t total_shared_size;

    size_t seg_dir_small_offset;
    size_t seg_dir_large_offset;
    size_t seg_dir_huge_offset;

    std::atomic<uint32_t> ready_flag;
};

struct DistributedSharedRegionLayout {
    size_t help_array_offset;
    size_t allocator_shared_offset;
    size_t small_shared_offset;
    size_t large_shared_offset;
    size_t huge_shared_offset;
    size_t owned_array_offset;
    size_t published_registry_offset;
    size_t seg_dir_small_offset;
    size_t seg_dir_large_offset;
    size_t seg_dir_huge_offset;
    size_t huge_slots_offset;
    size_t total_size;

    int rank;
    int total_processes;
    size_t huge_slot_count;
};

template<typename AllocatorSharedT = DistributedAllocatorShared>
inline DistributedSharedRegionLayout compute_distributed_shared_region_layout(
        int rank, int total_processes, size_t huge_slot_count,
        size_t slab_count_small, size_t slab_count_large) {
    (void)slab_count_small;
    (void)slab_count_large;
    DistributedSharedRegionLayout layout;
    layout.rank = rank;
    layout.total_processes = total_processes;
    layout.huge_slot_count = huge_slot_count;

    size_t off = 0;

    off += sizeof(SharedLayout);

    size_t help_rows_bytes = MAX_NUM_CORES * COUNT_THREAD * sizeof(std::atomic<uint16_t>);
    layout.help_array_offset = align_up(off, alignof(std::atomic<uint16_t>));
    off = layout.help_array_offset + help_rows_bytes;

    layout.allocator_shared_offset = align_up(off, alignof(AllocatorSharedT));
    off = layout.allocator_shared_offset + sizeof(AllocatorSharedT);

    layout.small_shared_offset = align_up(off, alignof(HeapShared<Small>));
    off = layout.small_shared_offset + sizeof(HeapShared<Small>);

    layout.large_shared_offset = align_up(off, alignof(HeapShared<Large>));
    off = layout.large_shared_offset + sizeof(HeapShared<Large>);

    layout.huge_shared_offset = align_up(off, alignof(std::atomic<uint64_t>));
    off = layout.huge_shared_offset + sizeof(HugeShared);

    size_t owned_size = sizeof(ThreadArray<AllocatorOwned>);
    layout.owned_array_offset = align_up(off, alignof(AllocatorOwned));
    off = layout.owned_array_offset + owned_size;

    layout.published_registry_offset = align_up(off, alignof(PublishedEntry));
    off = layout.published_registry_offset + sizeof(PublishedRegistry);

    // type_id_map is now in the BootstrapBlock, not in per-process metadata.
    // No offset needed here.

    layout.seg_dir_small_offset = align_up(off, alignof(SegmentDirectory));
    off = layout.seg_dir_small_offset + sizeof(SegmentDirectory);

    layout.seg_dir_large_offset = align_up(off, alignof(SegmentDirectory));
    off = layout.seg_dir_large_offset + sizeof(SegmentDirectory);

    layout.seg_dir_huge_offset = align_up(off, alignof(SegmentDirectory));
    off = layout.seg_dir_huge_offset + sizeof(SegmentDirectory);

    layout.huge_slots_offset = align_up(off, alignof(std::atomic<uint64_t>));
    off = layout.huge_slots_offset + MAX_HUGE_SLOTS * sizeof(std::atomic<uint64_t>);

    layout.total_size = align_up(off, SIZE_PAGE);

    LOG_TRACE("compute_distributed_shared_region_layout: pid=" << rank
              << " total_size=" << layout.total_size
              << " seg_dir_small=" << layout.seg_dir_small_offset
              << " seg_dir_large=" << layout.seg_dir_large_offset
              << " seg_dir_huge=" << layout.seg_dir_huge_offset
              << " huge_slots=" << layout.huge_slots_offset);

    return layout;
}

}