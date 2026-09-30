// SPDX-License-Identifier: Apache-2.0

/**
 * @file test_ar11_stl_containers.cpp
 * @brief AR-11 DT: STL Containers on Shared Memory
 *
 * Cases:
 *   DT11-1 R1,R2: ShmVector/ShmSet/ShmMap/ShmUmap semantics + traits
 *   DT11-2 R3,R4: container object in shm, nodes in the shm VA reservation
 *   DT11-3 R4: fork — P0 map, P1 copy-out + emplace(101), P0 sees 101
 *   DT11-4 R7: nested vector<vector<int>> — every buffer in shm
 *   DT11-5 R5: one-step construct + publish + shm_delete semantics
 *   DT11-6 R6: shm_delete destroys exactly once; freed blocks are reused
 *   DT11-7 R4: cross-process ShmVector — P0 push_back, P1 reads
 *
 */

#include "uballoc.hpp"
#include "uballoc/stl_alloc.hpp"
#include "test_helpers.hpp"

#include <atomic>
#include <cstdint>
#include <iostream>
#include <map>

#include <sys/wait.h>

namespace {

constexpr size_t M = 1024 * 1024;

struct Cnt {
    int v;
    inline static std::atomic<int> destroyed{0};
    explicit Cnt(int x) : v(x) {}
    ~Cnt() { destroyed.fetch_add(1); }
};

// -----------------------------------------------------------------------------
// DT11-1 R1,R2
// -----------------------------------------------------------------------------
void dt11_1_container_semantics(const std::string& heap_id) {
    std::cout << "[DT11-1] four containers: insert/erase/find/traverse..." << std::endl;
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        ar_set_heap_id(heap_id);
        ar_child_init();

        // R1: allocator traits required for composite containers.
        using Alloc = uballoc::ShmAlloc<int>;
        static_assert(Alloc::propagate_on_container_copy_assignment::value);
        static_assert(Alloc::propagate_on_container_move_assignment::value);
        static_assert(Alloc::propagate_on_container_swap::value);
        static_assert(Alloc::is_always_equal::value);

        // Stack containers with shm elements: fine for process-lifetime use.
        uballoc::ShmVector<int> v;
        for (int i = 0; i < 100; ++i) v.push_back(i);
        assert(v.size() == 100 && v[50] == 50);
        v.erase(v.begin());
        assert(v.size() == 99 && v[0] == 1);

        uballoc::ShmSet<int> s;
        for (int x : {5, 3, 1, 4, 2, 3}) s.insert(x);
        assert(s.size() == 5 && s.count(3) == 1);
        int expect = 1;
        for (int x : s) assert(x == expect++);  // ordered traversal 1..5

        uballoc::ShmMap<int, int> m;
        m[1] = 10;
        m.insert({2, 20});
        assert(m.at(1) == 10 && m.count(2) == 1);
        m.erase(1);
        assert(m.count(1) == 0);

        uballoc::ShmUmap<int, int> um;
        um[7] = 70;
        um.insert({8, 80});
        assert(um.find(7) != um.end() && um.at(7) == 70);
        um.erase(7);
        assert(um.count(7) == 0);

        std::cout << "  OK: std-compatible semantics" << std::endl;
        _exit(0);
    }
    int st;
    waitpid(pid, &st, 0);
    check_child(st);
}

// -----------------------------------------------------------------------------
// DT11-2 R3,R4: object + nodes resident in shm
// -----------------------------------------------------------------------------
void dt11_2_object_in_shm(const std::string& heap_id) {
    std::cout << "[DT11-2] ShmMap object and nodes live in the shm bracket..." << std::endl;
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        ar_set_heap_id(heap_id);
        ar_child_init();

        using ShmMapII = uballoc::ShmMap<int, int>;
        auto* m = uballoc::shm_new<ShmMapII>(uballoc::pub_tid{201});
        assert(m != nullptr);
        for (int i = 0; i < 50; ++i) (*m)[i] = i * i;

        auto info = uballoc::lookup_by_type(201);
        assert(info.owner_process == 0);
        assert(info.address == static_cast<void*>(m));
        assert(info.size == sizeof(ShmMapII));

        // The container object and every node must reside in the shm VA
        // reservation (located via /proc/self/maps).
        auto regions = ar_parse_maps();
        auto span = ar_find_large_span(regions, 1ULL << 40);  // >= 1TB
        assert(span.start != 0);
        assert(reinterpret_cast<uintptr_t>(m) >= span.start);
        assert(reinterpret_cast<uintptr_t>(m) < span.end);

        for (const auto& kv : *m) {
            uintptr_t node_addr = reinterpret_cast<uintptr_t>(&kv.first);
            assert(node_addr >= span.start);
            assert(node_addr < span.end);
            assert(ar_maps_contains(regions, node_addr));
            assert(kv.second == kv.first * kv.first);
        }

        uballoc::unpublish(static_cast<void*>(m));
        uballoc::shm_delete(m);
        std::cout << "  OK: object and all 50 nodes in shm" << std::endl;
        _exit(0);
    }
    int st;
    waitpid(pid, &st, 0);
    check_child(st);
}

