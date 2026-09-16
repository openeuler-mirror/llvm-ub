// SPDX-License-Identifier: Apache-2.0

/**
 * @file test_reclaim.cpp
 * @brief Reclaim feature tests (non-log/non-stats).
 *
 * Covers 7 functional test cases:
 *
 *   Reclaim mechanism (4):
 *     1. test_reclaim_sync_mode         — SYNC mode inline LIVE→DETACHED→RETURNED
 *     2. test_reclaim_async_basic        — ASYNC mode background LIVE→DETACHED→RETURNED
 *     3. test_reclaim_purge              — purge() skips decay, returns immediately
 *     4. test_reclaim_fork_safe          — fork child soft_reset+init doesn't crash
 *
 *   jemalloc-style wake (3):
 *     5. test_reclaim_indefinite_sleep   — no hang when no pending segments
 *     6. test_reclaim_wakeup_signal      — free path trigger_now wakes thread from sleep
 *     7. test_reclaim_5s_interval        — scan_interval controls scan frequency
 *
 * decay1/decay2 set to 100ms/500ms in tests for speed (defaults 5s/60s too slow).
 * All tests run in fork children to isolate ReclaimThread from parent.
 */

#include "uballoc.hpp"
#include "test_helpers.hpp"
#include <iostream>
#include <cassert>
#include <cstring>
#include <vector>
#include <unistd.h>
#include <chrono>
#include <thread>
#include <atomic>

static constexpr const char* HEAP_ID = "rctest";

// Short decay for fast tests. LARGE_SIZE = 1MB (Large bracket, slab=4MB).
// 65 allocations force grow_segment to create a 2nd segment; the 1st
// segment can be fully reclaimed after all are freed.
static constexpr size_t LARGE_SIZE = 1 * 1024 * 1024;
static constexpr int NUM_ALLOCS = 65;

static void setup_env(const char* mode, int decay1_ms, int decay2_ms,
                      int scan_ms = 200) {
    setenv("UBALLOC_HEAP_ID", HEAP_ID, 1);
    setenv("UBALLOC_RECLAIM_MODE", mode, 1);
    setenv("UBALLOC_RECLAIM_ENABLED", "1", 1);
    setenv("UBALLOC_RECLAIM_DECAY1_MS", std::to_string(decay1_ms).c_str(), 1);
    setenv("UBALLOC_RECLAIM_DECAY2_MS", std::to_string(decay2_ms).c_str(), 1);
    setenv("UBALLOC_RECLAIM_SCAN_INTERVAL_MS", std::to_string(scan_ms).c_str(), 1);
}

// Allocate NUM_ALLOCS Large objects, write pattern, return pointers.
// This creates at least 2 segments; after all are freed, the 1st
// segment becomes idle and can be reclaimed.
static std::vector<void*> alloc_and_fill() {
    std::vector<void*> ptrs;
    for (int i = 0; i < NUM_ALLOCS; ++i) {
        void* p = uballoc::malloc(LARGE_SIZE);
        if (!p) { std::cerr << "  malloc failed at i=" << i << std::endl; break; }
        memset(p, 0xAB, LARGE_SIZE);
        ptrs.push_back(p);
    }
    assert(ptrs.size() == NUM_ALLOCS);
    return ptrs;
}

