// SPDX-License-Identifier: Apache-2.0

/**
 * @file test_ar05_size_classes.cpp
 * @brief AR-05 DT: Small/Large/Huge Size Classes
 *
 * Cases:
 *   DT05-1 R1: routing boundaries — pointers in distinct VA regions
 *   DT05-2 R2: block reuse after free
 *   DT05-3 R3: slab reuse — full batch freed then re-allocated
 *   DT05-4 R4: cross-segment growth, unique addresses, span >= 128MB
 *   DT05-5 R5: 4-thread concurrency, unique addresses, main frees for them
 *   DT05-6 R6: two processes — per-rank bracket isolation
 *   DT05-7 R7: cross-process free by P1, owner (P0) reuses blocks
 *
 * NOTE: fork children rely on lazy thread init (never init_thread(0)).
 */

#include "uballoc.hpp"
#include "test_helpers.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <thread>
#include <unordered_set>
#include <vector>

#include <sys/wait.h>

namespace {

constexpr size_t K = 1024;
constexpr size_t M = 1024 * 1024;

// Pointer distance (absolute value), safe for unsigned.
inline uint64_t ptr_diff(uintptr_t a, uintptr_t b) {
    return (a > b) ? (a - b) : (b - a);
}

// -----------------------------------------------------------------------------
// DT05-1 R1: routing boundaries
// -----------------------------------------------------------------------------
void dt05_1_routing_boundaries(const std::string& heap_id) {
    std::cout << "[DT05-1] routing boundaries: 16384/16385/4MB/4MB+1..." << std::endl;
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        ar_set_heap_id(heap_id);
        ar_child_init();

        void* ps = uballoc::malloc(16384);
        void* pl = uballoc::malloc(16385);
        void* pm = uballoc::malloc(4 * M);
        void* pn = uballoc::malloc(4 * M + 1);
        assert(ps && pl && pm && pn);

        assert(ptr_diff((uintptr_t)ps, (uintptr_t)pl) >= (1ULL << 40));  // >= 1TB
        assert(ptr_diff((uintptr_t)ps, (uintptr_t)pm) >= (1ULL << 40));
        assert(ptr_diff((uintptr_t)ps, (uintptr_t)pn) >= (1ULL << 40));

        std::memset(ps, 0xA1, 16384);
        std::memset(pl, 0xA2, 16385);
        std::memset(pm, 0xA3, 4 * M);
        std::memset(pn, 0xA4, 4 * M + 1);
        assert(static_cast<uint8_t*>(ps)[0] == 0xA1);
        assert(static_cast<uint8_t*>(pl)[0] == 0xA2);
        assert(static_cast<uint8_t*>(pm)[0] == 0xA3);
        assert(static_cast<uint8_t*>(pn)[0] == 0xA4);

        uballoc::free(ps);
        uballoc::free(pl);
        uballoc::free(pm);
        uballoc::free(pn);
        std::cout << "  OK: routing separates Small from Huge" << std::endl;
        _exit(0);
    }
    int st;
    waitpid(pid, &st, 0);
    check_child(st);
}

// -----------------------------------------------------------------------------
// DT05-2 R2: block-level reuse
// -----------------------------------------------------------------------------
void dt05_2_block_reuse(const std::string& heap_id) {
    std::cout << "[DT05-2] free then malloc(64) returns the same block..." << std::endl;
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        ar_set_heap_id(heap_id);
        ar_child_init();

        void* a = uballoc::malloc(64);
        assert(a);
        std::memset(a, 0x42, 64);
        uballoc::free(a);

        void* b = uballoc::malloc(64);
        assert(b == a);
        uballoc::free(b);
        std::cout << "  OK: block reused" << std::endl;
        _exit(0);
    }
    int st;
    waitpid(pid, &st, 0);
    check_child(st);
}

// -----------------------------------------------------------------------------
// DT05-3 R3: slab-level reuse (600 x 128B spans >= 3 slabs)
// -----------------------------------------------------------------------------
void dt05_3_slab_reuse(const std::string& heap_id) {
    std::cout << "[DT05-3] full batch free -> slabs recycled (high overlap)..." << std::endl;
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        ar_set_heap_id(heap_id);
        ar_child_init();

        constexpr int N = 600;
        std::vector<uintptr_t> first;
        first.reserve(N);
        for (int i = 0; i < N; ++i) {
            void* p = uballoc::malloc(128);
            assert(p);
            std::memset(p, i & 0xFF, 128);
            first.push_back(reinterpret_cast<uintptr_t>(p));
        }
        for (int i = 0; i < N; ++i) uballoc::free(reinterpret_cast<void*>(first[i]));

        std::unordered_set<uintptr_t> first_set(first.begin(), first.end());
        std::vector<uintptr_t> second;
        second.reserve(N);
        int reused = 0;
        for (int i = 0; i < N; ++i) {
            void* p = uballoc::malloc(128);
            assert(p);
            uintptr_t v = reinterpret_cast<uintptr_t>(p);
            if (first_set.count(v)) ++reused;
            second.push_back(v);
        }

        // Slab-level reuse: free-list pop order differs, so exact pointer
        // equality doesn't hold — but the overlap must be high.
        assert(reused >= N / 2);

        for (uintptr_t v : second) uballoc::free(reinterpret_cast<void*>(v));
        std::cout << "  OK: " << reused << "/" << N << " blocks reused (slab recycle)" << std::endl;
        _exit(0);
    }
    int st;
    waitpid(pid, &st, 0);
    check_child(st);
}