// -----------------------------------------------------------------------------
// DT11-3 R4: cross-process in-place read and write
// -----------------------------------------------------------------------------
void dt11_3_cross_process(const std::string& heap_id) {
    std::cout << "[DT11-3] P0 map -> P1 copies out + emplaces -> P0 sees 101..." << std::endl;
    int fds[2], rp[2];
    assert(pipe(fds) == 0);
    assert(pipe(rp) == 0);

    ar_set_heap_id(heap_id);
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        ::close(fds[1]);
        ::close(rp[0]);
        assert(ar_read_byte(fds[0]) == 'G');

        ar_child_init();
        uint64_t v = 0;
        ar_read_fd(fds[0], &v, sizeof(v));
        auto* m = reinterpret_cast<uballoc::ShmMap<int, int>*>(v);

        auto info = uballoc::lookup_by_type_blocking(202, 5000);
        assert(info.owner_process == 0);
        assert(info.address == static_cast<void*>(m));

        // In-place read, copied out by value.
        std::map<int, int> local(m->begin(), m->end());
        assert(local.size() == 100);
        for (int i = 1; i <= 100; ++i) assert(local.count(i) == 1);

        // In-place write: node allocated in P1's rank bracket.
        auto res = m->emplace(101, 101);
        assert(res.second);
        ar_write_byte(rp[1], 'E');

        assert(ar_read_byte(fds[0]) == 'D');  // P0 destroyed the container
        _exit(0);
    }

    ::close(fds[0]);
    ::close(rp[1]);
    ar_parent_init();

    using ShmMapII = uballoc::ShmMap<int, int>;
    auto* m = uballoc::shm_new<ShmMapII>(uballoc::pub_tid{202});
    assert(m != nullptr);
    for (int i = 1; i <= 100; ++i) (*m)[i] = i;

    ar_write_byte(fds[1], 'G');
    uint64_t v = reinterpret_cast<uintptr_t>(m);
    ar_write_fd(fds[1], &v, sizeof(v));

    assert(ar_read_byte(rp[0]) == 'E');
    auto refresh = uballoc::lookup_by_type(202);
    assert(refresh.owner_process == 0);
    assert(m->size() == 101);
    assert(m->count(101) == 1 && (*m)[101] == 101);

    uballoc::unpublish(static_cast<void*>(m));
    uballoc::shm_delete(m);  // destroys P0- and P1-owned nodes
    ar_write_byte(fds[1], 'D');

    int st;
    waitpid(pid, &st, 0);
    check_child(st);
    ::close(fds[1]);
    ar_alloc().reset();
    std::cout << "  OK: mixed-rank nodes traversed (101 items)" << std::endl;
}

// -----------------------------------------------------------------------------
// DT11-4 R7: nested containers
// -----------------------------------------------------------------------------
void dt11_4_nested_containers(const std::string& heap_id) {
    std::cout << "[DT11-4] vector<vector<int>> — every buffer in shm..." << std::endl;
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        ar_set_heap_id(heap_id);
        ar_child_init();

        // Locate the shm VA reservation from /proc/self/maps.
        auto regions = ar_parse_maps();
        auto span = ar_find_large_span(regions, 1ULL << 40);  // >= 1TB
        assert(span.start != 0);

        auto* vv = uballoc::shm_new<uballoc::ShmVector<uballoc::ShmVector<int>>>();
        assert(vv != nullptr);
        vv->reserve(4);
        for (int i = 0; i < 4; ++i) {
            vv->emplace_back();
            for (int j = 0; j < 10; ++j) (*vv)[i].push_back(i * 10 + j);
        }

        // Outer element buffer (array of ShmVector<int> objects) in shm.
        uintptr_t outer = reinterpret_cast<uintptr_t>(vv->data());
        assert(outer >= span.start && outer < span.end);
        for (int i = 0; i < 4; ++i) {
            // Inner element buffers also in shm.
            uintptr_t inner = reinterpret_cast<uintptr_t>((*vv)[i].data());
            assert(inner >= span.start && inner < span.end);
            for (int j = 0; j < 10; ++j) {
                assert((*vv)[i][j] == i * 10 + j);
            }
        }

        uballoc::shm_delete(vv);  // recursive destruction
        std::cout << "  OK: nested buffers all in shm" << std::endl;
        _exit(0);
    }
    int st;
    waitpid(pid, &st, 0);
    check_child(st);
}

