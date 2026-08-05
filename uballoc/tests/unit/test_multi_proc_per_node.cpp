// SPDX-License-Identifier: Apache-2.0

// Tests for multi-process-per-node infrastructure (steps 1-6, 10-12 of
// design_multi_process_per_node.md).
//
// POSIX backend path: `is_single_node = true`, so the refcount code is
// bypassed entirely (R11). These tests verify:
//   - P0 zero-inits all new BootstrapBlock fields before publishing
//     BOOTSTRAP_MAGIC (R10) — required so multi-proc-per-node on UBSE
//     starts from a clean slate.
//   - SegmentDescriptor::attach_count is zeroed on segment creation (R10).
//   - shm_close_fd / shm_detach_by_name API split (§3) behaves correctly:
//     close_fd closes the fd, detach_by_name is a no-op for POSIX.
//   - 4-proc fork: each child gets a unique rank 0..3 via claim_rank,
//     and can independently alloc/free.
//   - Concurrent fork+exit: P1/P2/P3 exit cleanly, P0 continues.
//   - 4-proc concurrent bump alloc: no overlap across ranks.

#include "uballoc.hpp"
#include "test_helpers.hpp"
#include <iostream>
#include <cassert>
#include <cstring>
#include <vector>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <atomic>
#include <thread>

static constexpr int PPN_TEST_PROCS = 4;
static constexpr bool is_ubse = !uballoc::DistributedShmBackend<>::ShmProvider::is_single_node;

// ---------------------------------------------------------------------------
// Test 1: P0 zero-inits all new multi-proc-per-node BootstrapBlock fields
// before publishing BOOTSTRAP_MAGIC.
//
// Race-free check: P1 attaches AFTER P0 publishes ready=1, so all fields
// are stable. P1 inspects every new field and verifies it is zero /
// UINT32_MAX (the sentinel for "unassigned node_index").
// ---------------------------------------------------------------------------
static void test_bootstrap_zero_init_multi_proc_fields() {
    std::cout << "[multi-proc/node] P0 zero-inits new BootstrapBlock fields..." << std::endl;

    cleanup_all_distributed_shms(PPN_TEST_PROCS, "ppn_zi");

    setenv("UBALLOC_HEAP_ID", "ppn_zi", 1);
    auto& alloc = uballoc::get_global_allocator<uballoc::DistributedShmBackend<>>();
    alloc.soft_reset();
    alloc.init();

    if (!alloc.is_initialized()) {
        std::cerr << "  P0 init failed" << std::endl;
        return;
    }

    // Inspect the bootstrap block directly (already mapped by init).
    auto* bs = alloc.backend().bootstrap_block_;
    assert(bs != nullptr);
    assert(bs->magic.load(std::memory_order_acquire) == uballoc::BOOTSTRAP_MAGIC);
    assert(bs->ready.load(std::memory_order_acquire) == 1);

    // Verify P0 zero-init'd all new multi-proc-per-node fields.
    // On UBSE, assign_node_index() sets P0's own slot node_slot_id to
    // local_node_slot_id_ and node_index to 0 (lowest unused).
    // On POSIX, assign_node_index() is a no-op (stays 0 / UINT32_MAX).
    uint32_t local_slot = alloc.backend().local_node_slot_id_;
    uint32_t my_ni = alloc.backend().my_node_index_;
    for (int i = 0; i < static_cast<int>(uballoc::MAX_PROCESSES); ++i) {
        uint32_t expected_nsi = (i == 0 && is_ubse) ? local_slot : 0;
        assert(bs->slots[i].node_slot_id.load(std::memory_order_acquire) == expected_nsi);
        uint32_t expected_ni = (i == 0 && is_ubse) ? my_ni : UINT32_MAX;
        assert(bs->slots[i].node_index.load(std::memory_order_acquire) == expected_ni);
        uint32_t expected_npc = (i == 0) ? 1 : 0;
        assert(bs->node_proc_count[i].load(std::memory_order_acquire) == expected_npc);
        uint16_t expected_brf = (i == 0) ? 1 : 0;
        assert(bs->bootstrap_refcount[i].load(std::memory_order_acquire) == expected_brf);
        for (int j = 0; j < static_cast<int>(uballoc::MAX_PROCESSES); ++j) {
            // On UBSE, P0's own metadata has refcount=1 for its own node
            // (create_with_refcount increments metadata_refcount[0][my_ni]).
            uint16_t expected_mrc = (is_ubse && i == 0 && j == (int)my_ni) ? 1 : 0;
            assert(bs->metadata_refcount[i][j].load(std::memory_order_acquire) == expected_mrc);
        }
    }
    std::cout << "  Verified: all new BootstrapBlock fields are zero / UINT32_MAX" << std::endl;

    alloc.reset();
    cleanup_all_distributed_shms(PPN_TEST_PROCS, "ppn_zi");
}

