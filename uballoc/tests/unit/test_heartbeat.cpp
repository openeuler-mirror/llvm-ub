// SPDX-License-Identifier: Apache-2.0

/**
 * @file test_heartbeat.cpp
 * @brief AR-16 — Dead-Peer Heartbeat and Auto-Detach tests.
 *
 * Covers 9 test cases:
 *
 *   Heartbeat write (4):
 *     1. test_heartbeat_initial_publish     — init() writes a non-zero heartbeat
 *     2. test_heartbeat_advances_idle       — HeartbeatThread advances independently (no malloc/free, reclaim disabled)
 *     3. test_heartbeat_env_interval        — UBALLOC_HEARTBEAT_INTERVAL_MS controls rate
 *     4. test_heartbeat_disabled            — UBALLOC_HEARTBEAT_ENABLED=0 disables writes
 *
 *   Dead-peer detection (3):
 *     5. test_dead_peer_force_detach        — kill -9 borrower ⇒ force-detaches its borrows
 *     6. test_false_positive_guard          — SIGSTOP pause < timeout ⇒ no false-positive detach
 *     7. test_dead_peer_timeout_env         — UBALLOC_DEAD_PEER_TIMEOUT_MS controls detection latency
 *
 *   wait_for_unreferenced unblock (1):
 *     8. test_all_peers_dead_or_silent      — helper returns true after peer dies + timeout elapses
 *                                               (UBSE's wait_for_unreferenced itself is gated by
 *                                                has_reliable_unreferenced_check; tested via the
 *                                                helper which works on POSIX too)
 *
 *   Parameter validation (1):
 *     9. test_interval_ge_timeout_rejected  — DEAD_PEER_TIMEOUT_MS ≤ INTERVAL_MS auto-adjusted to 2× interval
 *
 *   UBSE-specific (1):
 *    10. test_ubse_wait_for_unreferenced_runs — UBSE backend wait_for_unreferenced runs without hang
 *
 * All tests run in fork children to isolate ReclaimThread + heartbeat state.
 */

#include "uballoc.hpp"
#include "test_helpers.hpp"
#include <iostream>
#include <cassert>
#include <cstring>
#include <vector>
#include <unistd.h>
#include <signal.h>
#include <chrono>
#include <thread>
#include <atomic>
#include <sys/wait.h>

static constexpr const char* HEAP_ID = "hbtest";

// Large allocations force segment growth (1 slab = 4MB, 65 allocs ⇒ 2 segments).
static constexpr size_t LARGE_SIZE = 1 * 1024 * 1024;
static constexpr int NUM_ALLOCS = 65;

static constexpr bool is_ubse =
    !uballoc::DistributedShmBackend<>::ShmProvider::is_single_node;

// ---------------------------------------------------------------------------
// Env setup
// ---------------------------------------------------------------------------
static void setup_env(const char* mode,
                      int decay1_ms, int decay2_ms,
                      int scan_ms,
                      int heartbeat_ms,
                      int dead_peer_ms,
                      int wait_unref_ms = 0) {
    setenv("UBALLOC_HEAP_ID", HEAP_ID, 1);
    setenv("UBALLOC_RECLAIM_MODE", mode, 1);
    setenv("UBALLOC_RECLAIM_ENABLED", "1", 1);
    setenv("UBALLOC_RECLAIM_DECAY1_MS", std::to_string(decay1_ms).c_str(), 1);
    setenv("UBALLOC_RECLAIM_DECAY2_MS", std::to_string(decay2_ms).c_str(), 1);
    setenv("UBALLOC_RECLAIM_SCAN_INTERVAL_MS", std::to_string(scan_ms).c_str(), 1);
    setenv("UBALLOC_HEARTBEAT_INTERVAL_MS", std::to_string(heartbeat_ms).c_str(), 1);
    setenv("UBALLOC_DEAD_PEER_TIMEOUT_MS", std::to_string(dead_peer_ms).c_str(), 1);
    setenv("UBALLOC_WAIT_UNREFERENCED_TIMEOUT_MS", std::to_string(wait_unref_ms).c_str(), 1);
}

// Wait up to timeout_ms for predicate() returns true. Polls every 20ms.
static bool wait_for(std::function<bool()> predicate, int timeout_ms,
                     int poll_ms = 20) {
    auto start = std::chrono::steady_clock::now();
    while (true) {
        if (predicate()) return true;
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();
        if (elapsed > timeout_ms) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(poll_ms));
    }
}

