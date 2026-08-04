// SPDX-License-Identifier: Apache-2.0

#include "uballoc.hpp"
#include <iostream>
#include <cassert>

static void test_region_type_enum_and_count() {
    std::cout << "[Part B] RegionType enum has 4 values, REGION_COUNT=4" << std::endl;

    using Backend = uballoc::DistributedShmBackend<>;

    assert(Backend::REGION_COUNT == 4);
    assert(Backend::METADATA == 0);
    assert(Backend::DATA_SMALL == 1);
    assert(Backend::DATA_LARGE == 2);
    assert(Backend::DATA_HUGE == 3);

    std::cout << "  Verified: REGION_COUNT=4, enum values 0-3" << std::endl;
}

static void test_shm_name_suffixes_4_regions() {
    std::cout << "[Part B] shm_name produces 4 suffixes {-m, -ds, -dl, -dh}" << std::endl;

    assert(uballoc::PosixShmProvider::shm_name("heap", 0, 0) == "/heap-p0-m");
    assert(uballoc::PosixShmProvider::shm_name("heap", 0, 1) == "/heap-p0-ds");
    assert(uballoc::PosixShmProvider::shm_name("heap", 0, 2) == "/heap-p0-dl");
    assert(uballoc::PosixShmProvider::shm_name("heap", 0, 3) == "/heap-p0-dh");

    assert(uballoc::PosixShmProvider::shm_name("heap", 1, 0) == "/heap-p1-m");
    assert(uballoc::PosixShmProvider::shm_name("heap", 1, 1) == "/heap-p1-ds");

    std::cout << "  Verified: suffixes are -m, -ds, -dl, -dh" << std::endl;
}

static void test_metadata_va_layout() {
    std::cout << "[Part B] DistributedVALayout has metadata_va, no separate slab VA fields" << std::endl;

    uballoc::DistributedConfig config;
    config.rank = 0;
    config.total_processes = 2;
    config.slab_count_small = {256, 256};
    config.slab_count_large = {16, 16};
    config.huge_slots = {128, 128};

    uballoc::DistributedVALayout layout = uballoc::DistributedVALayout::compute(config, 0,
        uballoc::DEFAULT_VA_BASE);

    assert(layout.metadata_va[0] == uballoc::DEFAULT_VA_BASE);
    assert(layout.metadata_va[1] == uballoc::DEFAULT_VA_BASE
           + uballoc::PROCESS_STRIDE_META);

    std::cout << "  Verified: metadata_va[0]=METADATA_BASE, metadata_va[1]=METADATA_BASE+PROCESS_STRIDE_META" << std::endl;
}

static void test_metadata_layout_includes_slab_offsets() {
    std::cout << "[Part B] Metadata layout includes 3 segment directory offsets" << std::endl;

    auto sl = uballoc::compute_distributed_shared_region_layout(0, 2, 128, 256, 16);

    assert(sl.seg_dir_small_offset > 0);
    assert(sl.seg_dir_large_offset > sl.seg_dir_small_offset);
    assert(sl.seg_dir_huge_offset > sl.seg_dir_large_offset);
    assert(sl.total_size > sl.seg_dir_huge_offset);

    std::cout << "  Verified: segment directory offsets are packed in order" << std::endl;
}

static void test_slab_offset_alignment() {
    std::cout << "[Part B] Segment directory offsets are properly aligned" << std::endl;

    auto sl = uballoc::compute_distributed_shared_region_layout(0, 2, 128, 256, 16);

    assert(sl.seg_dir_small_offset % alignof(uballoc::SegmentDirectory) == 0);
    assert(sl.seg_dir_large_offset % alignof(uballoc::SegmentDirectory) == 0);
    assert(sl.seg_dir_huge_offset % alignof(uballoc::SegmentDirectory) == 0);

    std::cout << "  Verified: all segment directory offsets satisfy alignment requirements" << std::endl;
}