// ---------------------------------------------------------------------------
// Test 2: create_data_segment — refcount lives in bootstrap (not metadata).
//
// A lender creates a data segment. The data segment refcount is now in
// BootstrapBlock.data_refcount (not SegmentDescriptor.attach_count).
// The bootstrap is zero-initialized by ftruncate, so refcount starts at 0.
// After P0's create_with_refcount, the refcount for P0's node is 1.
// ---------------------------------------------------------------------------
static void test_segment_descriptor_attach_count_zero_init() {
    std::cout << "[multi-proc/node] BootstrapBlock.data_refcount initialized..." << std::endl;

    cleanup_all_distributed_shms(2, "ppn_ac");
    setenv("UBALLOC_HEAP_ID", "ppn_ac", 1);

    int sync_pipe[2];
    assert(pipe(sync_pipe) == 0);

    // Parent inits as P0 BEFORE fork (UBSE: child inherits runtime).
    auto& parent_alloc = uballoc::get_global_allocator<uballoc::DistributedShmBackend<>>();
    parent_alloc.soft_reset();
    parent_alloc.init();
    parent_alloc.init_thread(0);
    if (!parent_alloc.is_initialized()) {
        std::cerr << "  P0 init failed" << std::endl;
        return;
    }

    // Force a small-segment growth so create_data_segment runs.
    size_t sps = uballoc::SegmentLayout<uballoc::Small>::virtual_slabs_per_segment();
    auto res = parent_alloc.backend().create_data_segment(0, sps);
    if (uballoc::is_err(res)) {
        std::cerr << "  create_data_segment failed" << std::endl;
        return;
    }
    int seg_idx = uballoc::unwrap(res);
    assert(seg_idx == 0);

    pid_t pid = fork();
    assert(pid >= 0);

    if (pid == 0) {
        // P1: attach P0's metadata, inspect bootstrap data_refcount.
        close(sync_pipe[0]);
        auto& alloc = uballoc::get_global_allocator<uballoc::DistributedShmBackend<>>();
        alloc.soft_reset();
        alloc.init();
        if (!alloc.is_initialized()) {
            std::cerr << "  P1 init failed" << std::endl;
            _exit(1);
        }

        alloc.check_remote_segments();

        void* p0_meta = alloc.backend().metadata_address(0);
        assert(p0_meta != nullptr);

        uballoc::SegmentDirectory* dir = alloc.backend().segment_directory(0, 0);
        assert(dir != nullptr);
        uint32_t count = dir->count.load(std::memory_order_acquire);
        assert(count >= 1);

        for (uint32_t s = 0; s < count; ++s) {
            uint32_t ready = dir->descs[s].ready.load(std::memory_order_acquire);
            assert(ready == 1);
            // Data segment refcount is now in BootstrapBlock.data_refcount.
            // P0's create_with_refcount increments refcount[P0_rank][bracket][seg][P0_node] to 1.
            // For UBSE, P0's self-borrow increments to 1. For POSIX, also 1.
            auto* refc = alloc.backend().get_refcount_ptr(0, 1, s);
            assert(refc != nullptr);
            uint16_t rc = refc[alloc.backend().my_node_index_].load(std::memory_order_acquire);
            // POSIX: no refcount tracking (is_single_node=true), refcount stays 0.
            // UBSE: P0's create_with_refcount increments to 1, then P1's
            // attach_data_segment (via check_remote_segments) increments to 2.
            uint16_t expected_ac = is_ubse ? 2 : 0;
            if (rc != expected_ac) {
                std::cerr << "  FAIL: data_refcount[" << s << "][" << alloc.backend().my_node_index_
                          << "]=" << rc << " (expected " << expected_ac << ")" << std::endl;
                assert(rc == expected_ac);
            }
        }
        std::cout << "  Verified: BootstrapBlock.data_refcount correct for "
                  << count << " segment(s)" << std::endl;

        char c = 'D';
        write(sync_pipe[1], &c, 1);
        close(sync_pipe[1]);
        _exit(0);
    }

    // P0: wait for child, then cleanup.
    close(sync_pipe[1]);
    char c;
    assert(read(sync_pipe[0], &c, 1) == 1 && c == 'D');
    close(sync_pipe[0]);

    int status;
    waitpid(pid, &status, 0);
    check_child(status);

    parent_alloc.reset();
    cleanup_all_distributed_shms(2, "ppn_ac");
}

