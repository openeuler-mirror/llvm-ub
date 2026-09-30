// SPDX-License-Identifier: Apache-2.0

/**
 * @file defrag_example.cpp
 * @brief Demonstrates introspection and defragmentation APIs.
 *
 * Single-process demo (no fork, no remote processes needed).
 *
 * Shows:
 *   1. uballoc::usable_size() — query the usable size of an allocation
 *   2. uballoc::return_stats() — check fragmentation ratio
 *   3. uballoc::defrag_hints() — get live allocations in low-occupancy memory
 *   4. Full defrag workflow: malloc + copy + free to consolidate
 *
 * Usage:
 *   UBALLOC_HEAP_ID=defrag ./defrag_example
 */

#include <uballoc.hpp>
#include <iostream>
#include <cstring>
#include <vector>
#include <cstdlib>

static void section(const char* title) {
    std::cout << "\n---------- " << title << " ----------" << std::endl;
}

static void print_frag_stats(const char* label) {
    auto s = uballoc::return_stats();

    std::cout << "[" << label << "] "
              << "frag_ratio=" << s.fragmentation_ratio
              << std::endl;
}

int main() {
    const char* heap_id = std::getenv("UBALLOC_HEAP_ID");
    if (!heap_id || *heap_id == '\0') {
        std::cerr << "Error: UBALLOC_HEAP_ID is not set." << std::endl;
        std::cerr << "Usage: UBALLOC_HEAP_ID=defrag ./defrag_example" << std::endl;
        return 1;
    }

    uballoc::init();

    // ================================================================
    // Step 1: Allocate objects + demonstrate usable_size
    //
    //   malloc(63) → usable_size returns 64 (size class rounds up)
    //   If the user later needs 64B: usable_size(64) >= 64
    //   → no realloc needed, the extra 1B is already available.
    // ================================================================
    section("Step 1: Allocate 40000 objects + usable_size demo");

    constexpr size_t REQ_SIZE = 63;
    constexpr int N = 40000;  // spans multiple memory regions

    std::vector<void*> ptrs;
    for (int i = 0; i < N; i++) {
        void* p = uballoc::malloc(REQ_SIZE);
        if (!p) { std::cerr << "malloc failed at i=" << i << std::endl; break; }
        memset(p, static_cast<uint8_t>(i & 0xFF), REQ_SIZE);
        ptrs.push_back(p);
    }
    std::cout << "  Allocated " << ptrs.size() << " x " << REQ_SIZE << "B objects" << std::endl;

    size_t us = uballoc::usable_size(ptrs[0]);
    std::cout << "  uballoc::usable_size(ptrs[0]) = " << us
              << " (requested " << REQ_SIZE << "B, usable " << us << "B"
              << ", internal waste " << (us - REQ_SIZE) << "B = "
              << (100.0 * (us - REQ_SIZE) / us) << "%)"
              << std::endl;

    print_frag_stats("after-alloc");

    // ================================================================
    // Step 2: Free 90% of objects — create fragmentation
    //
    //   Keep every 10th object (4000 total), free the rest.
    //   The remaining objects are scattered across memory regions,
    //   each region ~10% occupied → high fragmentation.
    // ================================================================
    section("Step 2: Free 90% of objects (create fragmentation)");

    int freed = 0;
    for (int i = 0; i < N; i++) {
        if (i % 10 != 0) {
            uballoc::free(ptrs[i]);
            ptrs[i] = nullptr;
            freed++;
        }
    }
    std::cout << "  Freed " << freed << " objects, " << (N - freed) << " remain"
              << std::endl;
    print_frag_stats("after-free");

    // ================================================================
    // Step 3: Get defrag_hints — find live allocations in sparse memory
    //
    //   defrag_hints(0.5) returns all live allocations whose memory
    //   region is less than 50% occupied. The app can relocate these
    //   to consolidate memory.
    //
    //   hint.occupancy = fraction of blocks in use within the memory
    //   region that contains this allocation (0.0~1.0). Lower means
    //   more wasted space → higher priority for relocation.
    // ================================================================
    section("Step 3: Get defrag_hints (threshold=0.5)");

    auto hints = uballoc::defrag_hints(0.5);
    std::cout << "  defrag_hints(0.5) returned " << hints.size() << " hints" << std::endl;

    for (size_t i = 0; i < hints.size() && i < 5; i++) {
        std::cout << "  hint[" << i << "]: ptr=" << hints[i].ptr
                  << " size=" << hints[i].size
                  << " occupancy=" << (hints[i].occupancy * 100) << "%"
                  << std::endl;
    }
    if (hints.size() > 5) std::cout << "  ... (" << hints.size() << " total)" << std::endl;

    // ================================================================
    // Step 4: Relocate — consolidate scattered allocations
    //
    //   For each live allocation: allocate new, copy data, free old.
    //   New allocations land in denser regions (allocator picks regions
    //   with free space), freeing up sparse regions for reclamation.
    // ================================================================
    section("Step 4: Relocate scattered allocations");

    size_t relocated = 0;
    for (int i = 0; i < N; i++) {
        if (!ptrs[i]) continue;
        void* old = ptrs[i];
        void* neu = uballoc::malloc(REQ_SIZE);
        if (!neu) break;
        memcpy(neu, old, REQ_SIZE);
        uballoc::free(old);
        ptrs[i] = neu;
        relocated++;
    }
    std::cout << "  Relocated " << relocated << " allocations" << std::endl;
    print_frag_stats("after-defrag");

    // ================================================================
    // Step 5: Purge — reclaim empty memory regions
    //
    //   After relocation, some memory regions are completely empty.
    //   purge() returns them to the OS immediately.
    //   frag_ratio drops because held decreases.
    // ================================================================
    section("Step 5: Purge — reclaim empty regions");

    uballoc::purge();
    print_frag_stats("after-purge");

    // ================================================================
    // Step 6: Verify data integrity
    // ================================================================
    section("Step 6: Verify");

    int live_count = 0;
    bool ok = true;
    for (int i = 0; i < N; i++) {
        if (!ptrs[i]) continue;
        live_count++;
        uint8_t expected = static_cast<uint8_t>(i & 0xFF);
        if (static_cast<uint8_t*>(ptrs[i])[0] != expected) {
            ok = false;
            std::cerr << "  DATA CORRUPTION at ptrs[" << i << "]" << std::endl;
        }
    }
    if (ok) {
        std::cout << "  Data integrity: OK (" << live_count << " objects verified)"
                  << std::endl;
    }

    // Cleanup
    for (void* p : ptrs) {
        if (p) uballoc::free(p);
    }
    uballoc::purge();

    std::cout << "\n=== Defrag example complete ===" << std::endl;
    return 0;
}
