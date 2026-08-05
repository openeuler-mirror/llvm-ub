// SPDX-License-Identifier: Apache-2.0

/**
 * @file published_example.cpp
 * @brief Demonstrates the combined malloc+publish API and blocking lookup.
 *
 * This example shows the simplest way to share data between processes:
 *
 *   void* p = uballoc::malloc(size, type_id);           // allocate + publish
 *   auto info = uballoc::lookup_by_type_blocking(type_id, 30000);  // wait for it
 *
 * P0 and P1 each allocate+publish a block, then blocking-look-up each other's
 * block by type_id. The data-readiness race is handled with an atomic ready
 * flag inside the published struct.
 *
 * Usage:
 *   Node 0: published_example 0
 *   Node 1: published_example 1
 *
 * Stale shm cleanup is the runner's responsibility, not the example's.
 */

#include <uballoc.hpp>
#include <iostream>
#include <cstdlib>
#include <cstring>
#include <cassert>
#include <atomic>
#include <unistd.h>

// Application-defined type ids for published regions.
constexpr uint32_t TYPE_P0_CONFIG = 100;
constexpr uint32_t TYPE_P1_CONFIG = 101;

// The struct we publish. The `ready` flag handles the data-readiness race
// inherent in malloc(size, type_id): the pointer is published before the
// caller initializes the memory, so lookers must spin on `ready`.
struct alignas(64) ConfigBlock {
    std::atomic<uint32_t> ready;
    uint32_t rank;
    uint32_t value_count;
    uint64_t values[8];
};

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <app_pid>" << std::endl;
        std::cerr << "  Environment: UBALLOC_HEAP_ID must be set." << std::endl;
        std::cerr << "  Node 0:  UBALLOC_HEAP_ID=pubex " << argv[0] << " 0" << std::endl;
        std::cerr << "  Node 1:  UBALLOC_HEAP_ID=pubex " << argv[0] << " 1" << std::endl;
        return 1;
    }

    int app_pid = std::atoi(argv[1]);

    const char* heap_id = std::getenv("UBALLOC_HEAP_ID");
    if (!heap_id) {
        std::cerr << "Error: UBALLOC_HEAP_ID not set." << std::endl;
        return 1;
    }

    std::cout << "=== Published Allocator Example ===" << std::endl;
    std::cout << "  UBALLOC_HEAP_ID=" << heap_id << std::endl;
    std::cout << "  app_pid=" << app_pid << std::endl;

    uballoc::init();

    if (!uballoc::is_initialized()) {
        std::cerr << "Error: allocator initialization failed." << std::endl;
        return 1;
    }

    int rank = uballoc::rank();
    uint32_t my_type = (app_pid == 0) ? TYPE_P0_CONFIG : TYPE_P1_CONFIG;
    uint32_t peer_type = (app_pid == 0) ? TYPE_P1_CONFIG : TYPE_P0_CONFIG;

    // --- Allocate + publish in one call ---
    std::cout << "  malloc(" << sizeof(ConfigBlock) << ", type_id=" << my_type << ")..." << std::endl;
    ConfigBlock* cfg = static_cast<ConfigBlock*>(
        uballoc::malloc(sizeof(ConfigBlock), my_type));
    if (!cfg) {
        std::cerr << "  malloc(size, type_id) failed!" << std::endl;
        return 1;
    }
    std::cout << "  Allocated+published at " << cfg << std::endl;

    // Initialize the data AFTER the publish (the race is expected).
    cfg->ready.store(0, std::memory_order_relaxed);
    cfg->rank = static_cast<uint32_t>(app_pid);
    cfg->value_count = 8;
    for (uint32_t i = 0; i < 8; i++) {
        cfg->values[i] = static_cast<uint64_t>(app_pid) * 1000 + i;
    }
    cfg->ready.store(1, std::memory_order_release);

    // --- Blocking lookup for peer's config ---
    std::cout << "  lookup_by_type_blocking(type_id=" << peer_type << ", 30s)..." << std::endl;
    auto info = uballoc::lookup_by_type_blocking(peer_type, 30000);
    if (info.owner_process < 0) {
        std::cerr << "  Timeout: peer did not publish type " << peer_type << std::endl;
        uballoc::free(cfg);
        return 1;
    }

    std::cout << "  Found peer's block: address=" << info.address
              << " size=" << info.size
              << " owner=" << info.owner_process << std::endl;

    // Spin on the peer's ready flag (handles the data-readiness race).
    ConfigBlock* peer_cfg = static_cast<ConfigBlock*>(info.address);
    while (peer_cfg->ready.load(std::memory_order_acquire) != 1) {
        ::usleep(1000);
    }

    std::cout << "  Peer config: rank=" << peer_cfg->rank
              << " values=[" << peer_cfg->values[0] << ", "
              << peer_cfg->values[1] << ", ... "
              << peer_cfg->values[7] << "]" << std::endl;

    assert(peer_cfg->rank == static_cast<uint32_t>(1 - app_pid));
    assert(peer_cfg->value_count == 8);
    for (uint32_t i = 0; i < 8; i++) {
        assert(peer_cfg->values[i] == static_cast<uint64_t>(1 - app_pid) * 1000 + i);
    }
    std::cout << "  Peer data verified OK" << std::endl;

    // --- Non-blocking lookup (should also find it now) ---
    auto info2 = uballoc::lookup_by_type(peer_type);
    assert(info2.owner_process == info.owner_process);
    std::cout << "  Non-blocking lookup_by_type also found it: OK" << std::endl;

    uballoc::free(cfg);
    std::cout << "  Process " << rank << " complete." << std::endl;

    // Keep alive briefly so the peer can finish and exit cleanly before
    // our uballoc destructor runs (avoids teardown races on shared state).
    sleep(1);
    return 0;
}
