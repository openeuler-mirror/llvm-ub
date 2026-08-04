// SPDX-License-Identifier: Apache-2.0

/**
 * @file stl_data_only_example.cpp
 * @brief Multi-process STL containers with header-on-stack, data-in-shm
 *
 * Demonstrates the "data-only" pattern: the std::vector object itself
 * (its header: begin/end/capacity pointers) lives on P0's stack, but its
 * dynamically-allocated buffer is in shm because ShmAlloc::allocate()
 * routes through uballoc. P0 publishes a small descriptor struct
 * {int* data; size_t size;} that lets P1 read the buffer as a raw int*
 * — P1 does not construct a ShmVector at all.
 *
 * This pattern is suitable for:
 *   - Single-writer (P0) + read-only observer (P1) workloads
 *   - Contiguous containers (vector) where the data pointer is the only
 *     thing other processes need
 *
 * This pattern does NOT work for:
 *   - std::map / std::set / std::unordered_map — the container header
 *     holds the root/bucket pointer; without it, other processes cannot
 *     traverse the tree/bucket structure. For those, use shm_new to put
 *     the header in shm too (see stl_full_shm_example.cpp).
 *
 * Caveats:
 *   - P0 must publish the data pointer AFTER all push_back calls —
 *     vector may reallocate during growth, invalidating earlier pointers.
 *   - Mutating the buffer from P1 is unsafe: P0's local size/capacity
 *     would diverge from P1's view, and on destruction P0 would free
 *     a buffer that P1 might still be reading. The done-signal pattern
 *     below coordinates cleanup.
 *
 * Launch:
 *   export UBALLOC_HEAP_ID=stlheap2
 *   ./stl_data_only_example 0 &     # app_pid=0 (P0 role)
 *   ./stl_data_only_example 1       # app_pid=1 (P1 role)
 *
 * Works because uballoc maps all shm regions at fixed virtual addresses
 * (see shm_provider.hpp), so the data pointer P0 publishes is valid in
 * P1's address space without translation.
 *
 * Contrast with stl_full_shm_example.cpp, which puts both header and
 * data in shm via shm_new.
 */

#include <uballoc.hpp>
#include <uballoc/stl_alloc.hpp>

#include <iostream>
#include <cstdlib>
#include <cstring>
#include <cassert>
#include <vector>
#include <unistd.h>

using Backend = uballoc::DistributedShmBackend<>;
using ShmProvider = Backend::ShmProvider;

static void section(const char* title) {
    std::cout << "\n---------- " << title << " ----------" << std::endl;
}

enum TypeId : uint32_t {
    TYPE_VEC_DESC = 20,   // PublishedVector descriptor
    TYPE_P1_DONE  = 21,
};

