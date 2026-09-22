// SPDX-License-Identifier: Apache-2.0

/**
 * @file reclaim_example.cpp
 * @brief Demonstrates memory reclaim in SYNC and ASYNC modes (two-process).
 *
 * P0 allocates and publishes memory, then frees it. After decay timers
 * elapse, segments transition LIVE→DETACHED→RETURNED (memory returned
 * to OS). P1 discovers P0's published pointer, reads it, and frees it
 * (cross-process free), verifying that reclaim works correctly even when
 * a borrower has touched the segment.
 *
 * Usage:
 *   Terminal 1:  UBALLOC_HEAP_ID=reclaim ./reclaim_example 0 [sync|async]
 *   Terminal 2:  UBALLOC_HEAP_ID=reclaim ./reclaim_example 1 [sync|async]
 *
 *   Mode defaults to "sync". Both sides must use the same mode.
 *
 * Steps (P0 perspective):
 *   1. P0 allocates 65 x 1MB (Large bracket), publishes one for P1
 *   2. P0 frees all → segment becomes idle (live_slab_count→0)
 *   3. Wait decay1 (1s) → LIVE→DETACHED
 *   4. Wait decay2 (1s) → DETACHED→RETURNED (memory returned to OS)
 *   5. P0 allocates fresh, frees, purge() → immediate return (skips decay)
 *
 * P1 perspective:
 *   - Discovers P0's published pointer via lookup_by_type_blocking
 *   - Reads and verifies the data
 *   - Frees it (cross-process free)
 *   - Waits for P0 to finish, then exits
 *
 * Environment variables (this demo sets its own defaults):
 *   UBALLOC_RECLAIM_MODE=sync|async
 *   UBALLOC_RECLAIM_ENABLED=1
 *   UBALLOC_RECLAIM_DECAY1_MS=1000
 *   UBALLOC_RECLAIM_DECAY2_MS=1000
 *   UBALLOC_RECLAIM_SCAN_INTERVAL_MS=500 (async only)
 */

#include <uballoc.hpp>
#include <iostream>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <unistd.h>
#include <chrono>
#include <thread>
#include <string>
#include <atomic>

constexpr uint32_t TYPE_P0_DATA = 100;
constexpr uint32_t TYPE_P0_DONE = 200;
constexpr uint32_t TYPE_P1_DONE = 201;

static bool ASYNC = false;

static void section(const char* title) {
    std::cout << "\n---------- " << title << " ----------" << std::endl;
}

static void print_stats(const char* label) {
    uballoc::ReturnStats stats = uballoc::return_stats();
    uint64_t held = (stats.requested_from_os_bytes >= stats.returned_to_os_bytes)
        ? (stats.requested_from_os_bytes - stats.returned_to_os_bytes) : 0;
    uint64_t in_use = (stats.allocated_to_app_bytes >= stats.freed_from_app_bytes)
        ? (stats.allocated_to_app_bytes - stats.freed_from_app_bytes) : 0;
    double eff = (held > 0) ? 100.0 * static_cast<double>(in_use) / static_cast<double>(held) : 0.0;
    std::cout << "[" << label << "] "
              << "live=" << stats.segments_live
              << " detached=" << stats.segments_detached
              << " returned=" << stats.segments_returned
              << " (total_detached=" << stats.total_detached_count
              << " total_returned=" << stats.total_returned_count << ")"
              << " | held=" << held / (1024*1024) << "MB"
              << " in_use=" << in_use / (1024*1024) << "MB"
              << " efficiency=" << eff << "%"
              << std::endl;
}

static void sleep_ms(int ms) {
    usleep(ms * 1000);
}

// In SYNC mode, a Large-bracket alloc+free nudges check_one_segment_for_reclaim.
static void trigger_reclaim_scan(size_t large_size) {
    void* p = uballoc::malloc(large_size);
    if (p) uballoc::free(p);
}

constexpr size_t LARGE_SIZE = 1 * 1024 * 1024;  // 1MB
constexpr int NUM_ALLOCS = 65;