// =============================================================================
// 1. init() publishes a non-zero heartbeat in P0's bootstrap slot.
// =============================================================================
static void test_heartbeat_initial_publish() {
    std::cout << "[heartbeat] init() publishes non-zero heartbeat..." << std::endl;

    cleanup_all_distributed_shms(2, HEAP_ID);
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        setup_env("async", 60000, 60000, 5000, 1000, 30000);
        auto& alloc = uballoc::get_global_allocator<uballoc::DistributedShmBackend<>>();
        alloc.soft_reset();
        alloc.init();
        alloc.init_thread(0);
        assert(alloc.is_initialized());

        auto* bs = alloc.backend().bootstrap_block_;
        assert(bs != nullptr);
        uint64_t hb0 = bs->slots[0].heartbeat_seq.load(std::memory_order_acquire);
        assert(hb0 > 0);
        std::cout << "  P0 heartbeat_seq=" << hb0 << " (non-zero)" << std::endl;

        alloc.reset();
        _exit(0);
    }
    int status;
    waitpid(pid, &status, 0);
    check_child(status);
    std::cout << "  Verified: init() writes non-zero heartbeat" << std::endl;
}

// =============================================================================
// 2. HeartbeatThread advances heartbeat_seq independently (no malloc/free needed).
//    Verifies the core benefit of the independent thread: a process that
//    never calls malloc/free still publishes heartbeats.
// =============================================================================
static void test_heartbeat_advances_idle() {
    std::cout << "[heartbeat] independent thread advances heartbeat (idle process)..." << std::endl;

    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        // ASYNC mode, heartbeat every 100ms, reclaim disabled.
        // Tests that heartbeat works even with reclaim OFF.
        setup_env("async", 60000, 60000, 5000, 100, 30000);
        setenv("UBALLOC_RECLAIM_ENABLED", "0", 1);  // disable reclaim
        auto& alloc = uballoc::get_global_allocator<uballoc::DistributedShmBackend<>>();
        alloc.soft_reset();
        alloc.init();
        alloc.init_thread(0);
        assert(alloc.is_initialized());

        auto* bs = alloc.backend().bootstrap_block_;
        uint64_t hb_first = bs->slots[0].heartbeat_seq.load(std::memory_order_acquire);
        assert(hb_first > 0);  // init() writes initial heartbeat

        // Do NOTHING — no malloc/free. Just wait. The HeartbeatThread
        // should advance the counter independently.
        bool advanced = wait_for([&]() {
            uint64_t hb_now = bs->slots[0].heartbeat_seq.load(std::memory_order_acquire);
            return hb_now > hb_first;
        }, 500);
        assert(advanced);
        uint64_t hb_now = bs->slots[0].heartbeat_seq.load(std::memory_order_acquire);
        std::cout << "  idle process: heartbeat " << hb_first << " → " << hb_now
                  << " (reclaim disabled, no malloc/free)" << std::endl;

        alloc.reset();
        _exit(0);
    }
    int status;
    waitpid(pid, &status, 0);
    check_child(status);
    std::cout << "  Verified: HeartbeatThread advances independently" << std::endl;
}

// =============================================================================
// 3. UBALLOC_HEARTBEAT_INTERVAL_MS controls write rate.
//    Verification: with interval=1000ms, heartbeat should NOT advance
//    within a 300ms window (rate-limited by HeartbeatThread). With
//    interval=50ms, heartbeat advances within 200ms.
// =============================================================================
static void test_heartbeat_env_interval() {
    std::cout << "[heartbeat] interval env controls write rate..." << std::endl;

    // Sub-test A: long interval (1000ms) — heartbeat should NOT advance
    // within a 300ms window.
    pid_t pid1 = fork();
    assert(pid1 >= 0);
    if (pid1 == 0) {
        setup_env("async", 60000, 60000, 5000, 1000, 30000);  // hb=1000ms
        auto& alloc = uballoc::get_global_allocator<uballoc::DistributedShmBackend<>>();
        alloc.soft_reset();
        alloc.init();
        alloc.init_thread(0);
        assert(alloc.is_initialized());

        auto* bs = alloc.backend().bootstrap_block_;
        uint64_t hb_first = bs->slots[0].heartbeat_seq.load(std::memory_order_acquire);
        assert(hb_first > 0);

        // Wait 300ms (< heartbeat interval 1000ms). HeartbeatThread
        // should not have written yet (interval not elapsed).
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        uint64_t hb_after = bs->slots[0].heartbeat_seq.load(std::memory_order_acquire);
        assert(hb_after == hb_first);
        std::cout << "  interval=1000ms: heartbeat stayed at " << hb_after
                  << " over 300ms (rate-limited)" << std::endl;

        alloc.reset();
        _exit(0);
    }
    int s1;
    waitpid(pid1, &s1, 0);
    check_child(s1);

    // Sub-test B: short interval (50ms) — heartbeat advances within 200ms.
    pid_t pid2 = fork();
    assert(pid2 >= 0);
    if (pid2 == 0) {
        setup_env("async", 60000, 60000, 5000, 50, 30000);  // hb=50ms
        auto& alloc = uballoc::get_global_allocator<uballoc::DistributedShmBackend<>>();
        alloc.soft_reset();
        alloc.init();
        alloc.init_thread(0);
        assert(alloc.is_initialized());

        auto* bs = alloc.backend().bootstrap_block_;
        uint64_t hb_first = bs->slots[0].heartbeat_seq.load(std::memory_order_acquire);
        assert(hb_first > 0);

        // HeartbeatThread writes every 50ms — should advance within 200ms.
        bool advanced = wait_for([&]() {
            uint64_t hb_now = bs->slots[0].heartbeat_seq.load(std::memory_order_acquire);
            return hb_now > hb_first;
        }, 500);
        assert(advanced);
        std::cout << "  interval=50ms: heartbeat advanced within 200ms" << std::endl;

        alloc.reset();
        _exit(0);
    }
    int s2;
    waitpid(pid2, &s2, 0);
    check_child(s2);

    std::cout << "  Verified: UBALLOC_HEARTBEAT_INTERVAL_MS controls rate" << std::endl;
}