// Descriptor published by P0 so P1 can find the vector's data buffer
// without needing the vector header. Allocated via uballoc::malloc
// (combined malloc+publish) so it lives in shm and is discoverable.
struct PublishedVector {
    int*   data;    // pointer into shm (vector's internal buffer)
    size_t size;    // number of elements
};

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "Usage: " << argv[0] << " <app_pid>\n"
                  << "  app_pid: application process id (0 or 1)\n"
                  << "Environment: UBALLOC_HEAP_ID must be set.\n"
                  << "  Terminal 1:  UBALLOC_HEAP_ID=stlheap2 " << argv[0] << " 0 &\n"
                  << "  Terminal 2:  UBALLOC_HEAP_ID=stlheap2 " << argv[0] << " 1\n";
        return 1;
    }
    int rank = std::atoi(argv[1]);
    if (rank != 0 && rank != 1) {
        std::cerr << "Error: app_pid must be 0 or 1, got: " << rank << std::endl;
        return 1;
    }

    const char* heap_id_env = std::getenv("UBALLOC_HEAP_ID");
    if (!heap_id_env || !*heap_id_env) {
        std::cerr << "Error: UBALLOC_HEAP_ID not set.\n"
                  << "Usage: export UBALLOC_HEAP_ID=stlheap2\n"
                  << "  Terminal 1:  UBALLOC_HEAP_ID=stlheap2 " << argv[0] << " 0 &\n"
                  << "  Terminal 2:  UBALLOC_HEAP_ID=stlheap2 " << argv[0] << " 1\n";
        return 1;
    }
    std::string heap_id(heap_id_env);

    std::cout << "========================================\n"
              << "  STL Data-Only Shm Pattern\n"
              << "  (header on stack, data in shm)\n"
              << "  UBALLOC_HEAP_ID=" << heap_id << "\n"
              << "  app_pid=" << rank << "\n"
              << "========================================" << std::endl;

    // Explicit init: reads UBALLOC_HEAP_ID, discovers rank and membership
    // at runtime via the bootstrap PID table. The application's own notion
    // of process identity (app_pid) is provided via argv[1] and is
    // independent of uballoc's auto-assigned rank.
    uballoc::init();
    std::cout << "  Init OK (app_pid=" << rank
              << " uballoc_total=" << uballoc::total_processes() << ")" << std::endl;

    if (rank == 0) {
        PublishedVector* desc = nullptr;

        {
            section("P0: Construct stack-local ShmVector (header on stack, buffer in shm)");

            // The vector OBJECT (begin/end/capacity pointers) is on P0's stack.
            // Its internal BUFFER, allocated by ShmAlloc::allocate(), is in shm.
            uballoc::ShmVector<int> v;

            for (int i = 1; i <= 5; ++i) v.push_back(i * 10);

            std::cout << "  vector object address (stack): " << &v
                      << "\n  vector data buffer address (shm): " << v.data()
                      << "\n  size=" << v.size() << " capacity=" << v.capacity()
                      << "\n  data=[";
            for (size_t i = 0; i < v.size(); ++i)
                std::cout << (i ? "," : "") << v[i];
            std::cout << "]" << std::endl;

            section("P0: Publish {data ptr, size} descriptor");

            // CRITICAL: publish AFTER all push_back calls — vector may have
            // reallocated during growth, invalidating earlier data() values.
            // Use combined shm_new+publish (allocates, constructs, publishes
            // in one call).
            desc = uballoc::shm_new<PublishedVector>(uballoc::pub_tid{TYPE_VEC_DESC});
            desc->data = v.data();
            desc->size = v.size();
            std::cout << "  Published descriptor at " << desc
                      << " -> {data=" << desc->data
                      << ", size=" << desc->size << "}" << std::endl;

            section("P0: Waiting for P1 to finish reading...");
            auto done = uballoc::lookup_by_type_blocking(TYPE_P1_DONE, 30000);
            if (done.owner_process < 0) {
                std::cerr << "P1 did not signal completion" << std::endl;
                uballoc::free(desc);
                return 1;
            }
            std::cout << "  P1 signaled done" << std::endl;
            // ~v() runs at end of this scope, freeing the data buffer via
            // ShmAlloc::deallocate. Safe because P1 has already signaled done.
        }

        section("P0: Cleanup");
        // Descriptor cleanup happens AFTER ~v() freed the data buffer.
        // Order is safe because P1 is no longer reading.
        uballoc::free(desc);
        std::cout << "  Descriptor freed" << std::endl;

        // Keep alive briefly so the peer can finish and exit cleanly before
        // our uballoc destructor runs (avoids teardown races on shared state).
        sleep(2);

        std::cout << "\n=== P0 complete ===" << std::endl;

    } else {
        section("P1: Look up P0's descriptor");

        auto info = uballoc::lookup_by_type_blocking(TYPE_VEC_DESC, 30000);
        if (info.owner_process < 0) {
            std::cerr << "Failed to look up P0's vector descriptor" << std::endl;
            return 1;
        }

        auto* desc = static_cast<PublishedVector*>(info.address);
        std::cout << "  descriptor -> " << desc
                  << " (owner=P" << info.owner_process << ")"
                  << "\n  {data=" << desc->data
                  << ", size=" << desc->size << "}" << std::endl;

        section("P1: Read P0's buffer via raw int* (no vector header on P1)");

        // P1 does NOT construct a ShmVector. The data pointer is in shm
        // (fixed VA across processes), so we read it as a plain array.
        // No container metadata is needed on P1's side — just the raw
        // pointer and the size from the descriptor.
        int* data = desc->data;
        size_t size = desc->size;

        assert(size == 5);
        for (size_t i = 0; i < size; ++i) {
            assert(data[i] == static_cast<int>((i + 1) * 10));
        }
        std::cout << "  read " << size << " values: [";
        for (size_t i = 0; i < size; ++i)
            std::cout << (i ? "," : "") << data[i];
        std::cout << "] OK" << std::endl;

        section("P1: Signaling completion");
        uballoc::malloc(1, TYPE_P1_DONE);
        std::cout << "  Published TYPE_P1_DONE" << std::endl;

        // Keep alive briefly so the peer can finish and exit cleanly before
        // our uballoc destructor runs (avoids teardown races on shared state).
        sleep(2);

        std::cout << "\n=== P1 complete ===" << std::endl;
    }

    return 0;
}