// ─── P0: lender (allocates, publishes, frees, reclaims) ──────────────

static int run_p0() {
    setenv("UBALLOC_RECLAIM_MODE", ASYNC ? "async" : "sync", 1);
    setenv("UBALLOC_RECLAIM_ENABLED", "1", 1);
    setenv("UBALLOC_RECLAIM_DECAY1_MS", "1000", 1);
    setenv("UBALLOC_RECLAIM_DECAY2_MS", "1000", 1);
    if (ASYNC) {
        setenv("UBALLOC_RECLAIM_SCAN_INTERVAL_MS", "500", 1);
    }

    std::cout << "=== P0 reclaim demo — "
              << (ASYNC ? "ASYNC" : "SYNC")
              << " ===" << std::endl;

    uballoc::init();
    // init_thread(0) is intentionally not called — init() auto-assigns the
    // correct thread id based on rank.

    section("Step 1: Allocate + publish for P1");
    std::vector<void*> ptrs;
    for (int i = 0; i < NUM_ALLOCS; ++i) {
        void* p = uballoc::malloc(LARGE_SIZE);
        if (!p) { std::cerr << "  malloc failed at i=" << i << std::endl; break; }
        memset(p, 0xAB, LARGE_SIZE);
        ptrs.push_back(p);
    }
    std::cout << "  Allocated " << ptrs.size() << " objects" << std::endl;

    // Publish the first allocation for P1 to discover
    uballoc::publish(ptrs[0], LARGE_SIZE, TYPE_P0_DATA);
    std::cout << "  Published ptr=" << ptrs[0] << " type=" << TYPE_P0_DATA << std::endl;
    print_stats("after-alloc");

    section("Step 2: Free all — segment becomes idle");
    // Don't free the published one yet — P1 needs to read it first.
    // Wait for P1 to signal it has read the data.
    std::cout << "  Waiting for P1 to read published data..." << std::endl;
    auto info = uballoc::lookup_by_type_blocking(TYPE_P1_DONE, 30000);
    if (info.owner_process < 0) {
        std::cerr << "  P1 did not respond in time" << std::endl;
        return 1;
    }
    std::cout << "  P1 signaled done" << std::endl;

    // Now safe to free everything (including the published pointer)
    for (void* p : ptrs) uballoc::free(p);
    ptrs.clear();
    print_stats("after-free");

    // ─── Step 3: LIVE → DETACHED (decay1) ────────────────────────────
    section(ASYNC ? "Step 3 (async): wait decay1 → LIVE→DETACHED"
                  : "Step 3 (sync):  wait decay1 → LIVE→DETACHED");
    if (ASYNC) {
        sleep_ms(1700);
    } else {
        sleep_ms(1100);
        trigger_reclaim_scan(LARGE_SIZE);
    }
    print_stats("after-decay1");

    // ─── Step 4: DETACHED → RETURNED (decay2) ────────────────────────
    section(ASYNC ? "Step 4 (async): wait decay2 → DETACHED→RETURNED"
                  : "Step 4 (sync):  wait decay2 → DETACHED→RETURNED");
    if (ASYNC) {
        sleep_ms(1200);
    } else {
        sleep_ms(1100);
        trigger_reclaim_scan(LARGE_SIZE);
    }
    print_stats("after-decay2");

    // ─── Step 5: purge() skips both decays ────────────────────────────
    section("Step 5: purge() — immediate return (skips decay)");
    std::vector<void*> fresh;
    for (int i = 0; i < NUM_ALLOCS; ++i) {
        void* p = uballoc::malloc(LARGE_SIZE);
        if (!p) break;
        memset(p, 0xCD, LARGE_SIZE);
        fresh.push_back(p);
    }
    for (void* p : fresh) uballoc::free(p);
    fresh.clear();
    print_stats("before-purge");
    uballoc::purge();
    print_stats("after-purge");

    // Signal P1 that P0 is done
    void* done_ptr = uballoc::malloc(64, TYPE_P0_DONE);
    if (done_ptr) {
        memset(done_ptr, 0, 64);
    }
    std::cout << "\n  P0 complete." << std::endl;
    sleep_ms(2000);  // keep alive for P1 to discover TYPE_P0_DONE
    uballoc::reset();
    return 0;
}