// -----------------------------------------------------------------------------
// DT05-4 R4: cross-segment growth
// -----------------------------------------------------------------------------
void dt05_4_cross_segment(const std::string& heap_id) {
    std::cout << "[DT05-4] segment overflow -> growth, span >= 128MB..." << std::endl;
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        ar_set_heap_id(heap_id);
        ar_child_init();

        // +10 forces a second segment (SEGMENT_VA_SIZE / 256 per segment).
        const size_t n = uballoc::SEGMENT_VA_SIZE / 256 + 10;

        std::vector<void*> ptrs;
        ptrs.reserve(n);
        std::unordered_set<uintptr_t> unique;
        unique.reserve(n);
        uintptr_t lo = ~uintptr_t(0), hi = 0;
        for (size_t i = 0; i < n; ++i) {
            void* p = uballoc::malloc(256);
            assert(p);
            std::memset(p, 0x5C, 256);
            uintptr_t v = reinterpret_cast<uintptr_t>(p);
            assert(unique.insert(v).second);
            if (v < lo) lo = v;
            if (v > hi) hi = v;
            ptrs.push_back(p);
        }

        assert(hi - lo >= uballoc::SEGMENT_VA_SIZE);

        for (void* p : ptrs) uballoc::free(p);
        std::cout << "  OK: " << n << " allocations, span >= "
                  << uballoc::SEGMENT_VA_SIZE / M << "MB" << std::endl;
        _exit(0);
    }
    int st;
    waitpid(pid, &st, 0);
    check_child(st);
}

// -----------------------------------------------------------------------------
// DT05-5 R5: 4-thread concurrency
// -----------------------------------------------------------------------------
void dt05_5_thread_concurrency(const std::string& heap_id) {
    std::cout << "[DT05-5] 4 threads x 500 allocs, unique, main frees..." << std::endl;
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        ar_set_heap_id(heap_id);
        ar_child_init();
        ar_alloc().init_thread(0);  // main thread is slot 0

        constexpr int kThreads = 4;
        constexpr int kPerThread = 500;
        std::vector<std::vector<uintptr_t>> addrs(kThreads);
        std::vector<std::thread> threads;
        for (int t = 0; t < kThreads; ++t) {
            threads.emplace_back([&addrs, t] {
                ar_alloc().init_thread(static_cast<uint16_t>(t + 1));
                for (int i = 0; i < kPerThread; ++i) {
                    void* p = uballoc::malloc(256);
                    assert(p);
                    auto* bytes = static_cast<uint8_t*>(p);
                    bytes[0] = static_cast<uint8_t>(t + 1);  // ownership marker
                    addrs[t].push_back(reinterpret_cast<uintptr_t>(p));
                }
            });
        }
        for (auto& th : threads) th.join();

        std::unordered_set<uintptr_t> unique;
        for (int t = 0; t < kThreads; ++t) {
            for (uintptr_t v : addrs[t]) {
                assert(unique.insert(v).second);
                auto* bytes = static_cast<const uint8_t*>(reinterpret_cast<void*>(v));
                assert(bytes[0] == t + 1);  // data intact
            }
        }

        // Main thread frees for all workers (cross-thread free).
        for (int t = 0; t < kThreads; ++t) {
            for (uintptr_t v : addrs[t]) uballoc::free(reinterpret_cast<void*>(v));
        }
        std::cout << "  OK: " << unique.size() << " unique addresses, no corruption" << std::endl;
        _exit(0);
    }
    int st;
    waitpid(pid, &st, 0);
    check_child(st);
}