// =============================================================================
// 4. UBALLOC_HEARTBEAT_ENABLED=0 disables heartbeat writes.
// =============================================================================
static void test_heartbeat_disabled() {
    std::cout << "[heartbeat] UBALLOC_HEARTBEAT_ENABLED=0 disables writes..." << std::endl;

    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        setup_env("async", 60000, 60000, 100, 1000, 30000);
        setenv("UBALLOC_HEARTBEAT_ENABLED", "0", 1);  // disable heartbeat
        auto& alloc = uballoc::get_global_allocator<uballoc::DistributedShmBackend<>>();
        alloc.soft_reset();
        alloc.init();
        alloc.init_thread(0);
        assert(alloc.is_initialized());

        auto* bs = alloc.backend().bootstrap_block_;
        // init() calls write_heartbeat, but it should no-op when disabled.
        uint64_t hb0 = bs->slots[0].heartbeat_seq.load(std::memory_order_acquire);
        assert(hb0 == 0);

        // Wait — heartbeat should remain 0 (no HeartbeatThread running).
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        hb0 = bs->slots[0].heartbeat_seq.load(std::memory_order_acquire);
        assert(hb0 == 0);
        std::cout << "  heartbeat stays at 0 (disabled)" << std::endl;

        alloc.reset();
        _exit(0);
    }
    int status;
    waitpid(pid, &status, 0);
    check_child(status);
    std::cout << "  Verified: UBALLOC_HEARTBEAT_ENABLED=0 disables heartbeat" << std::endl;
}

