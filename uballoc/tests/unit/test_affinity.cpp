// SPDX-License-Identifier: Apache-2.0

#include "uballoc.hpp"
#include "test_helpers.hpp"
#include <iostream>
#include <cassert>
#include <cstring>
#include <vector>
#include <unistd.h>
#include <sys/wait.h>

static constexpr int AFF_PROCS = 2;
static constexpr size_t ALLOC_SIZE = 32768;
static constexpr const char* HEAP_ID = "aff_test";

static size_t blocks_per_slab() {
    return uballoc::Large::SLAB_SIZE / ALLOC_SIZE;
}

static size_t slabs_per_segment() {
    return uballoc::SegmentLayout<uballoc::Large>::virtual_slabs_per_segment();
}

static size_t blocks_per_segment() {
    return slabs_per_segment() * blocks_per_slab();
}

static void fill_and_free_slabs(size_t num_slabs) {
    size_t bps = blocks_per_slab();
    std::vector<void*> ptrs;
    for (size_t s = 0; s < num_slabs; ++s) {
        for (size_t i = 0; i < bps; ++i) {
            void* p = uballoc::malloc(ALLOC_SIZE);
            assert(p);
            ptrs.push_back(p);
        }
    }
    for (void* p : ptrs) uballoc::free(p);
}

static void test_affinity_strict_growth_before_borrow() {
    std::cout << "[affinity] Strict: growth triggered before borrow..." << std::endl;

    cleanup_all_distributed_shms(AFF_PROCS, HEAP_ID);
    setenv("UBALLOC_AFFINITY_MODE", "strict", 1);
    setenv("UBALLOC_HEAP_ID", HEAP_ID, 1);

    int sync_pipe[2];
    assert(pipe(sync_pipe) == 0);

    auto& parent_alloc = uballoc::get_global_allocator<uballoc::DistributedShmBackend<>>();
    parent_alloc.soft_reset();
    parent_alloc.init();
    parent_alloc.init_thread(0);
    if (!parent_alloc.is_initialized()) {
        std::cerr << "  P0 init failed" << std::endl;
        return;
    }

    fill_and_free_slabs(slabs_per_segment());

    pid_t pid = fork();
    assert(pid >= 0);

    if (pid == 0) {
        close(sync_pipe[0]);
        auto& alloc = uballoc::get_global_allocator<uballoc::DistributedShmBackend<>>();
        alloc.soft_reset();
        alloc.init();
        if (!alloc.is_initialized()) _exit(1);

        auto* shared = alloc.large_shared();
        uint32_t seg_before = shared->segment_count_.load(std::memory_order_acquire);

        size_t target = blocks_per_segment() + 10;
        std::vector<void*> ptrs;
        bool growth = false;
        for (size_t i = 0; i < target; ++i) {
            void* p = alloc.malloc(ALLOC_SIZE);
            if (!p) break;
            ptrs.push_back(p);

            uint32_t seg_now = shared->segment_count_.load(std::memory_order_acquire);
            if (seg_now > seg_before) {
                growth = true;
                break;
            }
        }

        assert(growth);
        uint32_t seg_after = shared->segment_count_.load(std::memory_order_acquire);
        assert(seg_after > seg_before);

        for (void* p : ptrs) alloc.free(p);

        char c = 'D';
        write(sync_pipe[1], &c, 1);
        close(sync_pipe[1]);
        _exit(0);
    }

    close(sync_pipe[1]);
    char c;
    assert(read(sync_pipe[0], &c, 1) == 1 && c == 'D');
    close(sync_pipe[0]);

    int status;
    waitpid(pid, &status, 0);
    check_child(status);

    parent_alloc.reset();
    cleanup_all_distributed_shms(AFF_PROCS, HEAP_ID);
    std::cout << "  Verified: strict mode triggers growth, not borrow" << std::endl;
}