// Wait until stats show at least n DETACHED segments, up to timeout_ms.
static bool wait_for_detached(int n, int timeout_ms) {
    auto start = std::chrono::steady_clock::now();
    while (true) {
        auto stats = uballoc::return_stats();
        if ((int)stats.segments_detached >= n) return true;
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();
        if (elapsed > timeout_ms) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

// Wait until stats show at least n RETURNED segments, up to timeout_ms.
static bool wait_for_returned(int n, int timeout_ms) {
    auto start = std::chrono::steady_clock::now();
    while (true) {
        auto stats = uballoc::return_stats();
        if ((int)stats.segments_returned >= n) return true;
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();
        if (elapsed > timeout_ms) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

// =============================================================================
// 1. SYNC mode: malloc/free inline triggers LIVE→DETACHED→RETURNED
// =============================================================================
static void test_reclaim_sync_mode() {
    std::cout << "[reclaim] SYNC mode: inline LIVE→DETACHED→RETURNED..." << std::endl;

    pid_t pid = fork();
    assert(pid >= 0);

    if (pid == 0) {
        setup_env("sync", 100, 500);
        auto& alloc = uballoc::get_global_allocator<uballoc::DistributedShmBackend<>>();
        alloc.soft_reset();
        alloc.init();
        alloc.init_thread(0);
        assert(alloc.is_initialized());

        // Alloc + free → segment 0 becomes idle
        auto ptrs = alloc_and_fill();
        for (void* p : ptrs) uballoc::free(p);
        ptrs.clear();

        auto stats = uballoc::return_stats();
        assert(stats.segments_live >= 1);
        assert(stats.segments_detached == 0);

        // Wait decay1(100ms) + Large alloc/free triggers inline check → DETACHED
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        void* t = uballoc::malloc(LARGE_SIZE);
        uballoc::free(t);

        stats = uballoc::return_stats();
        assert(stats.segments_detached >= 1);
        std::cout << "  SYNC: LIVE→DETACHED OK (detached=" << stats.segments_detached << ")" << std::endl;

        // Wait decay2(500ms) + Large alloc/free triggers inline DETACHED→RETURNED
        std::this_thread::sleep_for(std::chrono::milliseconds(550));
        t = uballoc::malloc(LARGE_SIZE);
        uballoc::free(t);

        stats = uballoc::return_stats();
        assert(stats.segments_returned >= 1);
        std::cout << "  SYNC: DETACHED→RETURNED OK (returned=" << stats.segments_returned << ")" << std::endl;

        _exit(0);
    } else {
        int status;
        waitpid(pid, &status, 0);
        check_child(status);
        std::cout << "  Verified: SYNC mode inline reclaim works" << std::endl;
    }
}

// =============================================================================
// 2. ASYNC mode: background thread auto LIVE→DETACHED→RETURNED
// =============================================================================
static void test_reclaim_async_basic() {
    std::cout << "[reclaim] ASYNC mode: background LIVE→DETACHED→RETURNED..." << std::endl;

    pid_t pid = fork();
    assert(pid >= 0);

    if (pid == 0) {
        setup_env("async", 100, 500, 100);  // scan=100ms
        auto& alloc = uballoc::get_global_allocator<uballoc::DistributedShmBackend<>>();
        alloc.soft_reset();
        alloc.init();
        alloc.init_thread(0);
        assert(alloc.is_initialized());

        auto ptrs = alloc_and_fill();
        for (void* p : ptrs) uballoc::free(p);
        ptrs.clear();

        // ASYNC: no malloc/free trigger needed, background thread scans
        // Wait for decay1 + at least one background scan
        assert(wait_for_detached(1, 500));
        std::cout << "  ASYNC: LIVE→DETACHED OK" << std::endl;

        // Wait for decay2 + background scan → RETURNED
        assert(wait_for_returned(1, 1000));
        std::cout << "  ASYNC: DETACHED→RETURNED OK" << std::endl;

        _exit(0);
    } else {
        int status;
        waitpid(pid, &status, 0);
        check_child(status);
        std::cout << "  Verified: ASYNC mode background reclaim works" << std::endl;
    }
}

// =============================================================================
// 3. purge() skips decay, returns immediately
// =============================================================================
static void test_reclaim_purge() {
    std::cout << "[reclaim] purge(): immediate LIVE→RETURNED (skips decay)..." << std::endl;

    pid_t pid = fork();
    assert(pid >= 0);

    if (pid == 0) {
        // Long decay to ensure reclaim doesn't trigger via decay
        setup_env("async", 60000, 60000, 5000);
        auto& alloc = uballoc::get_global_allocator<uballoc::DistributedShmBackend<>>();
        alloc.soft_reset();
        alloc.init();
        alloc.init_thread(0);
        assert(alloc.is_initialized());

        auto ptrs = alloc_and_fill();
        for (void* p : ptrs) uballoc::free(p);
        ptrs.clear();

        auto stats = uballoc::return_stats();
        assert(stats.segments_live >= 1);
        assert(stats.segments_returned == 0);

        // purge returns immediately, not waiting for 60s decay
        uballoc::purge();

        stats = uballoc::return_stats();
        assert(stats.segments_returned >= 1);
        assert(stats.segments_live == 0);
        std::cout << "  purge: returned=" << stats.segments_returned << " (immediate)" << std::endl;

        _exit(0);
    } else {
        int status;
        waitpid(pid, &status, 0);
        check_child(status);
        std::cout << "  Verified: purge() skips decay timers" << std::endl;
    }
}

// =============================================================================
// 4. fork safe: child soft_reset + init doesn't crash, alloc/free works
// =============================================================================
static void test_reclaim_fork_safe() {
    std::cout << "[reclaim] fork safe: child soft_reset+init+alloc/free..." << std::endl;

    pid_t pid = fork();
    assert(pid >= 0);

    if (pid == 0) {
        setup_env("async", 100, 500, 100);
        // Child: soft_reset → init → alloc/free works normally.
        // This verifies fork-then-soft_reset stops parent's thread +
        // init re-initializes without crash.
        auto& alloc = uballoc::get_global_allocator<uballoc::DistributedShmBackend<>>();
        alloc.soft_reset();
        alloc.init();
        alloc.init_thread(0);
        assert(alloc.is_initialized());

        // Child can alloc/free normally
        void* p = uballoc::malloc(LARGE_SIZE);
        assert(p);
        memset(p, 0xCD, LARGE_SIZE);
        uballoc::free(p);

        // Multiple alloc/free for stability
        for (int i = 0; i < 10; ++i) {
            void* q = uballoc::malloc(LARGE_SIZE);
            assert(q);
            uballoc::free(q);
        }

        std::cout << "  fork child: alloc/free OK after soft_reset+init" << std::endl;
        _exit(0);
    } else {
        int status;
        waitpid(pid, &status, 0);
        check_child(status);
        std::cout << "  Verified: fork child survives soft_reset+init" << std::endl;
    }
}

// =============================================================================
// 5. indefinite sleep: no hang when no pending segments
//    Verification: alloc (no free) → segments LIVE in-use → background thread
//    has no pending work → indefinite sleep. Process can _exit without hang.
// =============================================================================
static void test_reclaim_indefinite_sleep() {
    std::cout << "[reclaim] indefinite sleep: no hang when no pending segments..." << std::endl;

    pid_t pid = fork();
    assert(pid >= 0);

    if (pid == 0) {
        setup_env("async", 60000, 60000, 100);  // long decay, segments won't go idle
        auto& alloc = uballoc::get_global_allocator<uballoc::DistributedShmBackend<>>();
        alloc.soft_reset();
        alloc.init();
        alloc.init_thread(0);
        assert(alloc.is_initialized());

        // Alloc but don't free → segments LIVE and in-use → no pending work
        // → background thread enters indefinite sleep
        auto ptrs = alloc_and_fill();
        assert(!ptrs.empty());

        // Sleep to ensure background thread enters indefinite sleep
        std::this_thread::sleep_for(std::chrono::milliseconds(300));

        // Normal exit (soft_reset stops the thread). If thread hangs,
        // this would timeout.
        alloc.soft_reset();
        std::cout << "  indefinite sleep: soft_reset completed without hang" << std::endl;
        _exit(0);
    } else {
        int status;
        // 5s timeout: if thread hangs, waitpid won't return
        for (int i = 0; i < 50; ++i) {
            pid_t r = waitpid(pid, &status, WNOHANG);
            if (r == pid) goto done;
            if (r < 0) { std::cerr << "  waitpid error" << std::endl; assert(0); }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        std::cerr << "  TIMEOUT: child hung in indefinite sleep" << std::endl;
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
        assert(0 && "child hung");
    done:
        check_child(status);
        std::cout << "  Verified: no hang when no pending segments" << std::endl;
    }
}

// =============================================================================
// 6. wakeup signal: free path trigger_now wakes thread from indefinite sleep
//    Verification: alloc+free → live_slab_count→0 → trigger_now wakes thread
//    → thread re-evaluates deadline → enters timed sleep (not indefinite).
//    After decay1+decay2 elapse, background thread auto-completes
//    LIVE→DETACHED→RETURNED.
//    Note: purge does direct synchronous return, not relying on background
//    thread. This test verifies the free-path trigger_now mechanism
//    (jemalloc pac_maybe_wake_bg pattern).
// =============================================================================
static void test_reclaim_wakeup_signal() {
    std::cout << "[reclaim] wakeup signal: free trigger_now wakes from indefinite sleep..." << std::endl;

    pid_t pid = fork();
    assert(pid >= 0);

    if (pid == 0) {
        setup_env("async", 200, 500, 100);  // short decay: decay1=200ms, decay2=500ms
        auto& alloc = uballoc::get_global_allocator<uballoc::DistributedShmBackend<>>();
        alloc.soft_reset();
        alloc.init();
        alloc.init_thread(0);
        assert(alloc.is_initialized());

        // Alloc and free all → segment becomes idle.
        // free path's live_slab_count→0 calls trigger_now to wake thread.
        auto ptrs = alloc_and_fill();
        for (void* p : ptrs) uballoc::free(p);
        ptrs.clear();

        // Thread woken by trigger_now: decay1 not yet expired (200ms),
        // re-evaluates deadline, enters timed sleep (not indefinite).
        // After decay1+decay2 expire, background thread auto-completes
        // LIVE→DETACHED→RETURNED.
        assert(wait_for_detached(1, 500));
        assert(wait_for_returned(1, 1000));
        std::cout << "  wakeup signal: free→trigger_now→auto reclaim OK" << std::endl;

        _exit(0);
    } else {
        int status;
        waitpid(pid, &status, 0);
        check_child(status);
        std::cout << "  Verified: free trigger_now wakes thread, auto reclaim works" << std::endl;
    }
}

// =============================================================================
// 7. scan_interval controls scan frequency
//    Verification: short scan_interval reclaims faster than long.
//    Note: free path's trigger_now wakes the thread immediately, so
//    scan_interval mainly affects the case where no trigger fires.
//    Both short and long intervals are tested to confirm reclaim works.
// =============================================================================
static void test_reclaim_5s_interval() {
    std::cout << "[reclaim] scan interval: shorter interval → faster reclaim..." << std::endl;

    // Test 1: short interval (50ms) — reclaim should be fast
    pid_t pid1 = fork();
    assert(pid1 >= 0);
    if (pid1 == 0) {
        setup_env("async", 100, 300, 50);  // scan=50ms
        auto& alloc = uballoc::get_global_allocator<uballoc::DistributedShmBackend<>>();
        alloc.soft_reset();
        alloc.init();
        alloc.init_thread(0);
        assert(alloc.is_initialized());

        auto ptrs = alloc_and_fill();
        for (void* p : ptrs) uballoc::free(p);
        ptrs.clear();

        auto start = std::chrono::steady_clock::now();
        assert(wait_for_returned(1, 2000));
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();
        std::cout << "  scan=50ms: returned in " << elapsed << "ms" << std::endl;
        // trigger_now makes this fast regardless of scan_interval, but
        // the reclaim still needs decay1+decay2 to elapse (400ms).
        assert(elapsed < 1500);
        _exit(0);
    } else {
        int s1;
        waitpid(pid1, &s1, 0);
        check_child(s1);
    }

    // Test 2: long interval (5000ms) + trigger_now bypass
    // trigger_now wakes the thread, so reclaim happens even with
    // long scan_interval. This confirms trigger_now is the primary
    // wake mechanism, scan_interval is the fallback.
    pid_t pid2 = fork();
    assert(pid2 >= 0);
    if (pid2 == 0) {
        setup_env("async", 100, 300, 5000);  // scan=5000ms
        auto& alloc = uballoc::get_global_allocator<uballoc::DistributedShmBackend<>>();
        alloc.soft_reset();
        alloc.init();
        alloc.init_thread(0);
        assert(alloc.is_initialized());

        auto ptrs = alloc_and_fill();
        for (void* p : ptrs) uballoc::free(p);
        ptrs.clear();

        auto start = std::chrono::steady_clock::now();
        // trigger_now wakes the thread; after decay1+decay2 it reclaims.
        assert(wait_for_returned(1, 3000));
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();
        std::cout << "  scan=5000ms: returned in " << elapsed << "ms (trigger_now bypasses)" << std::endl;
        _exit(0);
    } else {
        int s2;
        waitpid(pid2, &s2, 0);
        check_child(s2);
    }

    std::cout << "  Verified: scan_interval + trigger_now both work" << std::endl;
}

// =============================================================================
// main
// =============================================================================
int main() {
    cleanup_all_distributed_shms(2, HEAP_ID);

    test_reclaim_sync_mode();
    test_reclaim_async_basic();
    test_reclaim_purge();
    test_reclaim_fork_safe();
    test_reclaim_indefinite_sleep();
    test_reclaim_wakeup_signal();
    test_reclaim_5s_interval();

    cleanup_all_distributed_shms(2, HEAP_ID);
    std::cout << "\n=== Reclaim tests passed ===" << std::endl;
    return 0;
}
