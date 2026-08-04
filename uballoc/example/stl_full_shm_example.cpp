// SPDX-License-Identifier: Apache-2.0

/**
 * @file stl_full_shm_example.cpp
 * @brief Multi-process STL containers with BOTH header and data in shm
 *
 * Demonstrates the "full shm" pattern: the container object itself
 * (its header: begin/end/capacity pointers, tree root, bucket array
 * pointer, etc.) AND its dynamically-allocated nodes/buffers are both
 * in shm. This is achieved via uballoc::shm_new<T>(), which placement-
 * constructs the container into a uballoc-returned region.
 *
 * Containers demonstrated: std::vector, std::set, std::map,
 * std::unordered_map — all backed entirely by uballoc shared memory.
 * Two processes launched independently from different terminals share
 * the same containers.
 *
 * Launch:
 *   export UBALLOC_HEAP_ID=stlheap
 *   ./stl_full_shm_example 0 &     # app_pid=0 (P0 role)
 *   ./stl_full_shm_example 1       # app_pid=1 (P1 role)
 *
 * P0 creates the containers in shm via shm_new, fills them, and publishes
 * their addresses.  P1 looks them up, verifies P0's data, adds its own
 * entries, and signals completion.  P0 verifies P1's additions, then
 * destroys and frees everything.
 *
 * Works because uballoc maps all shm regions at fixed virtual addresses
 * (see shm_provider.hpp), so raw pointers are valid across processes.
 *
 * Contrast with stl_data_only_example.cpp, which keeps the container
 * header on the stack and only puts the data buffer in shm.
 */

#include <uballoc.hpp>
#include <uballoc/stl_alloc.hpp>

#include <iostream>
#include <cstdlib>
#include <cstring>
#include <cassert>
#include <vector>
#include <set>
#include <map>
#include <unordered_map>
#include <unistd.h>

using Backend = uballoc::DistributedShmBackend<>;
using ShmProvider = Backend::ShmProvider;

static void section(const char* title) {
    std::cout << "\n---------- " << title << " ----------" << std::endl;
}

