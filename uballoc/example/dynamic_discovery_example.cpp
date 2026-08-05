// SPDX-License-Identifier: Apache-2.0

/**
 * @file dynamic_discovery_example.cpp
 * @brief Demonstrates env-var-driven dynamic membership discovery.
 *
 * No parameters needed — just set UBALLOC_HEAP_ID and call uballoc::init().
 * Rank and membership are discovered at runtime via the bootstrap PID table.
 *
 * Usage:
 *   export UBALLOC_HEAP_ID=myheap
 *   ./dynamic_discovery_example 0 &     # app_pid=0
 *   ./dynamic_discovery_example 1       # app_pid=1
 *   ./dynamic_discovery_example 2       # app_pid=2 (late join)
 *
 * Each process allocates a block, publishes it, and looks up
 * the other process's published block. The application process
 * identity (app_pid) is provided via argv[1]; uballoc's internal
 * rank (auto-discovered) is separate and not exposed to the app.
 *
 * Stale shm cleanup is the runner's responsibility, not the example's.
 * If a previous run crashed, delete stale shm objects before rerunning.
 */

#include <uballoc.hpp>
#include <iostream>
#include <cstdlib>
#include <cstring>
#include <cassert>
#include <atomic>
#include <unistd.h>

constexpr uint32_t TYPE_P0_DATA = 200;
constexpr uint32_t TYPE_P1_DATA = 201;

struct alignas(64) DataBlock {
    std::atomic<uint32_t> ready;
    uint32_t rank;
    uint64_t timestamp;
    char message[64];
};

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "Usage: " << argv[0] << " <app_pid>\n"
                  << "  app_pid: application process id (0, 1, or 2)\n"
                  << "Environment: UBALLOC_HEAP_ID must be set.\n"
                  << "  Terminal 1:  UBALLOC_HEAP_ID=myheap " << argv[0] << " 0 &\n"
                  << "  Terminal 2:  UBALLOC_HEAP_ID=myheap " << argv[0] << " 1\n"
                  << "  Terminal 3:  UBALLOC_HEAP_ID=myheap " << argv[0] << " 2\n";
        return 1;
    }
    int app_pid = std::atoi(argv[1]);
    if (app_pid < 0 || app_pid > 2) {
        std::cerr << "Error: app_pid must be 0, 1, or 2, got: " << app_pid << std::endl;
        return 1;
    }

    const char* heap_id = std::getenv("UBALLOC_HEAP_ID");
    if (!heap_id) {
        std::cerr << "Error: UBALLOC_HEAP_ID not set." << std::endl;
        std::cerr << "Usage: export UBALLOC_HEAP_ID=myheap\n"
                  << "  Terminal 1:  UBALLOC_HEAP_ID=myheap " << argv[0] << " 0 &\n"
                  << "  Terminal 2:  UBALLOC_HEAP_ID=myheap " << argv[0] << " 1\n";
        return 1;
    }

    std::cout << "=== Dynamic Discovery Example ===" << std::endl;
    std::cout << "  UBALLOC_HEAP_ID=" << heap_id << std::endl;
    std::cout << "  app_pid=" << app_pid << std::endl;

    uballoc::init();

    uint32_t my_type   = (app_pid == 0) ? TYPE_P0_DATA : TYPE_P1_DATA;
    uint32_t peer_type = (app_pid == 0) ? TYPE_P1_DATA : TYPE_P0_DATA;

    DataBlock* block = static_cast<DataBlock*>(
        uballoc::malloc(sizeof(DataBlock), my_type));
    if (!block) {
        std::cerr << "  malloc(size, type_id) failed!" << std::endl;
        return 1;
    }

    block->ready.store(0, std::memory_order_relaxed);
    block->rank = static_cast<uint32_t>(app_pid);
    block->timestamp = static_cast<uint64_t>(::time(nullptr));
    std::snprintf(block->message, sizeof(block->message),
                  "Hello from P%d", app_pid);
    block->ready.store(1, std::memory_order_release);

    std::cout << "  Allocated+published at " << block
              << " app_pid=" << app_pid
              << " uballoc_total=" << uballoc::total_processes() << std::endl;

    auto info = uballoc::lookup_by_type_blocking(peer_type, 30000);
    if (info.owner_process >= 0 && info.address != block) {
        DataBlock* peer = static_cast<DataBlock*>(info.address);
        while (peer->ready.load(std::memory_order_acquire) != 1) {
            ::usleep(1000);
        }
        std::cout << "  Found peer: owner=P" << info.owner_process
                  << " app_pid=" << peer->rank
                  << " msg='" << peer->message << "'" << std::endl;
    } else if (info.address == block) {
        std::cout << "  Found own block (no peers yet)" << std::endl;
    } else {
        std::cout << "  No peer found within timeout" << std::endl;
    }

    std::cout << "  Process " << app_pid << " complete." << std::endl;

    uballoc::free(block);

    // Keep alive briefly so the peer can finish and exit cleanly before
    // our uballoc destructor runs (avoids teardown races on shared state).
    sleep(2);

    return 0;
}