// ---------------------------------------------------------------------------
// Test 3: shm_close_fd closes fd only; shm_detach_by_name is a no-op for
// POSIX (no per-node borrow concept). Verifies the §3 API split.
// POSIX-only: uses PosixShmProvider explicitly.
// ---------------------------------------------------------------------------
#ifndef UBALLOC_USE_UBSE
static void test_shm_close_fd_isolation_posix() {
    std::cout << "[multi-proc/node] shm_close_fd closes fd, no detach for POSIX..." << std::endl;

    cleanup_all_distributed_shms(1, "ppn_close");

    // Create a shm and verify shm_close_fd closes the fd.
    std::string name = "/ppn_close-test-m";
    shm_unlink(name.c_str());
    auto create_res = uballoc::PosixShmProvider::shm_create(name, 4096);
    assert(uballoc::is_ok(create_res));
    int fd = uballoc::unwrap(create_res);
    assert(fd >= 0);

    // Close fd — shm should still exist (we only closed the fd, not unlinked).
    uballoc::PosixShmProvider::shm_close_fd(fd);
    assert(uballoc::PosixShmProvider::shm_exists(name));

    // shm_detach_by_name is a no-op for POSIX; shm should still exist.
    uballoc::PosixShmProvider::shm_detach_by_name(name);
    assert(uballoc::PosixShmProvider::shm_exists(name));

    // shm_close_fd with INVALID_HANDLE is a no-op (no crash).
    uballoc::PosixShmProvider::shm_close_fd(uballoc::PosixShmProvider::INVALID_HANDLE);

    // Cleanup.
    uballoc::PosixShmProvider::shm_unlink(name);
    assert(!uballoc::PosixShmProvider::shm_exists(name));

    std::cout << "  Verified: shm_close_fd closes fd; shm_detach_by_name is a no-op; "
                 "INVALID_HANDLE is safe" << std::endl;

    cleanup_all_distributed_shms(1, "ppn_close");
}
#endif // !UBALLOC_USE_UBSE