// -----------------------------------------------------------------------------
// DT05-6 R6: per-rank bracket isolation
// -----------------------------------------------------------------------------
void dt05_6_rank_isolation(const std::string& heap_id) {
    std::cout << "[DT05-6] P0/P1 allocations in non-overlapping ranges..." << std::endl;
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

        // P1 allocates 1000 x 256B, records its [lo, hi] range.
        uintptr_t lo1 = ~uintptr_t(0), hi1 = 0;
        for (int i = 0; i < 1000; ++i) {
            void* p = uballoc::malloc(256);
            assert(p);
            uintptr_t v = reinterpret_cast<uintptr_t>(p);
            if (v < lo1) lo1 = v;
            if (v > hi1) hi1 = v;
        }

        // Read P0's [lo0, hi0] and verify non-overlap with a large gap.
        uintptr_t lo0 = 0, hi0 = 0;
        ar_read_fd(fds[0], &lo0, sizeof(lo0));
        ar_read_fd(fds[0], &hi0, sizeof(hi0));

        assert(ptr_diff(lo0, lo1) >= (1ULL << 30));

        ar_write_byte(rp[1], 'C');
        _exit(0);
    }

    ::close(fds[0]);
    ::close(rp[1]);
    ar_parent_init();

    // P0 allocates 1000 x 256B, records its [lo, hi] range.
    uintptr_t lo0 = ~uintptr_t(0), hi0 = 0;
    for (int i = 0; i < 1000; ++i) {
        void* p = uballoc::malloc(256);
        assert(p);
        uintptr_t v = reinterpret_cast<uintptr_t>(p);
        if (v < lo0) lo0 = v;
        if (v > hi0) hi0 = v;
    }

    ar_write_byte(fds[1], 'G');
    ar_write_fd(fds[1], &lo0, sizeof(lo0));
    ar_write_fd(fds[1], &hi0, sizeof(hi0));

    assert(ar_read_byte(rp[0]) == 'C');

    int st;
    waitpid(pid, &st, 0);
    check_child(st);
    ::close(fds[1]);
    ::close(rp[0]);
    ar_alloc().reset();
}

// -----------------------------------------------------------------------------
// DT05-7 R7: cross-process free + owner reuse
// -----------------------------------------------------------------------------
void dt05_7_cross_proc_free(const std::string& heap_id) {
    std::cout << "[DT05-7] cross-proc free: full-slab remote free -> owner reuse..." << std::endl;
    ::setenv("UBALLOC_AFFINITY_MODE", "reuse-first", 1);

    int fds[2], rp[2];
    assert(pipe(fds) == 0);
    assert(pipe(rp) == 0);
    constexpr int kBlocks = 256;

    ar_set_heap_id(heap_id);
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        ::close(fds[1]);
        ::close(rp[0]);
        assert(ar_read_byte(fds[0]) == 'G');
        ar_child_init();

        for (int i = 0; i < kBlocks; ++i) {
            uint64_t v = 0;
            ar_read_fd(fds[0], &v, sizeof(v));
            auto* p = reinterpret_cast<uint8_t*>(v);
            assert(p[i] == static_cast<uint8_t>(i + 1));  // pattern intact
            uballoc::free(p);                             // cross-process free
        }
        ar_write_byte(rp[1], 'D');
        _exit(0);
    }

    ::close(fds[0]);
    ::close(rp[1]);
    ar_parent_init();
    std::vector<uintptr_t> freed;
    for (int i = 0; i < kBlocks; ++i) {
        void* p = uballoc::malloc(256);
        assert(p);
        auto* bytes = static_cast<uint8_t*>(p);
        bytes[i] = static_cast<uint8_t>(i + 1);
        freed.push_back(reinterpret_cast<uintptr_t>(p));
    }
    ar_write_byte(fds[1], 'G');
    for (uintptr_t v : freed) ar_write_fd(fds[1], &v, sizeof(v));

    assert(ar_read_byte(rp[0]) == 'D');

    // Discovery refresh: the huge grow path runs check_remote_segments(),
    // wiring cross_free_ptrs_[1] for the owner.
    void* refresh = uballoc::malloc(4 * 1024 * 1024);
    assert(refresh);
    uballoc::free(refresh);

    // Owner (P0) reuses the recycled slabs via the cross_free pop.
    std::unordered_set<uintptr_t> old_set(freed.begin(), freed.end());
    int reused = 0;
    for (int i = 0; i < kBlocks; ++i) {
        void* p = uballoc::malloc(256);
        assert(p);
        if (old_set.count(reinterpret_cast<uintptr_t>(p))) ++reused;
    }
    assert(reused == kBlocks);  // both slabs recycled back to the owner

    // Restore affinity for any later cases in this process.
    uballoc::affinity_mode().store(static_cast<size_t>(uballoc::AffinityMode::Strict),
                                   std::memory_order_release);
    ::unsetenv("UBALLOC_AFFINITY_MODE");

    int st;
    waitpid(pid, &st, 0);
    check_child(st);
    ::close(fds[1]);
    ar_alloc().reset();
    std::cout << "  OK: cross-process free verified, owner reused " << reused
              << "/" << kBlocks << " blocks" << std::endl;
}

}  // namespace

int main() {
    std::string id = ar_heap_id("ar05");
    cleanup_all_distributed_shms(8, id);

    ar_pre_case_cleanup(id);
    dt05_1_routing_boundaries(id);
    ar_pre_case_cleanup(id);
    dt05_2_block_reuse(id);
    ar_pre_case_cleanup(id);
    dt05_3_slab_reuse(id);
    ar_pre_case_cleanup(id);
    dt05_4_cross_segment(id);
    ar_pre_case_cleanup(id);
    dt05_5_thread_concurrency(id);
    ar_pre_case_cleanup(id);
    dt05_6_rank_isolation(id);
    ar_pre_case_cleanup(id);
    dt05_7_cross_proc_free(id);

    cleanup_all_distributed_shms(8, id);
    std::cout << "\n=== AR-05 DT passed ===" << std::endl;
    return 0;
}
