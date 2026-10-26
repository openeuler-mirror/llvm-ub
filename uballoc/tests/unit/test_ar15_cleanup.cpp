// SPDX-License-Identifier: Apache-2.0
 
/**
 * @file test_ar15_cleanup.cpp
 * @brief AR-15 DT: Memory Status Cleanup on Proper/Improper Exit
 *
 * Cases:
 *   DT15-1 R1: soft_reset() — clears in-memory state, keeps shm objects
 *   DT15-2 R2: reset() — full cleanup including shm delete
 *   DT15-3 R3: process exit — destructor cleans up all shms
 *   DT15-4 R4: stale shm from crashed process — reclaimed on next init
 *   DT15-5 R5: cleanup order end-state (slot cleared, bootstrap unmapped,
 *              VA released) after reset()
 *   DT15-6 R6: soft_reset() calls uffd_uninstall + fh_uninstall
 *              (destructor does NOT call — OS reclaims fds/maps at exit)
 */
 
#include "uballoc.hpp"
#include "test_helpers.hpp"
 
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
 
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
 
namespace {
 
using ShmProvider = uballoc::DistributedShmBackend<>::ShmProvider;
 
// True if at least one own data shm exists for the given heap/rank.
bool own_data_shm_exists(const std::string& heap_id, int rank) {
    for (int b = 0; b < 3; ++b) {
        int rt = b + 1;
        // Check seg 0 (always created on first malloc).
        if (ShmProvider::shm_exists(ShmProvider::shm_name(heap_id, rank, rt, 0))) {
            return true;
        }
    }
    return false;
}
 
// True if the metadata shm for (heap_id, rank) exists.
bool metadata_shm_exists(const std::string& heap_id, int rank) {
    return ShmProvider::shm_exists(ShmProvider::shm_name(heap_id, rank, 0, 0));
}
 
// -----------------------------------------------------------------------------
// DT15-1 R1: soft_reset() clears in-memory state, keeps shm objects
// -----------------------------------------------------------------------------
void dt15_1_soft_reset_keeps_shms(const std::string& heap_id) {
    std::cout << "[DT15-1] soft_reset clears state, keeps shm objects..." << std::endl;
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        ar_set_heap_id(heap_id);
        ar_child_init();
        assert(uballoc::rank() == 0);
        std::string hid = ar_alloc().backend().heap_id_;
 
        void* p = uballoc::malloc(64);
        assert(p);
        std::memset(p, 0x15, 64);
 
        // Verify shms exist before soft_reset.
        assert(metadata_shm_exists(hid, 0));
        assert(own_data_shm_exists(hid, 0));
 
        // Verify backend state is live.
        assert(ar_alloc().backend().bootstrap_block_ != nullptr);
        assert(ar_alloc().backend().rank_ == 0);
        assert(ar_alloc().is_initialized());
 
        // soft_reset: clears in-memory state but DOES NOT unlink shms
        // (fork-child scenario — parent's shms must persist).
        ar_alloc().soft_reset();
 
        // In-memory state cleared.
        // Note: there are TWO rank_ fields — GlobalAllocator::rank_ (the
        // public rank() accessor) is reset to 0 (default/uninitialized),
        // while DistributedShmBackend::rank_ is reset to -1 (invalid).
        assert(!ar_alloc().is_initialized());
        assert(ar_alloc().rank() == 0);            // GlobalAllocator::rank_ → 0
        assert(ar_alloc().backend().rank_ == -1);   // DistributedShmBackend::rank_ → -1
        assert(ar_alloc().backend().bootstrap_block_ == nullptr);
        assert(ar_alloc().backend().heap_id_.empty());
 
        // Shm objects STILL exist (soft_reset didn't delete them).
        assert(metadata_shm_exists(hid, 0));
        assert(own_data_shm_exists(hid, 0));
 
        std::cout << "  OK: state cleared, shms preserved" << std::endl;
        // Manual cleanup so the next test starts clean.
        cleanup_all_distributed_shms(static_cast<int>(uballoc::MAX_PROCESSES), hid);
        _exit(0);
    }
    int st;
    waitpid(pid, &st, 0);
    check_child(st);
}
 
