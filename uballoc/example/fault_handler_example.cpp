// SPDX-License-Identifier: Apache-2.0

/**
 * @file fault_handler_example.cpp
 * @brief Demonstrates uffd lazy-attach for a remote segment not yet eager-attached.
 *
 * P0: allocates block A (small bracket), publishes it. After P1 confirms
 *     receipt, allocates block B (huge bracket, 32KB) in a NEW data segment
 *     that P1 hasn't seen. Stores block B's address in block A.
 * P1: looks up block A (eager attach). Then, WITHOUT calling lookup_by_type
 *     (which would trigger check_remote_segments and eagerly attach the new
 *     segment), accesses block B directly. The uffd handler fires (missing
 *     PTE in P0's huge-bracket VA range), discovers the segment from P0's
 *     metadata segment directory, attaches it, and the access succeeds.
 *
 * This tests the key uffd capability: discovering + attaching remote segments
 * on demand from the handler thread (normal context, not signal context).
 * The SIGSEGV handler cannot do this (ubs_mem_shm_attach is not async-signal-
 * safe), so this test requires uffd to be running. If uffd is unavailable, the
 * access at step 6 will crash with SIGSEGV — that is expected.
 *
 * Usage:
 *   export UBALLOC_HEAP_ID=myheap
 *   ./fault_handler_example 0 &    # P0: allocates A, then B
 *   ./fault_handler_example 1       # P1: eager-attach A, lazy-attach B
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
constexpr size_t BLOCK_B_SIZE = 32768;
constexpr uint8_t BLOCK_B_PATTERN = 0xBB;

struct alignas(64) DataBlock {
    std::atomic<uint32_t> ready;   // handshake: 0=init 1=A ready 2=P1 got A 3=B ready 4=done
    uint32_t rank;
    uint64_t block_b_addr;          // block B's address (set by P0 when ready=3)
    char message[64];
};

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "Usage: " << argv[0] << " <app_pid>\n"
                  << "  app_pid: 0 (publisher) or 1 (lazy-attach tester)\n"
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

    std::cout << "=== Fault Handler Example (uffd lazy-attach) ===" << std::endl;
    std::cout << "  UBALLOC_HEAP_ID=" << heap_id << std::endl;
    std::cout << "  app_pid=" << app_pid << std::endl;

    uballoc::init();

    if (app_pid == 0) {
        // -----------------------------------------------------------------
        // P0: allocate block A (small), publish. After P1 confirms,
        // allocate block B (huge, 32KB) in a new segment P1 hasn't seen.
        // Store B's address in block A for P1 to read.
        // -----------------------------------------------------------------
        DataBlock* block_a = uballoc::shm_new<DataBlock>(uballoc::pub_tid{TYPE_DATA});
        if (!block_a) {
            std::cerr << "  P0: shm_new<DataBlock>(pub_tid{TYPE_DATA}) failed!" << std::endl;
            return 1;
        }

        block_a->rank = 0;
        block_a->block_b_addr = 0;
        std::snprintf(block_a->message, sizeof(block_a->message), "Hello from P0");
        block_a->ready.store(1, std::memory_order_release);

        std::cout << "  P0: allocated+published block A at " << block_a << std::endl;

        // Wait for P1 to confirm it read block A.
        while (block_a->ready.load(std::memory_order_acquire) != 2) {
            ::usleep(10000);
        }
        std::cout << "  P0: P1 confirmed receipt of block A" << std::endl;

        // Allocate block B (32KB, huge bracket). This creates a new huge
        // data segment that P1 hasn't attached yet. P1's last
        // check_remote_segments (triggered by lookup_by_type) ran before
        // this segment existed, so P1 has no fd or mmap for it.
        void* block_b = uballoc::malloc(BLOCK_B_SIZE);
        if (!block_b) {
            std::cerr << "  P0: malloc(BLOCK_B_SIZE) failed!" << std::endl;
            return 1;
        }
        std::memset(block_b, BLOCK_B_PATTERN, BLOCK_B_SIZE);

        std::cout << "  P0: allocated block B at " << block_b
                  << " (new huge segment, not yet attached by P1)" << std::endl;

        // Store block B's address in block A and signal P1.
        // The release store on ready ensures that all prior writes
        // (including the segment directory update in create_data_segment)
        // are visible to P1 after its acquire load.
        block_a->block_b_addr = static_cast<uint64_t>(
            reinterpret_cast<uintptr_t>(block_b));
        block_a->ready.store(3, std::memory_order_release);

        // Wait for P1 to finish.
        while (block_a->ready.load(std::memory_order_acquire) != 4) {
            ::usleep(10000);
        }
        std::cout << "  P0: P1 signaled done" << std::endl;

        uballoc::free(block_b);
        uballoc::free(block_a);
        return 0;
    }

    // -----------------------------------------------------------------
    // P1: uffd is default-on (constructor 102 or lazy_init fallback).
    // -----------------------------------------------------------------

    // Step 1: Look up block A. This triggers check_remote_segments()
    // which eagerly attaches P0's CURRENT data segments (small bracket,
    // segment s=0). At this point, P0 hasn't created any huge-bracket
    // segments yet, so P1 has no huge segment attached.
    auto info = uballoc::lookup_by_type_blocking(TYPE_DATA, 30000);
    if (info.owner_process < 0) {
        std::cerr << "  P1: lookup failed (timeout)" << std::endl;
        return 1;
    }

    DataBlock* peer = static_cast<DataBlock*>(info.address);
    while (peer->ready.load(std::memory_order_acquire) != 1) {
        ::usleep(1000);
    }

    std::cout << "  P1: found block A at " << peer
              << " msg='" << peer->message << "'" << std::endl;

    assert(peer->rank == 0);
    assert(std::strcmp(peer->message, "Hello from P0") == 0);
    std::cout << "  P1: block A verified (eager attach OK)" << std::endl;

    // Signal P0 to allocate block B.
    peer->ready.store(2, std::memory_order_release);

    // Wait for P0 to allocate block B and store its address.
    while (peer->ready.load(std::memory_order_acquire) != 3) {
        ::usleep(10000);
    }

    // Step 2: Read block B's address from block A (in the already-attached
    // small-bracket segment). We do NOT call lookup_by_type for block B —
    // that would trigger check_remote_segments and eagerly attach the new
    // huge segment, defeating the purpose of this test.
    uintptr_t block_b_addr = static_cast<uintptr_t>(peer->block_b_addr);
    if (block_b_addr == 0) {
        std::cerr << "  P1: block B address is null" << std::endl;
        return 1;
    }

    std::cout << "  P1: block B address = 0x" << std::hex << block_b_addr
              << std::dec << " (in P0's huge-bracket VA, not yet attached)"
              << std::endl;

    // Step 3: Access block B directly. The huge-bracket segment is NOT
    // attached — P1 has no mmap or fd for it. The access triggers a page
    // fault (missing PTE within the uffd-registered VMA).
    //
    // The uffd handler:
    //   1. Classifies the fault (DATA_HUGE, process 0, some segment s)
    //   2. Ensures P0's metadata is attached (it is, from step 1)
    //   3. Reads the huge segment directory from P0's metadata
    //   4. Finds segment s with ready=1
    //   5. Reads segment name and size from the descriptor
    //   6. Calls shm_attach(segment_name) to establish the borrow
    //   7. mmaps the segment at the correct VA
    //   8. Registers in g_fh_entries for future fast-path hits
    //   9. uffd_wake → faulting instruction re-executes → access succeeds
    //
    // This is the KEY difference from the SIGSEGV handler: uffd can discover
    // and attach segments on demand from normal thread context (can call
    // shm_attach, malloc, sockets). The SIGSEGV handler could only re-mmap
    // using pre-registered fds (it cannot discover new segments).
    std::cout << "  P1: accessing block B directly (uffd should fire + lazy-attach)..."
              << std::endl;

    volatile uint8_t* bp = reinterpret_cast<volatile uint8_t*>(block_b_addr);
    uint8_t first_byte = bp[0];  // This access faults → uffd → lazy-attach.

    // Verify block B's contents (the entire buffer is accessible now).
    bool ok = true;
    for (size_t i = 0; i < BLOCK_B_SIZE; ++i) {
        if (bp[i] != BLOCK_B_PATTERN) {
            ok = false;
            break;
        }
    }

    if (!ok) {
        std::cerr << "  P1: block B data verification FAILED" << std::endl;
        return 1;
    }

    std::cout << "  P1: block B verified (first byte=0x" << std::hex
              << static_cast<int>(first_byte) << std::dec
              << ", all " << BLOCK_B_SIZE << " bytes match pattern 0x"
              << std::hex << static_cast<int>(BLOCK_B_PATTERN) << std::dec << ")"
              << std::endl;

    // Signal P0 that we're done.
    peer->ready.store(4, std::memory_order_release);

    std::cout << "  P1: test passed — uffd lazy-attached a remote segment on demand"
              << std::endl;

    return 0;
}