// ─── P1: borrower (discovers, reads, frees, verifies) ────────────────

static int run_p1() {
    setenv("UBALLOC_RECLAIM_MODE", ASYNC ? "async" : "sync", 1);
    setenv("UBALLOC_RECLAIM_ENABLED", "1", 1);
    setenv("UBALLOC_RECLAIM_DECAY1_MS", "1000", 1);
    setenv("UBALLOC_RECLAIM_DECAY2_MS", "1000", 1);
    if (ASYNC) {
        setenv("UBALLOC_RECLAIM_SCAN_INTERVAL_MS", "500", 1);
    }

    std::cout << "=== P1 reclaim demo — "
              << (ASYNC ? "ASYNC" : "SYNC")
              << " ===" << std::endl;

    uballoc::init();
    // Don't call init_thread(0) — init() calls ensure_thread_init() which
    // auto-assigns the correct thread id based on rank.

    section("P1: Discover P0's published data");
    auto info = uballoc::lookup_by_type_blocking(TYPE_P0_DATA, 30000);
    if (info.owner_process < 0) {
        std::cerr << "  P0 did not publish in time" << std::endl;
        return 1;
    }
    std::cout << "  Found P0 data: ptr=" << info.address
              << " size=" << info.size << std::endl;

    // Verify data integrity
    auto* data = static_cast<uint8_t*>(info.address);
    bool ok = true;
    for (size_t i = 0; i < info.size; ++i) {
        if (data[i] != 0xAB) { ok = false; break; }
    }
    std::cout << "  Data integrity: " << (ok ? "OK" : "FAIL") << std::endl;

    // Signal P0 that we've read the data
    void* p1_done = uballoc::malloc(64, TYPE_P1_DONE);
    if (p1_done) {
        memset(p1_done, 0, 64);
    }
    std::cout << "  P1 signaled done to P0" << std::endl;

    // Wait for P0 to finish reclaim + purge
    std::cout << "  Waiting for P0 to complete..." << std::endl;
    auto done = uballoc::lookup_by_type_blocking(TYPE_P0_DONE, 30000);
    if (done.owner_process < 0) {
        std::cerr << "  P0 did not finish in time" << std::endl;
        return 1;
    }
    std::cout << "  P0 completed." << std::endl;
    print_stats("p1-final");

    uballoc::reset();
    return 0;
}

// ─── main ──────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <app_pid> [sync|async]" << std::endl;
        std::cerr << "  app_pid: 0 (P0, lender) or 1 (P1, borrower)" << std::endl;
        std::cerr << "  mode: sync (default) or async" << std::endl;
        std::cerr << "Examples:" << std::endl;
        std::cerr << "  Terminal 1:  UBALLOC_HEAP_ID=reclaim " << argv[0] << " 0 async" << std::endl;
        std::cerr << "  Terminal 2:  UBALLOC_HEAP_ID=reclaim " << argv[0] << " 1 async" << std::endl;
        return 1;
    }

    int rank = std::atoi(argv[1]);
    if (rank != 0 && rank != 1) {
        std::cerr << "Error: app_pid must be 0 or 1" << std::endl;
        return 1;
    }

    // Parse mode (argv[2]), default sync
    if (argc > 2) {
        std::string m = argv[2];
        if (m == "async" || m == "ASYNC") ASYNC = true;
        else if (m == "sync" || m == "SYNC") ASYNC = false;
        else {
            std::cerr << "Error: mode must be 'sync' or 'async'" << std::endl;
            return 1;
        }
    }

    const char* heap_id = std::getenv("UBALLOC_HEAP_ID");
    if (!heap_id || !*heap_id) {
        std::cerr << "Error: UBALLOC_HEAP_ID not set" << std::endl;
        return 1;
    }

    if (rank == 0) {
        return run_p0();
    } else {
        return run_p1();
    }
}