// -----------------------------------------------------------------------------
// DT15-2 R2: reset() — full cleanup including shm delete
// -----------------------------------------------------------------------------
void dt15_2_reset_deletes_shms(const std::string& heap_id) {
    std::cout << "[DT15-2] reset deletes all shm objects..." << std::endl;
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        ar_set_heap_id(heap_id);
        ar_child_init();
        std::string hid = ar_alloc().backend().heap_id_;
 
        void* p = uballoc::malloc(64);
        assert(p);
        std::memset(p, 0x52, 64);
        assert(metadata_shm_exists(hid, 0));
        assert(own_data_shm_exists(hid, 0));
 
        ar_alloc().reset();
 
        // In-memory state cleared.
        assert(!ar_alloc().is_initialized());
        assert(ar_alloc().backend().rank_ == -1);
        assert(ar_alloc().backend().bootstrap_block_ == nullptr);
 
        // All own shms deleted by reset() (POSIX) / detach+unlink (UBSE).
        // On UBSE, the own-shm unlink only happens if am_last_on_node, so
        // we restrict the existence check to the metadata shm + seg 0 data
        // shm, which are always unlinked by the owning process on reset().
        // For UBSE in a single-process test, am_last_on_node is true.
        assert(!metadata_shm_exists(hid, 0));
        assert(!own_data_shm_exists(hid, 0));
 
        std::cout << "  OK: shms deleted, state cleared" << std::endl;
        _exit(0);
    }
    int st;
    waitpid(pid, &st, 0);
    check_child(st);
}
 
// -----------------------------------------------------------------------------
// DT15-3 R3: process exit — destructor cleans up all shms
//
// On POSIX: the destructor calls cleanup_own_shms which unlinks the
// exiting process's shms. So the child's shms are gone after exit.
//
// On UBSE: the destructor only unlinks shms if am_last_on_node. Since the
// parent (P0) is still alive on the same node, the child's shms are NOT
// unlinked. We verify the slot is cleared (works on both backends) and
// only check shm deletion on POSIX.
// -----------------------------------------------------------------------------
void dt15_3_exit_runs_destructor(const std::string& heap_id) {
    std::cout << "[DT15-3] process exit runs destructor cleanup..." << std::endl;
    ar_set_heap_id(heap_id);
    ar_parent_init();
    std::string hid = ar_alloc().backend().heap_id_;

    // Fork a child that becomes rank 1, allocates, then EXITS normally
    // (via exit(0) which runs atexit + static destructors).
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        ar_child_init();  // child becomes rank 1
        assert(uballoc::rank() == 1);
        void* p = uballoc::malloc(64);
        assert(p);
        std::memset(p, 0x53, 64);
        // exit(0) — runs atexit + static destructors (~GlobalAllocator
        // -> ~DistributedShmBackend -> clear_bootstrap_slot + cleanup).
        ::exit(0);
    }

    int st;
    waitpid(pid, &st, 0);
    check_child(st);

    // Slot 1 cleared in the bootstrap block (clear_bootstrap_slot ran).
    auto* bb = ar_alloc().backend().bootstrap_block_;
    assert(bb != nullptr);
    int32_t slot1_pid = bb->slots[1].pid.load(std::memory_order_acquire);
    assert(slot1_pid == 0);

    // On POSIX: the child's shms are unlinked by cleanup_own_shms.
    // On UBSE: the child's shms are NOT unlinked (am_last_on_node == false
    // because the parent is still alive on the same node). The parent will
    // clean them up when it exits (or reset()).
    if constexpr (!uballoc::DistributedShmBackend<>::ShmProvider::has_reliable_unreferenced_check) {
        assert(!metadata_shm_exists(hid, 1));
        assert(!own_data_shm_exists(hid, 1));
    }

    ar_alloc().reset();
    // Final cleanup of any remaining child shms (UBSE: parent didn't unlink them).
    cleanup_all_distributed_shms(static_cast<int>(uballoc::MAX_PROCESSES), hid);
    std::cout << "  OK: destructor ran, slot cleared"
              << (uballoc::DistributedShmBackend<>::ShmProvider::has_reliable_unreferenced_check
                  ? " (UBSE: shms kept, parent cleans up)" : ", shms deleted")
              << std::endl;
}
 
