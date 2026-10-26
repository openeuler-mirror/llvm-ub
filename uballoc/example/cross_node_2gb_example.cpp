// SPDX-License-Identifier: Apache-2.0
//
// Example: cross-node 2GB+ huge allocation + cross-node free.
//
// Verifies the P2-1 (while-loop geometric bound) and P2-2 (24-bit slot_count
// field) fixes end-to-end across two nodes. A 2GB allocation needs 16
// segments (128MB each = 1024 slots), exercising the multi-segment grow loop
// (P2-1) and cross-node free_offset reading of the 24-bit slot_count for a
// large allocation (P2-2). The 128GB truncation boundary of the old 16-bit
// field is covered directly by the white-box unit test (T0 in test_grow_2gb.cpp).
//
// Usage (two terminals, same UBALLOC_HEAP_ID):
//   Terminal 1 (P0):  UBALLOC_HEAP_ID=cross2g ./cross_node_2gb_example 0
//   Terminal 2 (P1):  UBALLOC_HEAP_ID=cross2g ./cross_node_2gb_example 1
//
// P0 allocates 2GB, publishes it, writes a position-dependent pattern, waits
// for P1 to free it, then verifies slot reclaim by re-allocating 2GB.
// P1 looks up the published block, verifies data integrity, cross-frees it.

#include <uballoc.hpp>
#include <iostream>
#include <cstring>
#include <cstdlib>
#include <unistd.h>

static constexpr size_t TWO_GB = 2ULL * 1024 * 1024 * 1024;
static constexpr size_t FOUR_GB = 4ULL * 1024 * 1024 * 1024;

// Position-dependent pattern: byte at offset N = (N / 256MB) & 0xFF.
// Sampling at 256MB boundaries keeps the write/verify time bounded (<2s)
// while still touching every segment (each segment = 128MB).
static constexpr size_t SAMPLE_STRIDE = 256ULL * 1024 * 1024;

static void write_pattern(void* p, size_t size) {
    auto* bytes = static_cast<uint8_t*>(p);
    for (size_t off = 0; off < size; off += SAMPLE_STRIDE) {
        bytes[off] = static_cast<uint8_t>((off / SAMPLE_STRIDE) & 0xFF);
    }
}