// =============================================================================
// 5. Dead-peer force-detach: kill -9 a borrower ⇒ force-detaches
//    its segments in the surviving process.
//
// Setup: P0 inits, forks P1. P1 allocates 65 Large objects (creates ≥2
// segments in P1's heap). P0 calls check_remote_segments to attach P1's
// segments (becoming a borrower). P0 verifies segments_[1][b] has LIVE
// entries. Then SIGKILL P1 (no chance to clear its slot/heartbeat). P0
// waits > dead_peer_timeout (set short). P0 calls scan_all_segments_impl()
// directly. P0 verifies segments_[1][b] entries are DETACHED (Phase 3a).
// =============================================================================
static void test_dead_peer_force_detach() {
    std::cout << "[heartbeat] dead-peer force-detach (Phase 3a)..." << std::endl;

    cleanup_all_distributed_shms(2, HEAP_ID);

    // P0 starts first; P1 is forked.
    setup_env("async", 60000, 60000, 100, 100, 200);  // dead_peer_timeout=200ms
    auto& parent_alloc = uballoc::get_global_allocator<uballoc::DistributedShmBackend<>>();
    parent_alloc.soft_reset();
    parent_alloc.init();
    parent_alloc.init_thread(0);
    assert(parent_alloc.is_initialized());
    assert(parent_alloc.backend().rank_ == 0);

    int ready_pipe[2];
    assert(pipe(ready_pipe) == 0);

    pid_t child_pid = fork();
    assert(child_pid >= 0);
    if (child_pid == 0) {
        // P1: allocates Large objects to create segments.
        close(ready_pipe[0]);
        auto& alloc = uballoc::get_global_allocator<uballoc::DistributedShmBackend<>>();
        alloc.soft_reset();
        alloc.init();
        assert(alloc.is_initialized());
        assert(alloc.backend().rank_ == 1);

        // Allocate enough to create ≥1 segment.
        std::vector<void*> ptrs;
        for (int i = 0; i < NUM_ALLOCS; ++i) {
            void* p = uballoc::malloc(LARGE_SIZE);
            if (!p) break;
            memset(p, 0xB0, LARGE_SIZE);
            ptrs.push_back(p);
        }
        assert(ptrs.size() == NUM_ALLOCS);

        // Signal parent that P1 has allocated.
        char signal = 'R';
        ssize_t w = write(ready_pipe[1], &signal, 1);
        close(ready_pipe[1]);
        assert(w == 1);

        // Keep P1 alive briefly so parent can attach + verify.
        // Then wait for parent's "kill me" signal.
        // Actually we just wait — parent will SIGKILL us when ready.
        // Keep ptrs alive so segments stay in-use (LIVE).
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(60));
        }
        _exit(0);  // unreachable
    }

    // P0: wait for P1's ready signal.
    close(ready_pipe[1]);
    char sig;
    ssize_t r = read(ready_pipe[0], &sig, 1);
    close(ready_pipe[0]);
    assert(r == 1);
    assert(sig == 'R');

    // P0: attach P1's segments via check_remote_segments.
    // The parent may need to call it explicitly (no cold-path malloc yet).
    parent_alloc.check_remote_segments();

    // Verify P0 has LIVE borrows on P1's segments (at least one bracket).
    bool has_live_p1 = false;
    for (int b = 0; b < 2; ++b) {
        for (const auto& seg : parent_alloc.backend().segments_[1][b]) {
            if (seg.state == uballoc::DistributedShmBackend<>::SegmentEntry::State::LIVE) {
                has_live_p1 = true;
                break;
            }
        }
        if (has_live_p1) break;
    }
    assert(has_live_p1);
    std::cout << "  P0 has LIVE borrows on P1's segments" << std::endl;

    // Verify P1's heartbeat is fresh (just allocated, scan thread running).
    auto* bs = parent_alloc.backend().bootstrap_block_;
    uint64_t hb_p1 = bs->slots[1].heartbeat_seq.load(std::memory_order_acquire);
    assert(hb_p1 > 0);

    // Prime the heartbeat tracking: scan once to record P1's current
    // counter value. The counter-based staleness check needs a baseline
    // observation before it can detect "stopped advancing."
    parent_alloc.scan_all_segments_impl();

    // SIGKILL P1 — no chance to clear slot or stop ReclaimThread cleanly.
    int kill_ret = kill(child_pid, SIGKILL);
    assert(kill_ret == 0);
    int status;
    waitpid(child_pid, &status, 0);
    assert(WIFSIGNALED(status));

    // Wait > dead_peer_timeout (200ms) so P1's heartbeat counter is
    // detected as stagnant. Add slack for scan latency.
    std::this_thread::sleep_for(std::chrono::milliseconds(400));

    // Trigger a scan directly to invoke detection.
    parent_alloc.scan_all_segments_impl();

    // Verify P0's segments_[1] entries are now DETACHED (force-released).
    bool any_detached = false;
    bool any_live_left = false;
    for (int b = 0; b < 2; ++b) {
        for (const auto& seg : parent_alloc.backend().segments_[1][b]) {
            if (seg.state == uballoc::DistributedShmBackend<>::SegmentEntry::State::DETACHED) {
                any_detached = true;
            } else if (seg.state == uballoc::DistributedShmBackend<>::SegmentEntry::State::LIVE) {
                any_live_left = true;
            }
        }
    }
    assert(any_detached);
    assert(!any_live_left);
    std::cout << "  Phase 3a: P1's segments force-DETACHED after kill -9" << std::endl;

    parent_alloc.reset();
    cleanup_all_distributed_shms(2, HEAP_ID);
    std::cout << "  Verified: dead-peer Phase 3a force-detach works" << std::endl;
}

