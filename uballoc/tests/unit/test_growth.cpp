// SPDX-License-Identifier: Apache-2.0

#include "uballoc.hpp"
#include "test_helpers.hpp"
#include <iostream>
#include <cassert>
#include <cstring>
#include <vector>
#include <unistd.h>
#include <sys/wait.h>

// On UBSE, the priority-101/102 constructors run before main(). If
// UBALLOC_HEAP_ID is set at process start, they create the VA reservation
// and enable uffd. If set later (via setenv), they defer and init() must
// do the work. Forking BEFORE any init means the child inherits nothing
// and tries to init the UBSE runtime from scratch — which crashes.
//
// The fix: parent inits as P0 before fork (matching test_main.cpp pattern).
// Child inherits the UBSE runtime, attaches as non-P0, and works.

static void test_growth_small_single_process() {
    std::cout << "[growth] Small: exhaust segment 0, verify segment 1 created..." << std::endl;

    pid_t pid = fork();
    assert(pid >= 0);

    if (pid == 0) {
        using Backend = uballoc::DistributedShmBackend<>;
        auto& alloc = uballoc::get_global_allocator<Backend>();
        alloc.soft_reset();
        alloc.init();

        if (!alloc.is_initialized()) {
            std::cerr << "  init failed" << std::endl;
            _exit(1);
        }

        auto* shared = alloc.small_shared();
        uint32_t seg_before = shared->segment_count_.load(std::memory_order_acquire);
        assert(seg_before == 0);

        size_t sps = uballoc::SegmentLayout<uballoc::Small>::virtual_slabs_per_segment();
        size_t slab_size = uballoc::Small::SLAB_SIZE;
        size_t alloc_size = 64;
        size_t blocks_per_slab = slab_size / alloc_size;

        size_t total_to_alloc = sps * blocks_per_slab + 10;

        std::vector<void*> ptrs;
        bool growth_triggered = false;
        for (size_t i = 0; i < total_to_alloc; ++i) {
            void* p = alloc.malloc(alloc_size);
            if (!p) {
                std::cerr << "  malloc failed at i=" << i << std::endl;
                break;
            }
            memset(p, static_cast<int>(i & 0xFF), alloc_size);
            ptrs.push_back(p);

            uint32_t seg_now = shared->segment_count_.load(std::memory_order_acquire);
            if (seg_now > seg_before) {
                growth_triggered = true;
                std::cout << "  Growth triggered at i=" << i
                          << " seg_count=" << seg_now << std::endl;
                break;
            }
        }

        assert(growth_triggered);
        uint32_t seg_after = shared->segment_count_.load(std::memory_order_acquire);
        assert(seg_after > seg_before);

        for (void* p : ptrs) {
            alloc.free(p);
        }

        _exit(0);
    } else {
        int status;
        waitpid(pid, &status, 0);
        check_child(status);
        std::cout << "  Verified: small segment growth triggered on bump exhaustion" << std::endl;
    }
}

static void test_growth_large_single_process() {
    std::cout << "[growth] Large: exhaust segment 0, verify segment 1 created..." << std::endl;

    pid_t pid = fork();
    assert(pid >= 0);

    if (pid == 0) {
        using Backend = uballoc::DistributedShmBackend<>;
        auto& alloc = uballoc::get_global_allocator<Backend>();
        alloc.soft_reset();
        alloc.init();

        if (!alloc.is_initialized()) {
            std::cerr << "  init failed" << std::endl;
            _exit(1);
        }

        auto* shared = alloc.large_shared();
        uint32_t seg_before = shared->segment_count_.load(std::memory_order_acquire);
        assert(seg_before == 0);

        size_t sps = uballoc::SegmentLayout<uballoc::Large>::virtual_slabs_per_segment();
        size_t slab_size = uballoc::Large::SLAB_SIZE;
        size_t alloc_size = 32768;
        size_t blocks_per_slab = slab_size / alloc_size;

        size_t total_to_alloc = sps * blocks_per_slab + 10;

        std::vector<void*> ptrs;
        bool growth_triggered = false;
        for (size_t i = 0; i < total_to_alloc; ++i) {
            void* p = alloc.malloc(alloc_size);
            if (!p) {
                std::cerr << "  malloc failed at i=" << i << std::endl;
                break;
            }
            memset(p, static_cast<int>(i & 0xFF), alloc_size);
            ptrs.push_back(p);

            uint32_t seg_now = shared->segment_count_.load(std::memory_order_acquire);
            if (seg_now > seg_before) {
                growth_triggered = true;
                std::cout << "  Growth triggered at i=" << i
                          << " seg_count=" << seg_now << std::endl;
                break;
            }
        }

        assert(growth_triggered);
        uint32_t seg_after = shared->segment_count_.load(std::memory_order_acquire);
        assert(seg_after > seg_before);

        for (void* p : ptrs) {
            alloc.free(p);
        }

        _exit(0);
    } else {
        int status;
        waitpid(pid, &status, 0);
        check_child(status);
        std::cout << "  Verified: large segment growth triggered on bump exhaustion" << std::endl;
    }
}