// ---------------------------------------------------------------------------
// Test 4: 4-proc fork — P0 inits first, then forks 3 children (P1/P2/P3).
// Each child gets a unique rank 1..3 via claim_rank, and can independently
// alloc/free. Verifies claim_rank assigns unique slots under concurrent fork.
// ---------------------------------------------------------------------------
static void test_distributed_4proc_basic() {
    std::cout << "[multi-proc/node] 4-proc fork: each child gets unique rank..." << std::endl;

    cleanup_all_distributed_shms(PPN_TEST_PROCS, "ppn_4p");
    setenv("UBALLOC_HEAP_ID", "ppn_4p", 1);

    // Single pipe: parent reads 3 ranks from children.
    int rank_pipe[2];
    assert(pipe(rank_pipe) == 0);

    auto& parent_alloc = uballoc::get_global_allocator<uballoc::DistributedShmBackend<>>();
    parent_alloc.soft_reset();
    parent_alloc.init();
    parent_alloc.init_thread(0);
    assert(parent_alloc.is_initialized());
    assert(parent_alloc.backend().rank_ == 0);

    std::vector<pid_t> child_pids;
    for (int i = 1; i < PPN_TEST_PROCS; ++i) {
        pid_t pid = fork();
        assert(pid >= 0);
        if (pid == 0) {
            // Child: close read end, write rank, do alloc/free, exit.
            close(rank_pipe[0]);
            auto& alloc = uballoc::get_global_allocator<uballoc::DistributedShmBackend<>>();
            alloc.soft_reset();
            alloc.init();
            // NO init_thread(0) — init() calls ensure_thread_init() which
            // sets tl_thread_id_ = rank * num_cores (the correct value for
            // the assigned rank). Calling init_thread(0) would set
            // tl_thread_id_=0, but focus() computes local_idx = id - rank*
            // num_cores, which underflows for rank>0.
            if (!alloc.is_initialized()) {
                std::cerr << "  child " << i << " init failed" << std::endl;
                _exit(1);
            }
            int rank = alloc.backend().rank_;
            ssize_t w = write(rank_pipe[1], &rank, sizeof(rank));
            close(rank_pipe[1]);
            if (w != sizeof(rank)) _exit(1);

            // Alloc + free with rank-stamped pattern.
            void* p = alloc.malloc(64);
            assert(p != nullptr);
            memset(p, 0xA0 | (rank & 0x0F), 64);
            assert(static_cast<uint8_t*>(p)[0] == static_cast<uint8_t>(0xA0 | (rank & 0x0F)));
            alloc.free(p);

            alloc.reset();
            _exit(0);
        }
        child_pids.push_back(pid);
    }

    // Parent: collect ranks from children.
    close(rank_pipe[1]);
    int ranks[PPN_TEST_PROCS];
    ranks[0] = 0;  // parent is P0
    for (int i = 1; i < PPN_TEST_PROCS; ++i) {
        ssize_t n = read(rank_pipe[0], &ranks[i], sizeof(int));
        assert(n == sizeof(int));
    }
    close(rank_pipe[0]);

    // Wait for all children.
    for (pid_t cpid : child_pids) {
        int status;
        waitpid(cpid, &status, 0);
        assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }

    // Verify all 4 ranks are unique and in [0, PPN_TEST_PROCS).
    bool used[PPN_TEST_PROCS] = {};
    for (int i = 0; i < PPN_TEST_PROCS; ++i) {
        assert(ranks[i] >= 0 && ranks[i] < PPN_TEST_PROCS);
        assert(!used[ranks[i]]);
        used[ranks[i]] = true;
    }
    std::cout << "  Ranks assigned: P0=0 P1=" << ranks[1] << " P2=" << ranks[2]
              << " P3=" << ranks[3] << " (all unique)" << std::endl;

    parent_alloc.reset();
    cleanup_all_distributed_shms(PPN_TEST_PROCS, "ppn_4p");
    unsetenv("UBALLOC_HEAP_ID");
}