// -----------------------------------------------------------------------------
// DT15-4 R4: stale shm from crashed process — reclaimed on next init
// -----------------------------------------------------------------------------
void dt15_4_stale_shm_reclaimed(const std::string& heap_id) {
    std::cout << "[DT15-4] stale shm from crashed process — reclaimed..." << std::endl;
    ar_set_heap_id(heap_id);
    ar_parent_init();
    std::string hid = ar_alloc().backend().heap_id_;

    // Fork a child (rank 1) that creates shms and then is SIGKILLed
    // (no destructor runs — shms remain as orphans).
    int fds[2], rp[2];
    assert(pipe(fds) == 0);
    assert(pipe(rp) == 0);
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        ::close(fds[0]);
        ::close(rp[1]);
        // Wait for P0 to be ready.
        assert(ar_read_byte(rp[0]) == 'G');
        ar_child_init();
        assert(uballoc::rank() == 1);
        void* p = uballoc::malloc(64);
        assert(p);
        std::memset(p, 0x54, 64);
        ar_write_byte(fds[1], 'R');
        // Hold the slot until killed.
        for (;;) ::sleep(60);
        _exit(0);
    }
    ::close(fds[1]);
    ::close(rp[0]);
    ar_write_byte(rp[1], 'G');  // signal child to init
    assert(ar_read_byte(fds[0]) == 'R');

    // Stale shms exist (created by the about-to-be-killed child).
    assert(metadata_shm_exists(hid, 1));
    assert(own_data_shm_exists(hid, 1));

    // SIGKILL the child — no destructor runs.
    int k = ::kill(pid, SIGKILL);
    assert(k == 0);
    int st;
    waitpid(pid, &st, 0);
    assert(WIFSIGNALED(st) && WTERMSIG(st) == SIGKILL);

    // Stale shms STILL exist (SIGKILL skipped destructor cleanup).
    assert(metadata_shm_exists(hid, 1));
    assert(own_data_shm_exists(hid, 1));

    // Fork a replacement; its init() must detect the stale slot 1 (kill
    // returns ESRCH on POSIX, or is_rank_dead_cross_node on UBSE), call
    // unlink_process_shms(1), and reclaim the slot.
    // Two pipes: p2c (parent→child 'G'), c2p (child→parent 'D').
    int p2c[2], c2p[2];
    assert(pipe(p2c) == 0);
    assert(pipe(c2p) == 0);
    pid_t pid2 = fork();
    assert(pid2 >= 0);
    if (pid2 == 0) {
        ::close(fds[0]);
        ::close(rp[1]);
        ::close(p2c[1]);
        ::close(c2p[0]);
        // Wait for P0 to be ready.
        assert(ar_read_byte(p2c[0]) == 'G');
        ar_child_init();
        assert(uballoc::rank() == 1);  // reclaimed stale slot
        void* p = uballoc::malloc(64);
        assert(p);
        std::memset(p, 0x55, 64);
        // Verify the new rank-1 shms exist.
        assert(metadata_shm_exists(ar_alloc().backend().heap_id_, 1));
        assert(own_data_shm_exists(ar_alloc().backend().heap_id_, 1));
        ar_write_byte(c2p[1], 'D');
        ::close(p2c[0]);
        ::close(c2p[1]);
        ::exit(0);  // destructor cleans up the new shms (or parent cleans up on UBSE)
    }
    ::close(fds[0]);
    ::close(rp[1]);
    ::close(p2c[0]);
    ::close(c2p[1]);
    ar_write_byte(p2c[1], 'G');  // signal replacement to init
    assert(ar_read_byte(c2p[0]) == 'D');
    int st2;
    waitpid(pid2, &st2, 0);
    check_child(st2);

    ::close(p2c[1]);
    ::close(c2p[0]);
    // On POSIX: replacement's destructor unlinked its shms.
    // On UBSE: replacement's shms are kept (parent is still alive on the
    // same node). The parent cleans them up via reset() + cleanup below.
    ar_alloc().reset();
    cleanup_all_distributed_shms(static_cast<int>(uballoc::MAX_PROCESSES), hid);
    std::cout << "  OK: stale slot detected, shms unlinked, slot reclaimed" << std::endl;
}
 