static void test_growth_huge_single_process() {
    std::cout << "[growth] Huge: exhaust segment 0 slots, verify segment 1 created..." << std::endl;

    pid_t pid = fork();
    assert(pid >= 0);

    if (pid == 0) {
        using Backend = uballoc::DistributedShmBackend<>;
        auto& alloc = uballoc::get_global_allocator<Backend>();
        alloc.soft_reset();
        alloc.init();

        if (!alloc.is_initialized()) {
            std::cerr << "  init failed" << std::endl;
            _exit(1);
        }

        auto* shared = reinterpret_cast<uballoc::HugeShared*>(
            alloc.huge_shared());
        uint32_t seg_before = shared->segment_count_.load(std::memory_order_acquire);
        assert(seg_before == 0);

        size_t slots_per_seg = uballoc::HUGE_SLOTS_PER_SEGMENT;
        size_t huge_slot_size = uballoc::HugeSize::SLAB_SIZE;

        std::vector<void*> ptrs;
        bool growth_triggered = false;
        for (size_t i = 0; i < slots_per_seg + 2; ++i) {
            void* p = alloc.malloc(huge_slot_size);
            if (!p) {
                std::cerr << "  malloc failed at i=" << i << std::endl;
                break;
            }
            memset(p, static_cast<int>(i & 0xFF), huge_slot_size);
            ptrs.push_back(p);

            uint32_t seg_now = shared->segment_count_.load(std::memory_order_acquire);
            if (seg_now > seg_before) {
                growth_triggered = true;
                std::cout << "  Growth triggered at i=" << i
                          << " seg_count=" << seg_now << std::endl;
                break;
            }
        }

        assert(growth_triggered);
        uint32_t seg_after = shared->segment_count_.load(std::memory_order_acquire);
        assert(seg_after > seg_before);

        for (void* p : ptrs) {
            alloc.free(p);
        }

        _exit(0);
    } else {
        int status;
        waitpid(pid, &status, 0);
        check_child(status);
        std::cout << "  Verified: huge segment growth triggered on slot exhaustion" << std::endl;
    }
}

static void test_growth_data_integrity_after_growth() {
    std::cout << "[growth] Data integrity: write before and after growth, verify all..." << std::endl;

    pid_t pid = fork();
    assert(pid >= 0);

    if (pid == 0) {
        using Backend = uballoc::DistributedShmBackend<>;
        auto& alloc = uballoc::get_global_allocator<Backend>();
        alloc.soft_reset();
        alloc.init();

        if (!alloc.is_initialized()) {
            _exit(1);
        }

        size_t alloc_size = 64;
        std::vector<std::pair<void*, uint8_t>> ptrs;

        for (int i = 0; i < 100; ++i) {
            void* p = alloc.malloc(alloc_size);
            if (!p) {
                std::cerr << "  malloc failed at i=" << i << std::endl;
                break;
            }
            uint8_t pattern = static_cast<uint8_t>(0xA0 | (i & 0x0F));
            memset(p, pattern, alloc_size);
            ptrs.push_back({p, pattern});
        }

        bool all_ok = true;
        for (auto& [p, pattern] : ptrs) {
            for (size_t b = 0; b < alloc_size; ++b) {
                if (static_cast<uint8_t*>(p)[b] != pattern) {
                    std::cerr << "  VERIFY FAIL at pattern=" << (int)pattern
                              << " byte=" << b << " got=" << (int)static_cast<uint8_t*>(p)[b] << std::endl;
                    all_ok = false;
                    break;
                }
            }
        }
        assert(all_ok);

        for (auto& [p, _] : ptrs) {
            alloc.free(p);
        }

        _exit(0);
    } else {
        int status;
        waitpid(pid, &status, 0);
        check_child(status);
        std::cout << "  Verified: all data intact across growth boundary" << std::endl;
    }
}

