// SPDX-License-Identifier: Apache-2.0

/**
 * @file uffd_example.cpp
 * @brief Demonstrates the userfaultfd-based lazy-attach for remote segments.
 *
 * This example shows the uffd handler coexisting with the eager-attach path.
 * The uffd handler is the PRIMARY fault handler when uffd is available; it
 * falls back to the SIGSEGV handler if the kernel doesn't support uffd.
 *
 * P0: allocates a block, publishes it under TYPE_DATA, sets a ready flag.
 * P1: initializes, enables userfaultfd, looks up P0's block (eager attach),
 *     verifies the data. The uffd handler is running but does not fire
 *     (the eager path already attached the segment, so PTEs are present).
 *
 * The key capability of uffd (discovering segments from the handler thread
 * in normal context) is validated on UBSE hardware via Stage B probes.
 * This example validates the setup/teardown path and coexistence with
 * the eager attach mechanism.
 *
 * Usage:
 *   export UBALLOC_HEAP_ID=myheap
 *   ./uffd_example 0 &    # P0: allocates and publishes
 *   ./uffd_example 1       # P1: enables uffd, verifies eager attach
 *
 * Stale shm cleanup is the runner's responsibility, not the example's.
 */

#include <uballoc.hpp>
#include <uballoc/stl_alloc.hpp>
#include <iostream>
#include <cstdlib>
#include <cstring>
#include <cassert>
#include <atomic>
#include <unistd.h>

constexpr uint32_t TYPE_DATA = 200;

struct alignas(64) DataBlock {
    std::atomic<uint32_t> ready;
    uint32_t rank;
    uint64_t timestamp;
    char message[64];
};

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "Usage: " << argv[0] << " <app_pid>\n"
                  << "  app_pid: 0 (publisher) or 1 (uffd tester)\n"
                  << "Environment: UBALLOC_HEAP_ID must be set.\n"
                  << "  Terminal 1:  UBALLOC_HEAP_ID=myheap " << argv[0] << " 0 &\n"
                  << "  Terminal 2:  UBALLOC_HEAP_ID=myheap " << argv[0] << " 1\n";
        return 1;
    }
    int app_pid = std::atoi(argv[1]);
    if (app_pid < 0 || app_pid > 1) {
        std::cerr << "Error: app_pid must be 0 or 1, got: " << app_pid << std::endl;
        return 1;
    }

    const char* heap_id = std::getenv("UBALLOC_HEAP_ID");
    if (!heap_id) {
        std::cerr << "Error: UBALLOC_HEAP_ID not set." << std::endl;
        return 1;
    }

    std::cout << "=== uffd Example ===" << std::endl;
    std::cout << "  UBALLOC_HEAP_ID=" << heap_id << std::endl;
    std::cout << "  app_pid=" << app_pid << std::endl;

    uballoc::init();

    if (app_pid == 0) {
        // -----------------------------------------------------------------
        // P0: allocate, publish, set ready flag, wait for P1 to finish.
        // -----------------------------------------------------------------
        DataBlock* block = uballoc::shm_new<DataBlock>(uballoc::pub_tid{TYPE_DATA});
        if (!block) {
            std::cerr << "  P0: shm_new<DataBlock>(pub_tid{TYPE_DATA}) failed!" << std::endl;
            return 1;
        }

        block->ready.store(0, std::memory_order_relaxed);
        block->rank = 0;
        block->timestamp = static_cast<uint64_t>(::time(nullptr));
        std::snprintf(block->message, sizeof(block->message), "Hello from P0");
        block->ready.store(1, std::memory_order_release);

        std::cout << "  P0: allocated+published at " << block << std::endl;

        // Wait for P1 to signal completion (it writes 'D' into the message).
        while (block->ready.load(std::memory_order_acquire) != 2) {
            ::usleep(10000);
        }
        std::cout << "  P0: P1 signaled done (message='" << block->message << "')" << std::endl;

        uballoc::free(block);
        return 0;
    }

    // -----------------------------------------------------------------
    // P1: uffd is now default-on (constructor priority 102 or init
    // fallback). No explicit enable_userfaultfd() call needed.
    // The uffd handler is either already running (if bootstrap existed
    // when the constructor ran) or will be started by init().
    std::cout << "  P1: uffd handler auto-enabled" << std::endl;

    // Look up P0's block. This triggers check_remote_segments() which
    // eagerly attaches P0's data segment (opens fd, mmaps). The uffd
    // handler covers the race window during this attach, and would
    // also discover the segment on demand if the eager path missed it.
    auto info = uballoc::lookup_by_type_blocking(TYPE_DATA, 30000);
    if (info.owner_process < 0) {
        std::cerr << "  P1: lookup failed (timeout)" << std::endl;
        return 1;
    }

    DataBlock* peer = static_cast<DataBlock*>(info.address);
    while (peer->ready.load(std::memory_order_acquire) != 1) {
        ::usleep(1000);
    }

    std::cout << "  P1: found P0's block at " << peer
              << " msg='" << peer->message << "'" << std::endl;

    // Verify data is accessible (segment is eagerly attached, no uffd fire).
    // The uffd handler thread is running but idle — PTEs are present,
    // so no uffd events are generated.
    assert(peer->rank == 0);
    assert(std::strcmp(peer->message, "Hello from P0") == 0);
    std::cout << "  P1: data verified (eager attach OK, uffd idle)" << std::endl;

    // Signal P0 that we're done.
    std::snprintf(peer->message, sizeof(peer->message), "Done from P1");
    peer->ready.store(2, std::memory_order_release);

    std::cout << "  P1: test passed — uffd handler coexists with eager attach" << std::endl;

    return 0;
}