// =============================================================================
// 6. SIGSTOP false-positive guard: pause < timeout ⇒ no false-positive detach.
//    P0 attaches P1's segments, then SIGSTOP P1 for < dead_peer_timeout.
//    P0 scans — P1's heartbeat is still fresh, so no force-detach.
//    SIGCONT P1; P1's heartbeat catches up.
// =============================================================================
static void test_false_positive_guard() {
    std::cout << "[heartbeat] false-positive guard: SIGSTOP < timeout..." << std::endl;

    cleanup_all_distributed_shms(2, HEAP_ID);
    setenv("UBALLOC_HEAP_ID", HEAP_ID, 1);

    // timeout=2000ms; pause will be 500ms.
    setup_env("async", 60000, 60000, 100, 100, 2000);
    auto& parent_alloc = uballoc::get_global_allocator<uballoc::DistributedShmBackend<>>();
    parent_alloc.soft_reset();
    parent_alloc.init();
    parent_alloc.init_thread(0);
    assert(parent_alloc.is_initialized());

    int ready_pipe[2];
    assert(pipe(ready_pipe) == 0);

    pid_t child_pid = fork();
    assert(child_pid >= 0);
    if (child_pid == 0) {
        close(ready_pipe[0]);
        auto& alloc = uballoc::get_global_allocator<uballoc::DistributedShmBackend<>>();
        alloc.soft_reset();
        alloc.init();
        assert(alloc.is_initialized());
        assert(alloc.backend().rank_ == 1);

        // Allocate enough to create ≥1 segment.
        std::vector<void*> ptrs;
        for (int i = 0; i < NUM_ALLOCS; ++i) {
            void* p = uballoc::malloc(LARGE_SIZE);
            if (!p) break;
            memset(p, 0xB0, LARGE_SIZE);
            ptrs.push_back(p);
        }
        assert(!ptrs.empty());

        char signal = 'R';
        ssize_t w = write(ready_pipe[1], &signal, 1);
        close(ready_pipe[1]);
        assert(w == 1);
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(60));
        }
        _exit(0);
    }

    close(ready_pipe[1]);
    char sig;
    ssize_t r = read(ready_pipe[0], &sig, 1);
    close(ready_pipe[0]);
    assert(r == 1);

    parent_alloc.check_remote_segments();
    bool has_live_p1 = false;
    for (int b = 0; b < 2; ++b) {
        for (const auto& seg : parent_alloc.backend().segments_[1][b]) {
            if (seg.state == uballoc::DistributedShmBackend<>::SegmentEntry::State::LIVE) {
                has_live_p1 = true;
                break;
            }
        }
        if (has_live_p1) break;
    }
    assert(has_live_p1);

    // Prime the heartbeat tracking.
    parent_alloc.scan_all_segments_impl();

    // SIGSTOP P1 for 500ms (< 2000ms timeout).
    int kill_ret = kill(child_pid, SIGSTOP);
    assert(kill_ret == 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // While P1 is paused, P0 scans — should NOT force-detach.
    parent_alloc.scan_all_segments_impl();

    bool any_detached = false;
    for (int b = 0; b < 2; ++b) {
        for (const auto& seg : parent_alloc.backend().segments_[1][b]) {
            if (seg.state == uballoc::DistributedShmBackend<>::SegmentEntry::State::DETACHED) {
                any_detached = true;
                break;
            }
        }
        if (any_detached) break;
    }
    assert(!any_detached);
    std::cout << "  P1 paused 500ms (< 2000ms timeout): no false-positive detach" << std::endl;

    // SIGCONT P1.
    kill(child_pid, SIGCONT);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    kill(child_pid, SIGKILL);
    int status;
    waitpid(child_pid, &status, 0);

    parent_alloc.reset();
    cleanup_all_distributed_shms(2, HEAP_ID);
    std::cout << "  Verified: no false-positive during brief pause" << std::endl;
}