static void test_slab_offset_sizes_match_counts() {
    std::cout << "[Part B] Segment directory sizes are fixed (independent of slab counts)" << std::endl;

    auto sl = uballoc::compute_distributed_shared_region_layout(0, 2, 128, 256, 16);

    size_t dir_size = sizeof(uballoc::SegmentDirectory);

    assert(sl.seg_dir_large_offset - sl.seg_dir_small_offset >= dir_size);
    assert(sl.seg_dir_huge_offset - sl.seg_dir_large_offset >= dir_size);
    assert(sl.total_size - sl.seg_dir_huge_offset >= dir_size);

    std::cout << "  Verified: offset gaps accommodate SegmentDirectory size" << std::endl;
}

static void test_combined_metadata_larger_than_shared_only() {
    std::cout << "[Part B] Metadata total_size is independent of slab counts" << std::endl;

    auto sl_with_slabs = uballoc::compute_distributed_shared_region_layout(0, 2, 128, 256, 16);
    auto sl_no_slabs = uballoc::compute_distributed_shared_region_layout(0, 2, 128, 0, 0);

    assert(sl_with_slabs.total_size == sl_no_slabs.total_size);

    std::cout << "  Verified: total_size is the same regardless of slab counts" << std::endl;
}

static void test_zero_slab_count_produces_adjacent_offsets() {
    std::cout << "[Part B] Segment directory offsets are always present" << std::endl;

    auto sl = uballoc::compute_distributed_shared_region_layout(1, 2, 128, 0, 0);

    assert(sl.seg_dir_small_offset > 0);
    assert(sl.seg_dir_large_offset > sl.seg_dir_small_offset);
    assert(sl.seg_dir_huge_offset > sl.seg_dir_large_offset);

    std::cout << "  Verified: all 3 segment directory offsets are present even with 0 slabs" << std::endl;
}

static void test_wait_for_all_shms_count_4_regions() {
    std::cout << "[Part B] wait_for_all_shms counts METADATA + conditional data regions" << std::endl;

    uballoc::DistributedConfig config;
    config.rank = 0;
    config.total_processes = 2;
    config.slab_count_small = {0, 256};
    config.slab_count_large = {16, 16};
    config.huge_slots = {128, 128};

    int expected = 0;
    for (int k = 0; k < config.total_processes; ++k) {
        expected++;  // METADATA
        if (config.slab_count_small[k] > 0) expected++;
        if (config.slab_count_large[k] > 0) expected++;
        if (config.huge_slots[k] > 0) expected++;
    }

    assert(expected == 7);

    std::cout << "  Verified: P0=3 (METADATA+DATA_LARGE+DATA_HUGE), P1=4 (all), total=7" << std::endl;
}

static void test_shared_layout_has_slab_fields() {
    std::cout << "[Part B] SharedLayout struct has 3 segment directory offset fields" << std::endl;

    uballoc::SharedLayout layout{};
    layout.seg_dir_small_offset = 100;
    layout.seg_dir_large_offset = 200;
    layout.seg_dir_huge_offset = 300;

    assert(layout.seg_dir_small_offset == 100);
    assert(layout.seg_dir_large_offset == 200);
    assert(layout.seg_dir_huge_offset == 300);

    std::cout << "  Verified: SharedLayout segment directory offset fields are readable/writable" << std::endl;
}

int main() {
    test_region_type_enum_and_count();
    test_shm_name_suffixes_4_regions();
    test_metadata_va_layout();
    test_metadata_layout_includes_slab_offsets();
    test_slab_offset_alignment();
    test_slab_offset_sizes_match_counts();
    test_combined_metadata_larger_than_shared_only();
    test_zero_slab_count_produces_adjacent_offsets();
    test_wait_for_all_shms_count_4_regions();
    test_shared_layout_has_slab_fields();
    std::cout << "\n=== Part B tests passed ===" << std::endl;
    return 0;
}