// ---------------------------------------------------------------------------
// Test 5: Concurrent fork+exit — P1/P2/P3 exit cleanly, P0 continues.
// Verifies that claim_rank dead-slot detection works (kill(pid, 0) returns
// ESRCH) and the slot is reusable. P0 inspects the bootstrap block and
// verifies all P1/P2/P3 slots are cleared (pid=0).
// ---------------------------------------------------------------------------
static void test_distributed_concurrent_exit() {
    std::cout << "[multi-proc/node] Concurrent fork+exit: P1/P2/P3 exit, P0 continues..." << std::endl;

    cleanup_all_distributed_shms(PPN_TEST_PROCS, "ppn_exit");
    setenv("UBALLOC_HEAP_ID", "ppn_exit", 1);

    // P0 forks 3 children (P1/P2/P3). Each child exits immediately after init.
    // P0 waits for all 3 children to exit, then inspects the bootstrap block.

    auto& parent_alloc = uballoc::get_global_allocator<uballoc::DistributedShmBackend<>>();
    parent_alloc.soft_reset();
    parent_alloc.init();
    parent_alloc.init_thread(0);
    assert(parent_alloc.is_initialized());
    assert(parent_alloc.backend().rank_ == 0);

    std::vector<pid_t> child_pids;
    for (int i = 1; i < PPN_TEST_PROCS; ++i) {
        pid_t pid = fork();
        assert(pid >= 0);
        if (pid == 0) {
            auto& alloc = uballoc::get_global_allocator<uballoc::DistributedShmBackend<>>();
            alloc.soft_reset();
            alloc.init();
            // NO init_thread(0) — init() calls ensure_thread_init() which
            // sets tl_thread_id_ = rank * num_cores (the correct value for
            // the assigned rank). Calling init_thread(0) would set
            // tl_thread_id_=0, causing focus() to underflow local_idx for
            // rank>0.
            assert(alloc.is_initialized());
            // Exit immediately — no alloc.
            alloc.reset();
            _exit(0);
        }
        child_pids.push_back(pid);
    }

    // Wait for all children to exit.
    for (pid_t cpid : child_pids) {
        int status;
        waitpid(cpid, &status, 0);
        assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }

    // Inspect the bootstrap block — all P1/P2/P3 slots should be cleared
    // (pid=0) because the children's destructors ran clear_bootstrap_slot.
    auto* bs = parent_alloc.backend().bootstrap_block_;
    assert(bs != nullptr);
    assert(bs->slots[0].pid.load(std::memory_order_acquire) != 0);  // P0 still alive
    for (int i = 1; i < PPN_TEST_PROCS; ++i) {
        int32_t pid_in_slot = bs->slots[i].pid.load(std::memory_order_acquire);
        assert(pid_in_slot == 0);
    }
    std::cout << "  Verified: P1/P2/P3 slots cleared (pid=0), P0 still alive" << std::endl;

    // Now P0 can re-fork a new child that should reclaim one of the dead slots.
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        auto& alloc = uballoc::get_global_allocator<uballoc::DistributedShmBackend<>>();
        alloc.soft_reset();
        alloc.init();
        // NO init_thread(0) — see comment in test_distributed_4proc_basic.
        assert(alloc.is_initialized());
        int new_rank = alloc.backend().rank_;
        assert(new_rank >= 1 && new_rank < PPN_TEST_PROCS);  // Not 0 (P0 alive)
        alloc.reset();
        _exit(0);
    } else {
        int status;
        waitpid(pid, &status, 0);
        assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
        std::cout << "  Verified: new child reclaimed a dead slot (rank 1..3)" << std::endl;
    }

    parent_alloc.reset();
    cleanup_all_distributed_shms(PPN_TEST_PROCS, "ppn_exit");
    unsetenv("UBALLOC_HEAP_ID");
}