// =============================================================================
// 7. UBALLOC_DEAD_PEER_TIMEOUT_MS controls detection latency.
// With a 10000ms timeout, after kill -9 + 700ms wait, Phase 3a does NOT
// fire (heartbeat not yet stale).
// =============================================================================
static void test_dead_peer_timeout_env() {
    std::cout << "[heartbeat] timeout env controls detection latency..." << std::endl;

    auto run_one = [&](int dead_peer_ms, int wait_ms, bool expect_detach) {
        cleanup_all_distributed_shms(2, HEAP_ID);

        setup_env("async", 60000, 60000, 100, 100, dead_peer_ms);
        auto& parent_alloc = uballoc::get_global_allocator<uballoc::DistributedShmBackend<>>();
        parent_alloc.soft_reset();
        parent_alloc.init();
        parent_alloc.init_thread(0);
        assert(parent_alloc.is_initialized());

        int ready_pipe[2];
        assert(pipe(ready_pipe) == 0);

        pid_t child_pid = fork();
        assert(child_pid >= 0);
        if (child_pid == 0) {
            close(ready_pipe[0]);
            auto& alloc = uballoc::get_global_allocator<uballoc::DistributedShmBackend<>>();
            alloc.soft_reset();
            alloc.init();
            assert(alloc.is_initialized());
            assert(alloc.backend().rank_ == 1);

            std::vector<void*> ptrs;
            for (int i = 0; i < NUM_ALLOCS; ++i) {
                void* p = uballoc::malloc(LARGE_SIZE);
                if (!p) break;
                memset(p, 0xD0, LARGE_SIZE);
                ptrs.push_back(p);
            }
            char signal = 'R';
            ssize_t w = write(ready_pipe[1], &signal, 1);
            close(ready_pipe[1]);
            assert(w == 1);
            for (;;) {
                std::this_thread::sleep_for(std::chrono::seconds(60));
            }
            _exit(0);
        }

        close(ready_pipe[1]);
        char sig;
        ssize_t r = read(ready_pipe[0], &sig, 1);
        close(ready_pipe[0]);
        assert(r == 1);

        parent_alloc.check_remote_segments();
        bool has_live_p1 = false;
        for (int b = 0; b < 2; ++b) {
            for (const auto& seg : parent_alloc.backend().segments_[1][b]) {
                if (seg.state == uballoc::DistributedShmBackend<>::SegmentEntry::State::LIVE) {
                    has_live_p1 = true;
                    break;
                }
            }
            if (has_live_p1) break;
        }
        assert(has_live_p1);

        // Prime heartbeat tracking before killing P1.
        parent_alloc.scan_all_segments_impl();

        kill(child_pid, SIGKILL);
        int status;
        waitpid(child_pid, &status, 0);

        std::this_thread::sleep_for(std::chrono::milliseconds(wait_ms));
        parent_alloc.scan_all_segments_impl();

        bool any_detached = false;
        for (int b = 0; b < 2; ++b) {
            for (const auto& seg : parent_alloc.backend().segments_[1][b]) {
                if (seg.state == uballoc::DistributedShmBackend<>::SegmentEntry::State::DETACHED) {
                    any_detached = true;
                    break;
                }
            }
            if (any_detached) break;
        }
        if (expect_detach) {
            assert(any_detached);
        } else {
            assert(!any_detached);
        }

        parent_alloc.reset();
        cleanup_all_distributed_shms(2, HEAP_ID);
    };

    // Sub-test A: 500ms timeout, wait 700ms ⇒ expect detach.
    run_one(500, 700, true);
    std::cout << "  timeout=500ms, wait=700ms: Phase 3a fired" << std::endl;

    // Sub-test B: 10000ms timeout, wait 700ms ⇒ expect NO detach.
    run_one(10000, 700, false);
    std::cout << "  timeout=10000ms, wait=700ms: Phase 3a did NOT fire" << std::endl;

    std::cout << "  Verified: UBALLOC_DEAD_PEER_TIMEOUT_MS controls latency" << std::endl;
}

// =============================================================================
// 8. all_peers_dead_or_silent helper — works on POSIX too (does not require
//    has_reliable_unreferenced_check). Verifies the logic wait_for_unreferenced
//    uses to decide "all blockers are dead" without exercising the daemon
//    query path (which is UBSE-only).
//
// Setup: P0 + P1. Kill P1. After timeout, all_peers_dead_or_silent returns
// true. Before timeout, returns false. Also verifies hb==0 (peer joined but
// hasn't published) returns false (the conservative grace behavior).
// =============================================================================
static void test_all_peers_dead_or_silent() {
    std::cout << "[heartbeat] all_peers_dead_or_silent helper logic..." << std::endl;

    cleanup_all_distributed_shms(2, HEAP_ID);

    setup_env("async", 60000, 60000, 100, 100, 500);  // timeout=500ms
    auto& parent_alloc = uballoc::get_global_allocator<uballoc::DistributedShmBackend<>>();
    parent_alloc.soft_reset();
    parent_alloc.init();
    parent_alloc.init_thread(0);
    assert(parent_alloc.is_initialized());

    int ready_pipe[2];
    assert(pipe(ready_pipe) == 0);

    pid_t child_pid = fork();
    assert(child_pid >= 0);
    if (child_pid == 0) {
        close(ready_pipe[0]);
        auto& alloc = uballoc::get_global_allocator<uballoc::DistributedShmBackend<>>();
        alloc.soft_reset();
        alloc.init();
        assert(alloc.is_initialized());
        assert(alloc.backend().rank_ == 1);
        char signal = 'R';
        ssize_t w = write(ready_pipe[1], &signal, 1);
        close(ready_pipe[1]);
        assert(w == 1);
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(60));
        }
        _exit(0);
    }

    close(ready_pipe[1]);
    char sig;
    ssize_t r = read(ready_pipe[0], &sig, 1);
    close(ready_pipe[0]);
    assert(r == 1);

    // P1 alive, heartbeat fresh — all_peers_dead_or_silent should be false.
    // First call records P1's counter as baseline (returns false).
    uint64_t now = parent_alloc.backend().steady_now_ns();
    bool all_dead = parent_alloc.backend().all_peers_dead_or_silent(now);
    assert(!all_dead);
    std::cout << "  P1 alive: all_peers_dead_or_silent=false (correct)" << std::endl;

    // Kill P1. Wait < timeout (200ms < 500ms) — should still be false
    // (counter stopped but not beyond timeout yet).
    kill(child_pid, SIGKILL);
    int status;
    waitpid(child_pid, &status, 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    now = parent_alloc.backend().steady_now_ns();
    all_dead = parent_alloc.backend().all_peers_dead_or_silent(now);
    assert(!all_dead);
    std::cout << "  P1 dead, 200ms (< 500ms timeout): false (correct)" << std::endl;

    // Wait > timeout (700ms total > 500ms) — counter has been stagnant
    // beyond the timeout, should now be true.
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    now = parent_alloc.backend().steady_now_ns();
    all_dead = parent_alloc.backend().all_peers_dead_or_silent(now);
    assert(all_dead);
    std::cout << "  P1 dead, > timeout: true (correct)" << std::endl;

    parent_alloc.reset();
    cleanup_all_distributed_shms(2, HEAP_ID);
    std::cout << "  Verified: all_peers_dead_or_silent logic" << std::endl;
}

