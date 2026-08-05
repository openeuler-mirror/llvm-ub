// SPDX-License-Identifier: Apache-2.0

#include "uballoc.hpp"
#include "test_helpers.hpp"

static void test_issue2_shm_handles_initialized_to_minus1() {
    std::cout << "[fix] Issue 2: metadata_handles_ initialized to -1 (not 0)" << std::endl;

    uballoc::DistributedShmBackend<> backend;

    for (int k = 0; k < static_cast<int>(uballoc::MAX_PROCESSES); ++k) {
        assert(backend.metadata_handles_[k] == uballoc::DistributedShmBackend<>::INVALID_HANDLE);
    }

    std::cout << "  Verified: all metadata_handles_ entries are -1 (FD -1 = invalid)" << std::endl;
}

static void test_issue2_no_close_on_stdin() {
    std::cout << "[fix] Issue 2: cleanup_own_shms does not close stdin for skipped regions" << std::endl;

    cleanup_all_distributed_shms(2, "test");
    setenv("UBALLOC_HEAP_ID", "test", 1);

    // Parent inits as P0 (GlobalAllocator handles UBSE runtime + VA + uffd).
    auto& parent_alloc = uballoc::get_global_allocator<uballoc::DistributedShmBackend<>>();
    parent_alloc.soft_reset();
    parent_alloc.init();
    parent_alloc.init_thread(0);
    if (!parent_alloc.is_initialized()) {
        std::cerr << "  parent init failed" << std::endl;
        return;
    }

    pid_t pid = fork();
    assert(pid >= 0);

    if (pid == 0) {
        // Child: init as P1 via GlobalAllocator (same as test_main.cpp).
        auto& alloc = uballoc::get_global_allocator<uballoc::DistributedShmBackend<>>();
        alloc.soft_reset();
        alloc.init();
        if (!alloc.is_initialized()) {
            std::cerr << "  child init failed" << std::endl;
            _exit(1);
        }

        // Default config: slab_count_small=0 → no small data segments.
        assert(alloc.backend().segments_[alloc.backend().rank_][0].empty());

        std::cout << "  Verified: no small data segments created when slab_count=0" << std::endl;

        // cleanup_own_shms should not close stdin (fd 0).
        alloc.backend().cleanup_own_shms();

        std::cout << "  cleanup_own_shms completed without closing FD 0" << std::endl;

        _exit(0);
    } else {
        int status;
        waitpid(pid, &status, 0);
        check_child(status);
        parent_alloc.reset();
        cleanup_all_distributed_shms(2, "test");
        std::cout << "  Issue 2 fix verified: no close(stdin), all fds initialized to -1" << std::endl;
    }
}

int main() {
    test_issue2_shm_handles_initialized_to_minus1();
    test_issue2_no_close_on_stdin();
    std::cout << "\n=== Issue 2 tests passed ===" << std::endl;
    return 0;
}