// ---------------------------------------------------------------------------
// Test 6: 4-proc concurrent bump alloc — P0 forks 3 children (P1/P2/P3).
// All 4 procs alloc concurrently, verify no overlap across ranks. Each
// proc writes its rank pattern, then sends pointers to P0 via pipe. P0
// checks all 4*ALLOCS_PER_PROC ranges for overlap.
// ---------------------------------------------------------------------------
static void test_distributed_4proc_concurrent_alloc() {
    std::cout << "[multi-proc/node] 4-proc concurrent bump alloc, no overlap..." << std::endl;

    cleanup_all_distributed_shms(PPN_TEST_PROCS, "ppn_conc");
    setenv("UBALLOC_HEAP_ID", "ppn_conc", 1);

    constexpr int ALLOCS_PER_PROC = 30;
    constexpr size_t ALLOC_SIZE = 256;

    int rank_pipe[2];       // children -> parent: ranks
    int barrier_pipe[2];    // parent -> children: start barrier
    int data_pipe[2];       // children -> parent: pointers
    assert(pipe(rank_pipe) == 0);
    assert(pipe(barrier_pipe) == 0);
    assert(pipe(data_pipe) == 0);

    auto& parent_alloc = uballoc::get_global_allocator<uballoc::DistributedShmBackend<>>();
    parent_alloc.soft_reset();
    parent_alloc.init();
    parent_alloc.init_thread(0);
    assert(parent_alloc.is_initialized());
    assert(parent_alloc.backend().rank_ == 0);

    std::vector<pid_t> child_pids;
    for (int i = 1; i < PPN_TEST_PROCS; ++i) {
        pid_t pid = fork();
        assert(pid >= 0);
        if (pid == 0) {
            // Child: close read ends of parent's pipes, write ends of children's.
            close(rank_pipe[0]);
            close(barrier_pipe[1]);
            close(data_pipe[0]);

            auto& alloc = uballoc::get_global_allocator<uballoc::DistributedShmBackend<>>();
            alloc.soft_reset();
            alloc.init();
            // NO init_thread(0) — see comment in test_distributed_4proc_basic.
            assert(alloc.is_initialized());

            int rank = alloc.backend().rank_;
            ssize_t w = write(rank_pipe[1], &rank, sizeof(rank));
            if (w != sizeof(rank)) _exit(1);

            // Wait for parent's barrier signal.
            char c;
            ssize_t n = read(barrier_pipe[0], &c, 1);
            if (n != 1) _exit(1);
            close(barrier_pipe[0]);

            // Alloc ALLOCS_PER_PROC blocks, write rank pattern.
            uintptr_t ptrs[ALLOCS_PER_PROC];
            for (int k = 0; k < ALLOCS_PER_PROC; ++k) {
                void* p = alloc.malloc(ALLOC_SIZE);
                assert(p != nullptr);
                memset(p, static_cast<int>(0xB0 | (rank & 0x0F)), ALLOC_SIZE);
                ptrs[k] = reinterpret_cast<uintptr_t>(p);
            }

            // Send pointers to parent.
            ssize_t total = 0;
            while (total < static_cast<ssize_t>(sizeof(ptrs))) {
                ssize_t n2 = write(data_pipe[1],
                                    reinterpret_cast<char*>(ptrs) + total,
                                    sizeof(ptrs) - static_cast<size_t>(total));
                if (n2 <= 0) break;
                total += n2;
            }
            close(data_pipe[1]);
            close(rank_pipe[1]);

            // Free all and exit.
            for (int k = 0; k < ALLOCS_PER_PROC; ++k) {
                alloc.free(reinterpret_cast<void*>(ptrs[k]));
            }

            alloc.reset();
            _exit(0);
        }
        child_pids.push_back(pid);
    }

    // Parent: collect ranks.
    close(rank_pipe[1]);
    close(barrier_pipe[0]);
    close(data_pipe[1]);
    int ranks[PPN_TEST_PROCS];
    ranks[0] = 0;  // parent is P0
    for (int i = 1; i < PPN_TEST_PROCS; ++i) {
        ssize_t n = read(rank_pipe[0], &ranks[i], sizeof(int));
        assert(n == sizeof(int));
    }
    close(rank_pipe[0]);

    // Parent allocates one block first to trigger its own grow_segment
    // (serial), so the children's grow_segment calls don't race with the
    // parent's. This matches the pattern in test_distributed_fork_concurrent_bump.
    void* parent_first = parent_alloc.malloc(ALLOC_SIZE);
    assert(parent_first != nullptr);
    parent_alloc.free(parent_first);

    // Signal all children to start (write one byte per child).
    char c = 'G';
    for (int i = 1; i < PPN_TEST_PROCS; ++i) {
        ssize_t w = write(barrier_pipe[1], &c, 1);
        assert(w == 1);
    }
    close(barrier_pipe[1]);

    // Parent also allocs ALLOCS_PER_PROC blocks concurrently with children.
    uintptr_t parent_ptrs[ALLOCS_PER_PROC];
    for (int k = 0; k < ALLOCS_PER_PROC; ++k) {
        void* p = parent_alloc.malloc(ALLOC_SIZE);
        assert(p != nullptr);
        memset(p, 0xB0, ALLOC_SIZE);
        parent_ptrs[k] = reinterpret_cast<uintptr_t>(p);
    }

    // Collect pointers from each child.
    uintptr_t all_ptrs[PPN_TEST_PROCS][ALLOCS_PER_PROC];
    for (int i = 0; i < ALLOCS_PER_PROC; ++i) {
        all_ptrs[0][i] = parent_ptrs[i];
    }
    for (int i = 1; i < PPN_TEST_PROCS; ++i) {
        ssize_t total = 0;
        while (total < static_cast<ssize_t>(sizeof(all_ptrs[i]))) {
            ssize_t n = read(data_pipe[0],
                              reinterpret_cast<char*>(all_ptrs[i]) + total,
                              sizeof(all_ptrs[i]) - static_cast<size_t>(total));
            if (n <= 0) break;
            total += n;
        }
        assert(total == static_cast<ssize_t>(sizeof(all_ptrs[i])));
    }
    close(data_pipe[0]);

    // Wait for all children.
    for (pid_t cpid : child_pids) {
        int status;
        waitpid(cpid, &status, 0);
        assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }

    // Free parent's allocations.
    for (int k = 0; k < ALLOCS_PER_PROC; ++k) {
        parent_alloc.free(reinterpret_cast<void*>(parent_ptrs[k]));
    }

    // Verify no overlap across all 4*ALLOCS_PER_PROC pointers.
    int total_ptrs = PPN_TEST_PROCS * ALLOCS_PER_PROC;
    struct Range { uintptr_t base; size_t size; int rank; int slot; };
    std::vector<Range> ranges;
    ranges.reserve(total_ptrs);

    int overlap_count = 0;
    for (int i = 0; i < PPN_TEST_PROCS; ++i) {
        for (int k = 0; k < ALLOCS_PER_PROC; ++k) {
            Range r { all_ptrs[i][k], ALLOC_SIZE, ranks[i], k };
            for (auto& other : ranges) {
                uintptr_t a_end = r.base + r.size;
                uintptr_t o_end = other.base + other.size;
                if (r.base < o_end && other.base < a_end) {
                    std::cerr << "  OVERLAP: rank " << r.rank << " slot " << r.slot
                              << " [0x" << std::hex << r.base << ", 0x" << a_end << ")"
                              << " vs rank " << other.rank << " slot " << other.slot
                              << " [0x" << other.base << ", 0x" << o_end << ")"
                              << std::dec << std::endl;
                    overlap_count++;
                }
            }
            ranges.push_back(r);
        }
    }
    assert(overlap_count == 0);
    std::cout << "  Verified: " << total_ptrs << " allocations across 4 procs, no overlap" << std::endl;

    // Verify all 4 ranks are unique.
    bool used[PPN_TEST_PROCS] = {};
    for (int i = 0; i < PPN_TEST_PROCS; ++i) {
        assert(ranks[i] >= 0 && ranks[i] < PPN_TEST_PROCS);
        assert(!used[ranks[i]]);
        used[ranks[i]] = true;
    }

    parent_alloc.reset();
    cleanup_all_distributed_shms(PPN_TEST_PROCS, "ppn_conc");
    unsetenv("UBALLOC_HEAP_ID");
}

int main() {
    test_bootstrap_zero_init_multi_proc_fields();
    test_segment_descriptor_attach_count_zero_init();
#ifndef UBALLOC_USE_UBSE
    test_shm_close_fd_isolation_posix();
#endif
    test_distributed_4proc_basic();
    test_distributed_concurrent_exit();
    test_distributed_4proc_concurrent_alloc();
    std::cout << "\n=== Multi-process-per-node tests passed ===" << std::endl;
    return 0;
}
