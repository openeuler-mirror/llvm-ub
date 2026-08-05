// SPDX-License-Identifier: Apache-2.0

/**
 * @file distributed_example.cpp
 * @brief Basic multi-process allocator demo with init().
 *
 * Stale shm cleanup is the runner's responsibility, not the example's.
 */

#include <uballoc.hpp>
#include <iostream>
#include <cstdlib>
#include <cstring>
#include <cassert>
#include <vector>
#include <unistd.h>

static void write_pattern(void* ptr, size_t size, uint8_t pattern) {
    memset(ptr, pattern, size);
}

static void verify_pattern(void* ptr, size_t size, uint8_t expected) {
    for (size_t i = 0; i < size; i++) {
        auto actual = static_cast<uint8_t*>(ptr)[i];
        if (actual != expected) {
            std::cerr << "VERIFY FAIL at offset " << i
                      << ": expected=0x" << std::hex << (int)expected
                      << " actual=0x" << (int)actual << std::dec << std::endl;
            return;
        }
    }
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <app_pid>" << std::endl;
        std::cerr << std::endl;
        std::cerr << "Environment: UBALLOC_HEAP_ID must be set." << std::endl;
        std::cerr << std::endl;
        std::cerr << "Two-node deployment:" << std::endl;
        std::cerr << "  Node 0:  UBALLOC_HEAP_ID=heap " << argv[0] << " 0" << std::endl;
        std::cerr << "  Node 1:  UBALLOC_HEAP_ID=heap " << argv[0] << " 1" << std::endl;
        std::cerr << std::endl;
        std::cerr << "Start process 0 first. It creates shm objects and waits for process 1." << std::endl;
        std::cerr << "Then start process 1 on the other node. It creates its shm and attaches to P0." << std::endl;
        std::cerr << "Both processes then allocate, verify, and free memory independently." << std::endl;
        return 1;
    }

    int app_pid = std::atoi(argv[1]);

    const char* heap_id = std::getenv("UBALLOC_HEAP_ID");
    if (!heap_id) {
        std::cerr << "Error: UBALLOC_HEAP_ID not set." << std::endl;
        std::cerr << "Usage: export UBALLOC_HEAP_ID=heap" << std::endl;
        return 1;
    }

    std::cout << "=== Distributed Allocator Example ===" << std::endl;
    std::cout << "  UBALLOC_HEAP_ID=" << heap_id << std::endl;
    std::cout << "  app_pid=" << app_pid << std::endl;

    uballoc::init();

    if (!uballoc::is_initialized()) {
        std::cerr << "Error: allocator initialization failed." << std::endl;
        std::cerr << "  Make sure all processes start within the 30-second window." << std::endl;
        return 1;
    }

    int rank = uballoc::rank();
    std::cout << "  Allocator initialized (rank=" << rank
              << " is_owner=" << uballoc::is_owner() << ")" << std::endl;

    constexpr int N_SMALL = 50;
    constexpr int N_LARGE = 10;
    constexpr size_t SMALL_SIZE = 64;
    constexpr size_t LARGE_SIZE = 4096;

    std::vector<void*> small_allocs;
    std::vector<void*> large_allocs;

    uint8_t small_pattern = static_cast<uint8_t>(0xA0 | rank);
    uint8_t large_pattern = static_cast<uint8_t>(0xB0 | rank);

    std::cout << "  Allocating " << N_SMALL << " small blocks (" << SMALL_SIZE << " bytes each)..." << std::endl;
    for (int i = 0; i < N_SMALL; ++i) {
        void* p = uballoc::malloc(SMALL_SIZE);
        if (!p) {
            std::cerr << "  malloc failed for small block " << i << std::endl;
            return 1;
        }
        write_pattern(p, SMALL_SIZE, small_pattern);
        small_allocs.push_back(p);
    }

    std::cout << "  Allocating " << N_LARGE << " large blocks (" << LARGE_SIZE << " bytes each)..." << std::endl;
    for (int i = 0; i < N_LARGE; ++i) {
        void* p = uballoc::malloc(LARGE_SIZE);
        if (!p) {
            std::cerr << "  malloc failed for large block " << i << std::endl;
            return 1;
        }
        write_pattern(p, LARGE_SIZE, large_pattern);
        large_allocs.push_back(p);
    }

    std::cout << "  Verifying integrity..." << std::endl;
    for (auto* p : small_allocs) {
        verify_pattern(p, SMALL_SIZE, small_pattern);
    }
    for (auto* p : large_allocs) {
        verify_pattern(p, LARGE_SIZE, large_pattern);
    }
    std::cout << "  All allocations verified OK" << std::endl;

    constexpr uint32_t TYPE_RING_BUFFER = 1;
    constexpr uint32_t TYPE_SHARED_STATE = 2;
    constexpr uint32_t TYPE_REMOTE_FREE_BLOCK = 3;
    uint32_t my_type = (rank == 0) ? TYPE_RING_BUFFER : TYPE_SHARED_STATE;
    uint32_t peer_type = (rank == 0) ? TYPE_SHARED_STATE : TYPE_RING_BUFFER;

    std::cout << "  malloc(" << SMALL_SIZE << ", type_id=" << my_type << ")..." << std::endl;
    void* published_ptr = uballoc::malloc(SMALL_SIZE, my_type);
    if (!published_ptr) {
        std::cerr << "  malloc(size, type_id) failed" << std::endl;
        return 1;
    }
    write_pattern(published_ptr, SMALL_SIZE, small_pattern);
    std::cout << "  Allocated+published at " << published_ptr << std::endl;

    std::cout << "  lookup_by_type_blocking(type_id=" << peer_type << ", 30s)..." << std::endl;
    auto info = uballoc::lookup_by_type_blocking(peer_type, 30000);
    if (info.owner_process >= 0) {
        std::cout << "  Found peer's block: address=" << info.address
                  << " size=" << info.size
                  << " owner_process=" << info.owner_process << std::endl;
        uint8_t expected = static_cast<uint8_t>(0xA0 | (1 - rank));
        verify_pattern(info.address, info.size, expected);
    } else {
        std::cerr << "  Timeout waiting for peer's type " << peer_type << std::endl;
    }

    auto addr_info = uballoc::lookup_by_address(published_ptr);
    std::cout << "  lookup_by_address(" << published_ptr << ") -> owner_process="
              << addr_info.owner_process << " type_id=" << addr_info.type_id << std::endl;

    std::cout << "  --- Remote free demo ---" << std::endl;
    if (rank == 0) {
        void* remote_free_ptr = uballoc::malloc(SMALL_SIZE, TYPE_REMOTE_FREE_BLOCK);
        if (!remote_free_ptr) {
            std::cerr << "  malloc(size, type_id) failed for remote-free block" << std::endl;
            return 1;
        }
        write_pattern(remote_free_ptr, SMALL_SIZE, static_cast<uint8_t>(0xC0));
        std::cout << "  P0 allocated+published remote-free block at " << remote_free_ptr << std::endl;
    } else {
        auto remote_info = uballoc::lookup_by_type_blocking(TYPE_REMOTE_FREE_BLOCK, 30000);
        if (remote_info.owner_process >= 0) {
            std::cout << "  P1 found remote-free block from P0: address=" << remote_info.address
                      << " owner_process=" << remote_info.owner_process << std::endl;
            verify_pattern(remote_info.address, remote_info.size, static_cast<uint8_t>(0xC0));
            std::cout << "  P1 freeing P0's block (cross-process remote free)..." << std::endl;
            uballoc::free(remote_info.address);
            std::cout << "  Remote free complete" << std::endl;
        } else {
            std::cout << "  P1: remote-free block not found" << std::endl;
        }
    }

    uballoc::free(published_ptr);

    std::cout << "  Freeing " << small_allocs.size() << " small blocks..." << std::endl;
    for (auto* p : small_allocs) {
        uballoc::free(p);
    }

    std::cout << "  Freeing " << large_allocs.size() << " large blocks..." << std::endl;
    for (auto* p : large_allocs) {
        uballoc::free(p);
    }

    std::cout << "  All blocks freed" << std::endl;

    std::cout << "  Process " << rank << " complete." << std::endl;

    // Keep alive briefly so the peer can finish and exit cleanly before
    // our uballoc destructor runs (avoids teardown races on shared state).
    sleep(2);

    return 0;
}