// -----------------------------------------------------------------------------
// DT15-5 R5: cleanup order end-state — slot cleared, bootstrap unmapped,
//             VA released — after reset()
//
// We verify the END STATE after reset() reflects each cleanup stage ran:
//   (1) clear_bootstrap_slot    → slot pid == 0 (verified in DT15-3/4)
//   (2) cleanup_remote_regions  → metadata_regions_[k] zeroed (no leak)
//   (3) cleanup_own_shms        → own shms unlinked (shm_exists == false)
//   (4) cleanup_bootstrap_mapping → bootstrap_block_ == nullptr
//   (5) release_reservation     → 28TB PROT_NONE mapping gone from maps
// -----------------------------------------------------------------------------
void dt15_5_cleanup_order_end_state(const std::string& heap_id) {
    std::cout << "[DT15-5] cleanup order end-state after reset()..." << std::endl;
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        ar_set_heap_id(heap_id);
        ar_child_init();
        std::string hid = ar_alloc().backend().heap_id_;
        int my_rank = uballoc::rank();
        assert(my_rank == 0);

        // Allocate to create own shms + bootstrap slot is populated.
        void* p = uballoc::malloc(64);
        assert(p);
        std::memset(p, 0x55, 64);

        auto& backend = ar_alloc().backend();
        assert(backend.bootstrap_block_ != nullptr);

        // Locate the 28TB VA reservation.
        auto regions = ar_parse_maps();
        uintptr_t va_base = ar_find_large_span_start(regions, 1ULL << 40);
        assert(va_base != 0);

        // Verify shms exist before reset.
        assert(metadata_shm_exists(hid, my_rank));
        assert(own_data_shm_exists(hid, my_rank));

        ar_alloc().reset();

        // (2) In-memory remote regions zeroed.
        for (int k = 0; k < static_cast<int>(uballoc::MAX_PROCESSES); ++k) {
            if (k == my_rank) continue;
            assert(backend.metadata_regions_[k].address == nullptr);
        }

        // (3) Own shms unlinked (POSIX: cleanup_own_shms; UBSE: am_last_on_node
        // → unlink_node_shms). In a single-process test, am_last_on_node is
        // true, so the shms are unlinked on both backends.
        assert(!metadata_shm_exists(hid, my_rank));
        assert(!own_data_shm_exists(hid, my_rank));

        // (4) bootstrap_block_ cleared.
        assert(backend.bootstrap_block_ == nullptr);
        assert(backend.bootstrap_ptr_ == nullptr);

        // (5) VA reservation released — 28TB mapping gone.
        auto regions_after = ar_parse_maps();
        uint64_t covered = ar_maps_covered_bytes(
            regions_after, va_base, va_base + uballoc::VA_RESERVATION_SIZE);
        assert(covered == 0);

        std::cout << "  OK: remote zeroed, own unlinked, "
                  << "bootstrap unmapped, VA released" << std::endl;
        // Final cleanup of bootstrap shm (not unlinked by reset()).
        ShmProvider::shm_unlink(ShmProvider::bootstrap_shm_name(hid));
        _exit(0);
    }
    int st;
    waitpid(pid, &st, 0);
    check_child(st);
}
 
// -----------------------------------------------------------------------------
// DT15-6 R6: soft_reset() calls uffd_uninstall + fh_uninstall
//            (destructor does NOT call — OS reclaims fds/maps at exit)
// -----------------------------------------------------------------------------
void dt15_6_soft_reset_uninstalls_handlers(const std::string& heap_id) {
    std::cout << "[DT15-6] soft_reset calls uffd_uninstall + fh_uninstall..." << std::endl;
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        ar_set_heap_id(heap_id);
        ar_child_init();
 
        // Enable the fault handler (sets g_fh_active = true).
        bool fh_ok = uballoc::enable_fault_handler();
        assert(fh_ok);
        assert(uballoc::g_fh_active.load(std::memory_order_acquire));
 
        // Enable uffd (sets g_uffd_active = true if uffd available; on
        // systems without uffd, it falls back to fh and stays true).
        bool uffd_ok = ar_alloc().enable_userfaultfd();
        assert(uffd_ok);
        // After enable_userfaultfd, g_fh_active is also true (uffd installs
        // fh as the safety net).
        assert(uballoc::g_fh_active.load(std::memory_order_acquire));
 
        // soft_reset must uninstall both (uffd_uninstall + fh_uninstall).
        ar_alloc().soft_reset();
 
        assert(!uballoc::g_uffd_active.load(std::memory_order_acquire));
        assert(!uballoc::g_fh_active.load(std::memory_order_acquire));
 
        std::cout << "  OK: g_uffd_active=false, g_fh_active=false after soft_reset"
                  << std::endl;
        _exit(0);
    }
    int st;
    waitpid(pid, &st, 0);
    check_child(st);
}
 
}  // namespace
 
int main() {
    std::string id = ar_heap_id("ar15");
    cleanup_all_distributed_shms(8, id);
 
    ar_pre_case_cleanup(id);
    dt15_1_soft_reset_keeps_shms(id);
    ar_pre_case_cleanup(id);
    dt15_2_reset_deletes_shms(id);
    ar_pre_case_cleanup(id);
    dt15_3_exit_runs_destructor(id);
    ar_pre_case_cleanup(id);
    dt15_4_stale_shm_reclaimed(id);
    ar_pre_case_cleanup(id);
    dt15_5_cleanup_order_end_state(id);
    ar_pre_case_cleanup(id);
    dt15_6_soft_reset_uninstalls_handlers(id);
 
    cleanup_all_distributed_shms(8, id);
    std::cout << "\n=== AR-15 DT passed ===" << std::endl;
    return 0;
}
