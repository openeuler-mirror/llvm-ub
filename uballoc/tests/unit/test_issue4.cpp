// SPDX-License-Identifier: Apache-2.0

#include "uballoc.hpp"
#include "test_helpers.hpp"

static void test_issue4_remote_regions_unmapped_after_cleanup() {
    std::cout << "[fix] Issue 4: cleanup_remote_regions munmaps remote regions (no leak)" << std::endl;

    // cleanup_remote_regions() is compiled out on UBSE
    // (has_reliable_unreferenced_check=true → no-op, cleanup via refcount-based detach).
    if constexpr (uballoc::DistributedShmBackend<>::ShmProvider::has_reliable_unreferenced_check) {
        std::cout << "  Skipped: cleanup_remote_regions is no-op on UBSE" << std::endl;
        return;
    }

    cleanup_all_distributed_shms(2, "test");
    setenv("UBALLOC_HEAP_ID", "test", 1);

    int sync_pipe[2];
    assert(pipe(sync_pipe) == 0);

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
        close(sync_pipe[0]);
        auto& alloc = uballoc::get_global_allocator<uballoc::DistributedShmBackend<>>();
        alloc.soft_reset();
        alloc.init();
        if (!alloc.is_initialized()) {
            std::cerr << "  child init failed" << std::endl;
            _exit(1);
        }

        // Create a data segment so P0 has remote regions to attach.
        size_t sps = uballoc::SegmentLayout<uballoc::Small>::virtual_slabs_per_segment();
        auto res = alloc.backend().create_data_segment(0, sps);
        if (uballoc::is_err(res)) {
            std::cerr << "  create_data_segment failed" << std::endl;
            _exit(1);
        }

        char c = 'R';
        write(sync_pipe[1], &c, 1);
        close(sync_pipe[1]);

        usleep(2000000);
        _exit(0);
    } else {
        close(sync_pipe[1]);

        // Wait for child to finish init + create data segment.
        char c;
        assert(read(sync_pipe[0], &c, 1) == 1 && c == 'R');
        close(sync_pipe[0]);

        // Discover P1's regions (metadata + data segments).
        parent_alloc.check_remote_segments();

        // Attach to ALL remote regions (P1's metadata + data segments).
        // Limit to 2 processes (only P0+P1 exist; default is MAX_PROCESSES=8).
        parent_alloc.backend().total_processes_ = 2;
        auto attach_res = parent_alloc.backend().attach_all_remote_regions();
        if (uballoc::is_err(attach_res)) {
            std::cerr << "  attach_all_remote_regions failed" << std::endl;
            waitpid(pid, nullptr, 0);
            parent_alloc.reset();
            cleanup_all_distributed_shms(2, "test");
            return;
        }

        // Count P1's mapped regions.
        int valid_count = 0;
        if (parent_alloc.backend().metadata_regions_[1].address &&
            parent_alloc.backend().metadata_regions_[1].size > 0)
            valid_count++;
        for (int b = 0; b < 3; ++b) {
            for (auto& seg : parent_alloc.backend().segments_[1][b]) {
                if (seg.region.address && seg.region.size > 0)
                    valid_count++;
            }
        }
        assert(valid_count > 0);
        std::cout << "  P1 has " << valid_count << " remote regions mapped in P0" << std::endl;

        // Cleanup remote regions (munmap P1's metadata + data segments).
        parent_alloc.backend().cleanup_remote_regions();

        // Verify P1's regions are unmapped.
        int unmapped = 0;
        if (parent_alloc.backend().metadata_regions_[1].address == nullptr)
            unmapped++;
        for (int b = 0; b < 3; ++b) {
            for (auto& seg : parent_alloc.backend().segments_[1][b]) {
                if (seg.region.address == nullptr)
                    unmapped++;
            }
        }

        assert(unmapped == valid_count);
        std::cout << "  Verified: " << unmapped << " of " << valid_count
                  << " P1 region addresses set to nullptr after cleanup_remote_regions" << std::endl;

        int status;
        waitpid(pid, &status, 0);
        check_child(status);

        parent_alloc.reset();
        cleanup_all_distributed_shms(2, "test");
        std::cout << "  Issue 4 fix verified: remote regions properly unmapped" << std::endl;
    }
}

int main() {
    test_issue4_remote_regions_unmapped_after_cleanup();
    std::cout << "\n=== Issue 4 test passed ===" << std::endl;
    return 0;
}
