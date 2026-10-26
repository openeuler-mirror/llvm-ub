// SPDX-License-Identifier: Apache-2.0
 
/**
 * @file test_ar07_heap_growth.cpp
 * @brief AR-07 DT: Dynamic Heap Growth
 *
 * Cases:
 *   DT07-1 R1,R2: fill a segment to capacity, trigger growth — new segment
 *   DT07-2 R3:    SegmentDirectory.count incremented, descs[s].ready == 1
 *   DT07-3 R4:    shm name follows ub-{heap}-p{k}-{ds|dl|dh}-s{idx} convention
 *   DT07-4 R5:    multiple growth iterations — segment indices increment
 */
 
#include "uballoc.hpp"
#include "test_helpers.hpp"
 
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>
 
#include <sys/wait.h>
 
namespace {
 
using ShmProvider = uballoc::DistributedShmBackend<>::ShmProvider;
 
constexpr size_t M = 1024 * 1024;
 
// Compute the expected shm name for a (rank, bracket, seg_idx) tuple using
// the ShmProvider's convention (used as the oracle in DT07-3).
inline std::string expected_name(const std::string& heap_id, int rank, int bracket, int seg_idx) {
    int region_type = bracket + 1;  // DATA_SMALL=1, DATA_LARGE=2, DATA_HUGE=3
    return ShmProvider::shm_name(heap_id, rank, region_type, seg_idx);
}
 
// Drain a bracket by allocating until the segment_count bumps, then return
// the new seg_count. Used by the small/large growth tests.
template <typename Bracket>
uint32_t fill_until_growth(uballoc::HeapShared<Bracket>* shared,
                           size_t alloc_size, size_t slab_size) {
    uint32_t seg_before = shared->segment_count_.load(std::memory_order_acquire);
    size_t blocks_per_slab = slab_size / alloc_size;
    size_t sps = uballoc::SegmentLayout<Bracket>::virtual_slabs_per_segment();
    size_t target = sps * blocks_per_slab + 4;
 
    for (size_t i = 0; i < target; ++i) {
        void* p = uballoc::malloc(alloc_size);
        assert(p);
        std::memset(p, static_cast<int>(i & 0xFF), alloc_size);
        uint32_t seg_now = shared->segment_count_.load(std::memory_order_acquire);
        if (seg_now > seg_before) {
            return seg_now;
        }
    }
    return shared->segment_count_.load(std::memory_order_acquire);
}
 
// -----------------------------------------------------------------------------
// DT07-1 R1,R2: fill a segment to capacity, trigger growth — new segment
// -----------------------------------------------------------------------------
void dt07_1_trigger_growth(const std::string& heap_id) {
    std::cout << "[DT07-1] fill small/large/huge -> growth triggers new segment..." << std::endl;
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        ar_set_heap_id(heap_id);
        ar_child_init();
 
        // Small bracket.
        {
            auto* shared = ar_alloc().small_shared();
            uint32_t before = shared->segment_count_.load(std::memory_order_acquire);
            uint32_t after = fill_until_growth<uballoc::Small>(
                shared, 64, uballoc::Small::SLAB_SIZE);
            assert(after > before);
            std::cout << "  small: seg_count " << before << " -> " << after << std::endl;
        }
 
        // Large bracket.
        {
            auto* shared = ar_alloc().large_shared();
            uint32_t before = shared->segment_count_.load(std::memory_order_acquire);
            uint32_t after = fill_until_growth<uballoc::Large>(
                shared, 32 * 1024, uballoc::Large::SLAB_SIZE);
            assert(after > before);
            std::cout << "  large: seg_count " << before << " -> " << after << std::endl;
        }
 
        // Huge bracket.
        {
            auto* shared = reinterpret_cast<uballoc::HugeShared*>(ar_alloc().huge_shared());
            uint32_t before = shared->segment_count_.load(std::memory_order_acquire);
            size_t slot = uballoc::HugeSize::SLAB_SIZE;
            // Allocate one beyond the per-segment slot count to force growth.
            size_t slots_per_seg = uballoc::HUGE_SLOTS_PER_SEGMENT;
            for (size_t i = 0; i < slots_per_seg + 1; ++i) {
                void* p = uballoc::malloc(slot);
                if (!p) break;
                std::memset(p, static_cast<int>(i & 0xFF), slot);
                uint32_t seg_now = shared->segment_count_.load(std::memory_order_acquire);
                if (seg_now > before) break;
            }
            uint32_t after = shared->segment_count_.load(std::memory_order_acquire);
            assert(after > before);
            std::cout << "  huge:  seg_count " << before << " -> " << after << std::endl;
        }
 
        _exit(0);
    }
    int st;
    waitpid(pid, &st, 0);
    check_child(st);
}
 