static void test_growth_free_after_growth() {
    std::cout << "[growth] Free after growth: alloc across segments, free all..." << std::endl;

    pid_t pid = fork();
    assert(pid >= 0);

    if (pid == 0) {
        using Backend = uballoc::DistributedShmBackend<>;
        auto& alloc = uballoc::get_global_allocator<Backend>();
        alloc.soft_reset();
        alloc.init();

        if (!alloc.is_initialized()) {
            _exit(1);
        }

        size_t alloc_size = 64;
        std::vector<void*> ptrs;

        for (int i = 0; i < 200; ++i) {
            void* p = alloc.malloc(alloc_size);
            if (!p) {
                std::cerr << "  malloc failed at i=" << i << std::endl;
                break;
            }
            ptrs.push_back(p);
        }
        assert(ptrs.size() == 200);

        for (void* p : ptrs) {
            alloc.free(p);
        }

        void* p = alloc.malloc(alloc_size);
        assert(p != nullptr);
        memset(p, 0xAA, alloc_size);
        assert(static_cast<uint8_t*>(p)[0] == 0xAA);
        alloc.free(p);

        _exit(0);
    } else {
        int status;
        waitpid(pid, &status, 0);
        check_child(status);
        std::cout << "  Verified: free works correctly across grown segments" << std::endl;
    }
}

static void test_growth_atomic_flag_state() {
    std::cout << "[growth] Atomic flag: growing_ transitions 0->1->0 correctly..." << std::endl;

    pid_t pid = fork();
    assert(pid >= 0);

    if (pid == 0) {
        using Backend = uballoc::DistributedShmBackend<>;
        auto& alloc = uballoc::get_global_allocator<Backend>();
        alloc.soft_reset();
        alloc.init();

        if (!alloc.is_initialized()) {
            _exit(1);
        }

        auto* shared = alloc.small_shared();
        assert(shared->growing_.load(std::memory_order_relaxed) == 0);
        assert(shared->segment_count_.load(std::memory_order_relaxed) == 0);

        size_t sps = uballoc::SegmentLayout<uballoc::Small>::virtual_slabs_per_segment();
        size_t blocks_per_slab = uballoc::Small::SLAB_SIZE / 64;

        for (size_t i = 0; i < sps * blocks_per_slab + 1; ++i) {
            void* p = alloc.malloc(64);
            if (!p) break;
            alloc.free(p);
        }

        assert(shared->growing_.load(std::memory_order_relaxed) == 0);
        assert(shared->segment_count_.load(std::memory_order_relaxed) >= 1);

        _exit(0);
    } else {
        int status;
        waitpid(pid, &status, 0);
        check_child(status);
        std::cout << "  Verified: growing_ flag is 0 after growth completes" << std::endl;
    }
}

int main() {
    cleanup_all_distributed_shms(1, "gtest");
    setenv("UBALLOC_HEAP_ID", "gtest", 1);

    auto& parent_alloc = uballoc::get_global_allocator<uballoc::DistributedShmBackend<>>();
    parent_alloc.soft_reset();
    parent_alloc.init();
    parent_alloc.init_thread(0);
    if (!parent_alloc.is_initialized()) {
        std::cerr << "Parent init failed" << std::endl;
        return 1;
    }

    test_growth_small_single_process();
    test_growth_large_single_process();
    test_growth_huge_single_process();
    test_growth_data_integrity_after_growth();
    test_growth_free_after_growth();
    test_growth_atomic_flag_state();

    parent_alloc.reset();
    cleanup_all_distributed_shms(1, "gtest");
    std::cout << "\n=== Growth tests passed ===" << std::endl;
    return 0;
}