// -----------------------------------------------------------------------------
// DT11-5 R5: one-step construct + publish + shm_delete semantics
// -----------------------------------------------------------------------------
void dt11_5_pub_tid_wrapper(const std::string& heap_id) {
    std::cout << "[DT11-5] shm_new<Cnt>(pub_tid{205}, 7) + shm_delete + dangling..." << std::endl;
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        ar_set_heap_id(heap_id);
        ar_child_init();
        Cnt::destroyed.store(0);

        auto* f = uballoc::shm_new<Cnt>(uballoc::pub_tid{205}, 7);
        assert(f != nullptr && f->v == 7);

        auto info = uballoc::lookup_by_type(205);
        assert(info.owner_process == 0);
        assert(info.address == static_cast<void*>(f));
        assert(info.size == sizeof(Cnt));
        assert(static_cast<Cnt*>(info.address)->v == 7);

        uballoc::shm_delete(f);
        assert(Cnt::destroyed.load() == 1);

        // shm_delete calls free() but NOT unpublish — Registry entry survives.
        auto stale = uballoc::lookup_by_type(205);
        assert(stale.address == static_cast<void*>(f));
        assert(uballoc::unpublish(static_cast<void*>(f)));

        std::cout << "  OK: construct+publish, destroy once, dangling entry" << std::endl;
        _exit(0);
    }
    int st;
    waitpid(pid, &st, 0);
    check_child(st);
}

// -----------------------------------------------------------------------------
// DT11-6 R6: destructor once per element + block reuse
// -----------------------------------------------------------------------------
void dt11_6_shm_delete(const std::string& heap_id) {
    std::cout << "[DT11-6] shm_delete destroys once; freed block reused..." << std::endl;
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        ar_set_heap_id(heap_id);
        ar_child_init();
        Cnt::destroyed.store(0);

        Cnt* a = uballoc::shm_new<Cnt>(1);
        Cnt* b = uballoc::shm_new<Cnt>(2);
        Cnt* c = uballoc::shm_new<Cnt>(3);
        assert(a && b && c && a != b && b != c);

        uballoc::shm_delete(a);
        uballoc::shm_delete(b);
        uballoc::shm_delete(c);
        assert(Cnt::destroyed.load() == 3);

        Cnt* d = uballoc::shm_new<Cnt>(4);
        assert(d == a || d == b || d == c);  // freed block reused
        uballoc::shm_delete(d);
        assert(Cnt::destroyed.load() == 4);

        std::cout << "  OK: 4 constructions, 4 destructions, block reused" << std::endl;
        _exit(0);
    }
    int st;
    waitpid(pid, &st, 0);
    check_child(st);
}

// -----------------------------------------------------------------------------
// DT11-7 R4: cross-process ShmVector — P0 push_back, P1 reads
// -----------------------------------------------------------------------------
void dt11_7_cross_process_vector(const std::string& heap_id) {
    std::cout << "[DT11-7] P0 ShmVector push_back -> P1 reads..." << std::endl;
    int fds[2];
    assert(pipe(fds) == 0);

    ar_set_heap_id(heap_id);
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        ::close(fds[1]);
        assert(ar_read_byte(fds[0]) == 'G');

        ar_child_init();
        uint64_t v = 0;
        ar_read_fd(fds[0], &v, sizeof(v));
        auto* vec = reinterpret_cast<uballoc::ShmVector<int>*>(v);

        auto info = uballoc::lookup_by_type(207);
        assert(info.owner_process == 0);
        assert(info.address == static_cast<void*>(vec));
        assert(info.size == sizeof(uballoc::ShmVector<int>));

        assert(vec->size() == 100);
        for (int i = 0; i < 100; ++i) {
            assert((*vec)[i] == i * 2);
        }

        std::cout << "  OK: P1 read 100 elements from P0's ShmVector" << std::endl;
        _exit(0);
    }

    ::close(fds[0]);
    ar_parent_init();

    using ShmVecI = uballoc::ShmVector<int>;
    auto* vec = uballoc::shm_new<ShmVecI>(uballoc::pub_tid{207});
    assert(vec != nullptr);
    for (int i = 0; i < 100; ++i) vec->push_back(i * 2);
    assert(vec->size() == 100);

    ar_write_byte(fds[1], 'G');
    uint64_t v = reinterpret_cast<uintptr_t>(vec);
    ar_write_fd(fds[1], &v, sizeof(v));

    int st;
    waitpid(pid, &st, 0);
    check_child(st);
    ::close(fds[1]);

    uballoc::unpublish(static_cast<void*>(vec));
    uballoc::shm_delete(vec);
    ar_alloc().reset();
    std::cout << "  OK: P0's ShmVector visible to P1 (fixed VA)" << std::endl;
}

}  // namespace

int main() {
    std::string id = ar_heap_id("ar11");
    cleanup_all_distributed_shms(8, id);

    ar_pre_case_cleanup(id);

    dt11_1_container_semantics(id);
    ar_pre_case_cleanup(id);
    dt11_2_object_in_shm(id);
    ar_pre_case_cleanup(id);
    dt11_3_cross_process(id);
    ar_pre_case_cleanup(id);
    dt11_4_nested_containers(id);
    ar_pre_case_cleanup(id);
    dt11_5_pub_tid_wrapper(id);
    ar_pre_case_cleanup(id);
    dt11_6_shm_delete(id);
    ar_pre_case_cleanup(id);
    dt11_7_cross_process_vector(id);

    cleanup_all_distributed_shms(8, id);
    std::cout << "\n=== AR-11 DT passed ===" << std::endl;
    return 0;
}