// -----------------------------------------------------------------------------
// DT07-2 R3: SegmentDirectory.count incremented, descs[s].ready == 1
// DT07-3 R4: shm name follows ub-{heap}-p{k}-{ds|dl|dh}-s{idx} convention
// DT07-4 R5: multiple growth iterations — segment indices increment
// -----------------------------------------------------------------------------
void dt07_2_3_4_directory_count_name_indices(const std::string& heap_id) {
    std::cout << "[DT07-2/3/4] directory.count, descs[s].ready, shm name, indices..." << std::endl;
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        ar_set_heap_id(heap_id);
        ar_child_init();
        ar_alloc().init_thread(0);
 
        auto& alloc = ar_alloc();
        int my_rank = uballoc::rank();
        const std::string& hid = alloc.backend().heap_id_;
 
        // For Small: force 2 explicit growth calls, then verify directory.
        {
            int bracket = 0;
            // Allocate enough to trigger at least 1 natural growth (so seg 0
            // exists with ready=1), then call grow_segment() explicitly twice
            // to create segs 1 and 2.
            auto* shared = alloc.small_shared();
            uint32_t seg0 = shared->segment_count_.load(std::memory_order_acquire);
            (void)seg0;
            // Force-fill seg 0 by exhausting bump.
            size_t sps = uballoc::SegmentLayout<uballoc::Small>::virtual_slabs_per_segment();
            size_t slab_size = uballoc::Small::SLAB_SIZE;
            size_t blocks_per_slab = slab_size / 64;
            size_t target = sps * blocks_per_slab + 4;
            std::vector<void*> ptrs;
            for (size_t i = 0; i < target; ++i) {
                void* p = uballoc::malloc(64);
                if (!p) break;
                ptrs.push_back(p);
                uint32_t now = shared->segment_count_.load(std::memory_order_acquire);
                if (now >= 1) break;
            }
 
            // Explicit growth: create 2 more segments.
            uint32_t before_explicit = shared->segment_count_.load(std::memory_order_acquire);
            assert(alloc.grow_segment(bracket));
            assert(alloc.grow_segment(bracket));
            uint32_t after_explicit = shared->segment_count_.load(std::memory_order_acquire);
            assert(after_explicit == before_explicit + 2);
 
            auto* dir = alloc.backend().segment_directory(my_rank, bracket);
            assert(dir != nullptr);
            uint32_t dir_count = dir->count.load(std::memory_order_acquire);
            assert(dir_count == after_explicit);
 
            // Verify each segment's ready flag and shm name.
            for (uint32_t s = 0; s < dir_count; ++s) {
                uint32_t ready = dir->descs[s].ready.load(std::memory_order_acquire);
                assert(ready == 1u);
                std::string expected = expected_name(hid, my_rank, bracket, static_cast<int>(s));
                std::string actual(dir->descs[s].name);
                assert(actual == expected);
            }
 
            // DT07-4: indices 0..(dir_count-1) all present and increment by 1.
            for (uint32_t s = 0; s < dir_count; ++s) {
                std::string expected = expected_name(hid, my_rank, bracket, static_cast<int>(s));
                std::string actual(dir->descs[s].name);
                // The -s{idx} suffix is present when region_type > 0 AND seg_idx > 0.
                if (s > 0) {
                    assert(actual.find("-s" + std::to_string(s)) != std::string::npos);
                } else {
                    // seg_idx == 0: no -s0 suffix (per shm_name convention).
                    assert(actual.find("-s0") == std::string::npos);
                }
            }
            std::cout << "  small: dir_count=" << dir_count
                      << " names match convention, indices 0.." << (dir_count - 1) << std::endl;
 
            for (void* p : ptrs) uballoc::free(p);
        }
 
        // Large: same checks.
        {
            int bracket = 1;
            auto* shared = alloc.large_shared();
            size_t sps = uballoc::SegmentLayout<uballoc::Large>::virtual_slabs_per_segment();
            size_t slab_size = uballoc::Large::SLAB_SIZE;
            size_t blocks_per_slab = slab_size / (32 * 1024);
            size_t target = sps * blocks_per_slab + 4;
            std::vector<void*> ptrs;
            for (size_t i = 0; i < target; ++i) {
                void* p = uballoc::malloc(32 * 1024);
                if (!p) break;
                ptrs.push_back(p);
                uint32_t now = shared->segment_count_.load(std::memory_order_acquire);
                if (now >= 1) break;
            }
 
            uint32_t before = shared->segment_count_.load(std::memory_order_acquire);
            assert(alloc.grow_segment(bracket));
            uint32_t after = shared->segment_count_.load(std::memory_order_acquire);
            assert(after == before + 1);
 
            auto* dir = alloc.backend().segment_directory(my_rank, bracket);
            assert(dir != nullptr);
            uint32_t dir_count = dir->count.load(std::memory_order_acquire);
            assert(dir_count == after);
            for (uint32_t s = 0; s < dir_count; ++s) {
                assert(dir->descs[s].ready.load(std::memory_order_acquire) == 1u);
                std::string expected = expected_name(hid, my_rank, bracket, static_cast<int>(s));
                std::string actual(dir->descs[s].name);
                assert(actual == expected);
            }
            std::cout << "  large: dir_count=" << dir_count << " names match" << std::endl;
 
            for (void* p : ptrs) uballoc::free(p);
        }
 
        _exit(0);
    }
    int st;
    waitpid(pid, &st, 0);
    check_child(st);
}
 
}  // namespace
 
int main() {
    std::string id = ar_heap_id("ar07");
    cleanup_all_distributed_shms(8, id);
 
    ar_pre_case_cleanup(id);
    dt07_1_trigger_growth(id);
    ar_pre_case_cleanup(id);
    dt07_2_3_4_directory_count_name_indices(id);
 
    cleanup_all_distributed_shms(8, id);
    std::cout << "\n=== AR-07 DT passed ===" << std::endl;
    return 0;
}