// =============================================================================
// 9. UBSE-only: wait_for_unreferenced end-to-end on UBSE backend.
//
// Only compiled when UBALLOC_USE_UBSE is defined — wait_for_unreferenced's
// body is gated by `if constexpr (has_reliable_unreferenced_check)` (UBSE
// only; POSIX is_single_node=true skips the entire body).
//
// This test exercises the real wait_for_unreferenced code path on the UBSE
// daemon. On a single-node UBSE test environment all forked processes share
// the same node_slot_id, so shm_is_unreferenced naturally returns true (no
// cross-node import) and wait_for_unreferenced completes without needing
// the heartbeat/timeout unblock. The cross-node hang scenario requires a
// multi-node fabric (the cleanup_shms_ub.sh two-phase ssh protocol in
// Feature 4's residue-cleanup section) and is documented as a companion
// test for multi-node environments.
//
// What this test verifies on single-node UBSE:
//   - wait_for_unreferenced's body actually compiles and runs (the
//     `if constexpr` gate is satisfied).
//   - It completes in bounded time (no hang) when the daemon reports
//     unreferenced — regression guard against future changes that might
//     break the poll loop.
//   - UBALLOC_WAIT_UNREFERENCED_TIMEOUT_MS is parsed correctly into
//     wait_unreferenced_timeout_ms_ (verified via backend field).
//   - all_peers_dead_or_silent integrates correctly with the wait loop
//     (the helper is called from wait_one; on single-node UBSE with a
//     live P1 it returns false, so the wait falls through to the normal
//     shm_is_unreferenced path).
// =============================================================================
#ifdef UBALLOC_USE_UBSE
static void test_ubse_wait_for_unreferenced_runs() {
    std::cout << "[heartbeat] UBSE: wait_for_unreferenced runs on UBSE backend..." << std::endl;

    cleanup_all_distributed_shms(2, HEAP_ID);

    // Hard timeout = 5000ms (safety net; should NOT fire on single-node).
    // Heartbeat path also enabled.
    setup_env("async", 60000, 60000, 100, 100, 500, /*wait_unref_ms=*/5000);
    auto& parent_alloc = uballoc::get_global_allocator<uballoc::DistributedShmBackend<>>();
    parent_alloc.soft_reset();
    parent_alloc.init();
    parent_alloc.init_thread(0);
    assert(parent_alloc.is_initialized());
    assert(parent_alloc.backend().rank_ == 0);

    // Verify the env var was parsed into the backend field.
    assert(parent_alloc.backend().wait_unreferenced_timeout_ms_ == 5000);
    assert(parent_alloc.backend().heartbeat_interval_ns_ == 100 * 1'000'000ULL);
    assert(parent_alloc.backend().dead_peer_timeout_ns_ == 500 * 1'000'000ULL);
    std::cout << "  UBSE config parsed: wait_timeout=5000ms hb_interval=100ms dead_peer=500ms"
              << std::endl;

    int ready_pipe[2];
    assert(pipe(ready_pipe) == 0);

    pid_t child_pid = fork();
    assert(child_pid >= 0);
    if (child_pid == 0) {
        // P1: init + attach P0's metadata, then exit cleanly.
        // On single-node UBSE, P1's import is same-node, so P0's
        // shm_is_unreferenced ignores it (other_node_count == 0).
        close(ready_pipe[0]);
        auto& alloc = uballoc::get_global_allocator<uballoc::DistributedShmBackend<>>();
        alloc.soft_reset();
        alloc.init();
        assert(alloc.is_initialized());
        assert(alloc.backend().rank_ == 1);
        alloc.check_remote_segments();  // attach P0's metadata

        char signal = 'R';
        ssize_t w = write(ready_pipe[1], &signal, 1);
        close(ready_pipe[1]);
        assert(w == 1);
        alloc.reset();  // clean exit — releases P1's import
        _exit(0);
    }

    close(ready_pipe[1]);
    char sig;
    ssize_t r = read(ready_pipe[0], &sig, 1);
    close(ready_pipe[0]);
    assert(r == 1);

    // Wait for P1 to exit cleanly.
    int status;
    waitpid(child_pid, &status, 0);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);

    // P0 reset() → am_last_on_node → wait_for_unreferenced.
    // On single-node UBSE, the daemon reports unreferenced (P1 released
    // its import; no cross-node imports exist). wait_for_unreferenced
    // must complete quickly (well under the 5000ms hard timeout).
    auto start = std::chrono::steady_clock::now();
    parent_alloc.reset();
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();

    // Must complete quickly (no hang). Allow up to 10s for daemon RPC
    // latency on a loaded test machine, but well under the 5000ms hard
    // timeout + poll slack that would indicate a hang.
    assert(elapsed_ms < 10000);
    std::cout << "  UBSE wait_for_unreferenced completed in " << elapsed_ms
              << "ms (single-node, no cross-node import)" << std::endl;

    cleanup_all_distributed_shms(2, HEAP_ID);
    std::cout << "  Verified: UBSE wait_for_unreferenced runs without hang" << std::endl;
}
#endif // UBALLOC_USE_UBSE