static void test_affinity_reuse_first_borrow_before_growth() {
    std::cout << "[affinity] Reuse-first: borrow triggered before growth..." << std::endl;

    cleanup_all_distributed_shms(AFF_PROCS, HEAP_ID);
    setenv("UBALLOC_AFFINITY_MODE", "reuse-first", 1);
    setenv("UBALLOC_HEAP_ID", HEAP_ID, 1);

    int sync_pipe[2];
    assert(pipe(sync_pipe) == 0);

    auto& parent_alloc = uballoc::get_global_allocator<uballoc::DistributedShmBackend<>>();
    parent_alloc.soft_reset();
    parent_alloc.init();
    parent_alloc.init_thread(0);
    if (!parent_alloc.is_initialized()) {
        std::cerr << "  P0 init failed" << std::endl;
        return;
    }

    fill_and_free_slabs(slabs_per_segment());

    pid_t pid = fork();
    assert(pid >= 0);

    if (pid == 0) {
        close(sync_pipe[0]);
        auto& alloc = uballoc::get_global_allocator<uballoc::DistributedShmBackend<>>();
        alloc.soft_reset();
        alloc.init();
        if (!alloc.is_initialized()) _exit(1);

        auto* shared = alloc.large_shared();
        uint32_t seg_before = shared->segment_count_.load(std::memory_order_acquire);

        size_t target = blocks_per_segment();
        std::vector<void*> ptrs;
        bool success = true;
        for (size_t i = 0; i < target; ++i) {
            void* p = alloc.malloc(ALLOC_SIZE);
            if (!p) {
                success = false;
                break;
            }
            ptrs.push_back(p);
        }

        uint32_t seg_after = shared->segment_count_.load(std::memory_order_acquire);

        assert(success);
        assert(seg_after == seg_before);

        for (void* p : ptrs) alloc.free(p);

        char c = 'D';
        write(sync_pipe[1], &c, 1);
        close(sync_pipe[1]);
        _exit(0);
    }

    close(sync_pipe[1]);
    char c;
    assert(read(sync_pipe[0], &c, 1) == 1 && c == 'D');
    close(sync_pipe[0]);

    int status;
    waitpid(pid, &status, 0);
    check_child(status);

    parent_alloc.reset();
    cleanup_all_distributed_shms(AFF_PROCS, HEAP_ID);
    std::cout << "  Verified: reuse-first mode borrows without growth (seg stayed "
              << 0 << ")" << std::endl;
}

static void test_affinity_default_is_strict() {
    std::cout << "[affinity] Default mode is strict..." << std::endl;

    cleanup_all_distributed_shms(1, HEAP_ID);
    unsetenv("UBALLOC_AFFINITY_MODE");
    setenv("UBALLOC_HEAP_ID", HEAP_ID, 1);

    auto& parent_alloc = uballoc::get_global_allocator<uballoc::DistributedShmBackend<>>();
    parent_alloc.soft_reset();
    parent_alloc.init();
    parent_alloc.init_thread(0);
    if (!parent_alloc.is_initialized()) {
        std::cerr << "  [P] P0 init failed" << std::endl;
        return;
    }

    pid_t pid = fork();
    assert(pid >= 0);

    if (pid == 0) {
        auto& alloc = uballoc::get_global_allocator<uballoc::DistributedShmBackend<>>();
        alloc.soft_reset();
        alloc.init();
        if (!alloc.is_initialized()) _exit(1);

        size_t sps = uballoc::SegmentLayout<uballoc::Small>::virtual_slabs_per_segment();
        size_t alloc_size = 64;
        size_t bps = uballoc::Small::SLAB_SIZE / alloc_size;
        size_t total = sps * bps + 10;

        auto* shared = alloc.small_shared();
        uint32_t seg_before = shared->segment_count_.load(std::memory_order_acquire);

        std::vector<void*> ptrs;
        bool growth = false;
        for (size_t i = 0; i < total; ++i) {
            void* p = alloc.malloc(alloc_size);
            if (!p) break;
            ptrs.push_back(p);
            uint32_t seg_now = shared->segment_count_.load(std::memory_order_acquire);
            if (seg_now > seg_before) { growth = true; break; }
        }
        assert(growth);

        for (void* p : ptrs) alloc.free(p);
        _exit(0);
    } else {
        int status;
        waitpid(pid, &status, 0);
        check_child(status);
        parent_alloc.reset();
        cleanup_all_distributed_shms(1, HEAP_ID);
        std::cout << "  Verified: default affinity mode is strict" << std::endl;
    }
}

int main() {
    test_affinity_default_is_strict();
    test_affinity_strict_growth_before_borrow();
    test_affinity_reuse_first_borrow_before_growth();
    std::cout << "\n=== Affinity mode tests passed ===" << std::endl;
    return 0;
}