enum TypeId : uint32_t {
    TYPE_VECTOR   = 10,
    TYPE_SET      = 11,
    TYPE_MAP      = 12,
    TYPE_UMAP     = 13,
    TYPE_P1_DONE  = 14,
};

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "Usage: " << argv[0] << " <app_pid>\n"
                  << "  app_pid: application process id (0 or 1)\n"
                  << "Environment: UBALLOC_HEAP_ID must be set.\n"
                  << "  Terminal 1:  UBALLOC_HEAP_ID=stlheap " << argv[0] << " 0 &\n"
                  << "  Terminal 2:  UBALLOC_HEAP_ID=stlheap " << argv[0] << " 1\n";
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
                  << "Usage: export UBALLOC_HEAP_ID=stlheap\n"
                  << "  Terminal 1:  UBALLOC_HEAP_ID=stlheap " << argv[0] << " 0 &\n"
                  << "  Terminal 2:  UBALLOC_HEAP_ID=stlheap " << argv[0] << " 1\n";
        return 1;
    }
    std::string heap_id(heap_id_env);

    std::cout << "========================================\n"
              << "  STL Containers in Shared Memory\n"
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
        section("P0: Create STL containers in shared memory");

        auto* vec  = uballoc::shm_new<uballoc::ShmVector<int>>(uballoc::pub_tid{TYPE_VECTOR});
        auto* set  = uballoc::shm_new<uballoc::ShmSet<int>>(uballoc::pub_tid{TYPE_SET});
        auto* map  = uballoc::shm_new<uballoc::ShmMap<int, int>>(uballoc::pub_tid{TYPE_MAP});
        auto* umap = uballoc::shm_new<uballoc::ShmUmap<int, int>>(uballoc::pub_tid{TYPE_UMAP});

        for (int i = 1; i <= 5; ++i) vec->push_back(i * 10);
        for (int i = 1; i <= 3; ++i) set->insert(i * 100);
        for (int i = 1; i <= 3; ++i) (*map)[i] = i * 10;
        for (int i = 1; i <= 3; ++i) (*umap)[i] = i * 100;

        std::cout << "  vector: size=" << vec->size()
                  << " data=[";
        for (size_t i = 0; i < vec->size(); ++i)
            std::cout << (i ? "," : "") << (*vec)[i];
        std::cout << "]  ptr=" << vec << std::endl;

        std::cout << "  set: size=" << set->size()
                  << " data={";
        bool first = true;
        for (int v : *set) { std::cout << (first ? "" : ",") << v; first = false; }
        std::cout << "}  ptr=" << set << std::endl;

        std::cout << "  map: size=" << map->size()
                  << " data={";
        first = true;
        for (auto& [k, v] : *map) {
            std::cout << (first ? "" : ",") << k << ":" << v; first = false;
        }
        std::cout << "}  ptr=" << map << std::endl;

        std::cout << "  umap: size=" << umap->size()
                  << " data={";
        first = true;
        for (auto& [k, v] : *umap) {
            std::cout << (first ? "" : ",") << k << ":" << v; first = false;
        }
        std::cout << "}  ptr=" << umap << std::endl;

        std::cout << "  Published types " << TYPE_VECTOR << "-" << TYPE_UMAP
                  << " (via combined shm_new+publish)" << std::endl;

        section("P0: Waiting for P1 to finish...");
        auto done = uballoc::lookup_by_type_blocking(TYPE_P1_DONE, 30000);
        if (done.owner_process < 0) {
            std::cerr << "P1 did not signal completion" << std::endl;
            uballoc::shm_delete(vec);
            uballoc::shm_delete(set);
            uballoc::shm_delete(map);
            uballoc::shm_delete(umap);
            return 1;
        }
        std::cout << "  P1 signaled done" << std::endl;

        section("P0: Verifying P1's additions");

        assert(vec->size() == 8);
        assert((*vec)[5] == 60 && (*vec)[6] == 70 && (*vec)[7] == 80);
        std::cout << "  vector: size=" << vec->size() << " (expected 8) OK" << std::endl;

        assert(set->size() == 5);
        assert(set->count(400) && set->count(500));
        std::cout << "  set: size=" << set->size() << " (expected 5), has 400,500 OK" << std::endl;

        assert(map->size() == 5);
        assert(map->at(4) == 40 && map->at(5) == 50);
        std::cout << "  map: size=" << map->size() << " (expected 5), map[4]=40 map[5]=50 OK" << std::endl;

        assert(umap->size() == 4);
        assert(umap->at(4) == 400);
        std::cout << "  umap: size=" << umap->size() << " (expected 4), umap[4]=400 OK" << std::endl;

        section("P0: Cleanup");
        uballoc::unpublish(vec);
        uballoc::unpublish(set);
        uballoc::unpublish(map);
        uballoc::unpublish(umap);

        uballoc::shm_delete(vec);
        uballoc::shm_delete(set);
        uballoc::shm_delete(map);
        uballoc::shm_delete(umap);
        std::cout << "  All containers destroyed and freed" << std::endl;

        // Keep alive briefly so the peer can finish and exit cleanly before
        // our uballoc destructor runs (avoids teardown races on shared state).
        sleep(2);

        std::cout << "\n=== P0 complete ===" << std::endl;

    } else {
        section("P1: Looking up P0's containers");
        auto vec_info  = uballoc::lookup_by_type_blocking(TYPE_VECTOR, 30000);
        auto set_info  = uballoc::lookup_by_type_blocking(TYPE_SET, 30000);
        auto map_info  = uballoc::lookup_by_type_blocking(TYPE_MAP, 30000);
        auto umap_info = uballoc::lookup_by_type_blocking(TYPE_UMAP, 30000);

        if (vec_info.owner_process < 0 || set_info.owner_process < 0 ||
            map_info.owner_process < 0 || umap_info.owner_process < 0) {
            std::cerr << "Failed to look up all containers from P0" << std::endl;
            return 1;
        }

        auto* vec  = reinterpret_cast<uballoc::ShmVector<int>*>(vec_info.address);
        auto* set  = reinterpret_cast<uballoc::ShmSet<int>*>(set_info.address);
        auto* map  = reinterpret_cast<uballoc::ShmMap<int, int>*>(map_info.address);
        auto* umap = reinterpret_cast<uballoc::ShmUmap<int, int>*>(umap_info.address);

        std::cout << "  vector -> " << vec << " (owner=P" << vec_info.owner_process << ")" << std::endl;
        std::cout << "  set    -> " << set << " (owner=P" << set_info.owner_process << ")" << std::endl;
        std::cout << "  map    -> " << map << " (owner=P" << map_info.owner_process << ")" << std::endl;
        std::cout << "  umap   -> " << umap << " (owner=P" << umap_info.owner_process << ")" << std::endl;

        section("P1: Verifying P0's data");

        assert(vec->size() == 5);
        for (int i = 0; i < 5; ++i) assert((*vec)[i] == (i + 1) * 10);
        std::cout << "  vector: size=" << vec->size() << " values [10,20,30,40,50] OK" << std::endl;

        assert(set->size() == 3);
        assert(set->count(100) && set->count(200) && set->count(300));
        std::cout << "  set: size=" << set->size() << " has {100,200,300} OK" << std::endl;

        assert(map->size() == 3);
        assert(map->at(1) == 10 && map->at(2) == 20 && map->at(3) == 30);
        std::cout << "  map: size=" << map->size() << " {1:10,2:20,3:30} OK" << std::endl;

        assert(umap->size() == 3);
        assert(umap->at(1) == 100 && umap->at(2) == 200 && umap->at(3) == 300);
        std::cout << "  umap: size=" << umap->size() << " {1:100,2:200,3:300} OK" << std::endl;

        section("P1: Adding new entries");

        vec->push_back(60);
        vec->push_back(70);
        vec->push_back(80);
        std::cout << "  vector: appended {60,70,80} -> size=" << vec->size() << std::endl;

        set->insert(400);
        set->insert(500);
        std::cout << "  set: inserted {400,500} -> size=" << set->size() << std::endl;

        (*map)[4] = 40;
        (*map)[5] = 50;
        std::cout << "  map: added {4:40,5:50} -> size=" << map->size() << std::endl;

        (*umap)[4] = 400;
        std::cout << "  umap: added {4:400} -> size=" << umap->size() << std::endl;

        section("P1: Signaling completion");
        uballoc::publish(reinterpret_cast<void*>(1), 0, TYPE_P1_DONE);
        std::cout << "  Published TYPE_P1_DONE" << std::endl;

        // Keep alive briefly so the peer can finish and exit cleanly before
        // our uballoc destructor runs (avoids teardown races on shared state).
        sleep(2);

        std::cout << "\n=== P1 complete ===" << std::endl;
    }

    return 0;
}