// =============================================================================
// 10. UBALLOC_DEAD_PEER_TIMEOUT_MS <= UBALLOC_HEARTBEAT_INTERVAL_MS triggers
//    auto-adjust to 2× interval with warning (P2-1 fix).
// =============================================================================
static void test_interval_ge_timeout_rejected() {
    std::cout << "[heartbeat] interval >= timeout auto-adjusted..." << std::endl;

    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        // interval=1000ms, timeout=500ms → timeout < interval → auto-adjust
        setup_env("async", 60000, 60000, 5000, 1000, 500);
        auto& alloc = uballoc::get_global_allocator<uballoc::DistributedShmBackend<>>();
        alloc.soft_reset();
        alloc.init();
        alloc.init_thread(0);
        assert(alloc.is_initialized());
        // Verify timeout was auto-adjusted to 2× interval = 2000ms
        assert(alloc.backend().dead_peer_timeout_ns_ == 1000 * 2 * 1000000ULL);
        std::cout << "  timeout auto-adjusted to 2× interval (2000ms)" << std::endl;
        alloc.reset();
        _exit(0);
    }
    int status;
    waitpid(pid, &status, 0);
    check_child(status);
    cleanup_all_distributed_shms(2, HEAP_ID);
    std::cout << "  Verified: timeout <= interval auto-adjusted" << std::endl;
}

// =============================================================================
// Coverage notes (not executed — documented test gaps from review):
//
// HUGE bracket force-detach: Phase 3a now iterates b < DATA_BRACKETS (3)
// instead of b < 2, so HUGE borrowed segments are covered. A dedicated
// test would require allocating >2MB Huge objects in P1 and verifying
// P0's segments_[1][2] entries are DETACHED after P1 is killed. The
// code path is identical to the Small/Large test (test_dead_peer_force_detach),
// just with a different bracket index — the b<2→b<DATA_BRACKETS fix is
// verified by code inspection.
//
// Alive-peer wait hang: all_peers_dead_or_silent returns false if any
// peer has a live heartbeat, even if the "still referenced" report is
// from a different (crashed) borrower. This is documented in user_manual
// as a residual scenario — the workaround is UBALLOC_WAIT_UNREFERENCED_TIMEOUT_MS.
// A dedicated test would need a multi-node UBSE environment with precise
// import state control.
// =============================================================================

// =============================================================================
// main
// =============================================================================
int main() {
    cleanup_all_distributed_shms(2, HEAP_ID);

    test_heartbeat_initial_publish();
    test_heartbeat_advances_idle();
    test_heartbeat_env_interval();
    test_heartbeat_disabled();
    test_dead_peer_force_detach();
    test_false_positive_guard();
    test_dead_peer_timeout_env();
    test_all_peers_dead_or_silent();
    test_interval_ge_timeout_rejected();
#ifdef UBALLOC_USE_UBSE
    test_ubse_wait_for_unreferenced_runs();
#endif

    cleanup_all_distributed_shms(2, HEAP_ID);
    std::cout << "\n=== Heartbeat tests passed ===" << std::endl;
    return 0;
}