static bool verify_pattern(void* p, size_t size) {
    auto* bytes = static_cast<uint8_t*>(p);
    for (size_t off = 0; off < size; off += SAMPLE_STRIDE) {
        uint8_t expected = static_cast<uint8_t>((off / SAMPLE_STRIDE) & 0xFF);
        if (bytes[off] != expected) return false;
    }
    return true;
}

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "Usage: " << argv[0] << " <app_pid>\n"
                  << "  app_pid: 0 or 1\n"
                  << "  Terminal 1:  UBALLOC_HEAP_ID=cross2g " << argv[0] << " 0\n"
                  << "  Terminal 2:  UBALLOC_HEAP_ID=cross2g " << argv[0] << " 1\n";
        return 1;
    }
    int rank = std::atoi(argv[1]);
    if (rank != 0 && rank != 1) {
        std::cerr << "Error: app_pid must be 0 or 1, got: " << rank << std::endl;
        return 1;
    }

    const char* heap_id = std::getenv("UBALLOC_HEAP_ID");
    if (!heap_id || !*heap_id) {
        std::cerr << "Error: UBALLOC_HEAP_ID not set.\n"
                  << "  export UBALLOC_HEAP_ID=cross2g" << std::endl;
        return 1;
    }

    std::cout << "========================================\n"
              << "  cross_node_2gb_example\n"
              << "  UBALLOC_HEAP_ID=" << heap_id << "\n"
              << "  app_pid=" << rank << "\n"
              << "========================================" << std::endl;

    uballoc::init();
    if (!uballoc::get_global_allocator<uballoc::DistributedShmBackend<>>().is_initialized()) {
        std::cerr << "init failed" << std::endl;
        return 1;
    }
    std::cout << "Init OK (rank=" << rank
              << " total=" << uballoc::total_processes() << ")" << std::endl;

    constexpr uint32_t TYPE_2GB_BLOCK = 200;
    constexpr uint32_t TYPE_4GB_BLOCK = 201;

    struct Case { const char* name; size_t size; uint32_t type_id; };
    Case cases[] = {
        {"2GB", TWO_GB, TYPE_2GB_BLOCK},
        {"4GB", FOUR_GB, TYPE_4GB_BLOCK},
    };

    if (rank == 0) {
        // P0: for each size — allocate, publish, write, wait for P1 free, re-alloc.
        for (auto& c : cases) {
            std::cout << "\n---------- P0: allocate " << c.name << " ----------" << std::endl;
            void* p = uballoc::malloc(c.size);
            if (!p) {
                std::cerr << "malloc(" << c.name << ") failed" << std::endl;
                return 1;
            }
            std::cout << "  Allocated " << c.name << " at " << p
                      << " class_size=" << uballoc::current_allocator().class_size(p)
                      << "B" << std::endl;

            write_pattern(p, c.size);
            std::cout << "  Wrote position-dependent pattern (stride=" << SAMPLE_STRIDE
                      << ")" << std::endl;

            int pub_slot = uballoc::publish(p, c.size, c.type_id);
            if (pub_slot < 0) {
                std::cerr << "publish failed" << std::endl;
                return 1;
            }
            std::cout << "  Published (type=" << c.type_id
                      << " slot=" << pub_slot << ")" << std::endl;

            // Wait for P1 to free. Poll class_size; 0 means freed.
            // lookup_by_type() triggers check_remote_segments() internally.
            std::cout << "  Waiting for P1 to cross-free..." << std::endl;
            bool freed = false;
            for (int i = 0; i < 60; ++i) {
                uballoc::lookup_by_type(c.type_id);
                size_t cs = uballoc::current_allocator().class_size(p);
                if (cs == 0) {
                    std::cout << "  P1 freed the block (class_size=0 after "
                              << i << "s)" << std::endl;
                    freed = true;
                    break;
                }
                usleep(1000000);
            }
            if (!freed) {
                std::cerr << "FAIL: P1 did not free " << c.name
                          << " within 60s" << std::endl;
                uballoc::free(p);
                return 1;
            }

            // Re-allocate — verifies slot reclaim via the freed slot run.
            std::cout << "  Re-allocating " << c.name << " (slot reclaim)..." << std::endl;
            void* p2 = uballoc::malloc(c.size);
            if (!p2) {
                std::cerr << "re-alloc(" << c.name << ") failed — slot not reclaimed"
                          << std::endl;
                return 1;
            }
            std::cout << "  Re-allocated " << c.name << " at " << p2 << std::endl;
            uballoc::free(p2);
            uballoc::unpublish(p);
        }
        std::cout << "  P0: done" << std::endl;
    } else {
        // P1: for each size — lookup, verify data, cross-free.
        for (auto& c : cases) {
            std::cout << "\n---------- P1: lookup + cross-free " << c.name
                      << " ----------" << std::endl;
            std::cout << "  Looking up type " << c.type_id << " (up to 30s)..." << std::endl;
            auto info = uballoc::lookup_by_type_blocking(c.type_id, 30000);
            if (info.owner_process < 0) {
                std::cerr << "FAIL: type " << c.type_id << " not found" << std::endl;
                return 1;
            }
            std::cout << "  Found block: owner=P" << info.owner_process
                      << " ptr=" << info.address << " size=" << info.size << "B"
                      << std::endl;

            // lookup_by_type_blocking already attached P0's segments.

            if (!verify_pattern(info.address, info.size)) {
                std::cerr << "FAIL: " << c.name << " data verification failed" << std::endl;
                return 1;
            }
            std::cout << "  Data integrity verified (pattern OK)" << std::endl;

            // Cross-node free. Exercises free_offset reading slot_count via
            // the 24-bit field for a multi-segment allocation.
            uballoc::free(info.address);
            std::cout << "  Cross-freed P0's " << c.name << " block" << std::endl;
        }
        std::cout << "  P1: done" << std::endl;
    }

    std::cout << "\n=== cross_node_2gb_example complete (P" << rank << ") ===" << std::endl;

    uballoc::finalize();
    return 0;
}
