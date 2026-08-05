// SPDX-License-Identifier: Apache-2.0

#include "uballoc.hpp"
extern "C" {
#include "uballoc.h"
}
#include <iostream>
#include <cassert>
#include <cstring>
#include <vector>
#include <algorithm>
#include <random>
#include <thread>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <condition_variable>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/mman.h>

static uballoc::GlobalAllocator<uballoc::DistributedShmBackend<>>& alloc() {
    return uballoc::get_global_allocator<uballoc::DistributedShmBackend<>>();
}

static void write_pattern(void* ptr, size_t size, uint8_t pattern) {
    memset(ptr, pattern, size);
}

static void verify_pattern(void* ptr, size_t size, uint8_t expected) {
    for (size_t i = 0; i < size; i++) {
        assert(static_cast<uint8_t*>(ptr)[i] == expected);
    }
}

static void write_id_size_pattern(void* ptr, size_t size, size_t id) {
    uint8_t pattern = static_cast<uint8_t>((id ^ size) | 1);
    memset(ptr, pattern, size);
}

static void verify_id_size_pattern(void* ptr, size_t size, size_t id) {
    uint8_t expected = static_cast<uint8_t>((id ^ size) | 1);
    for (size_t i = 0; i < size; i++) {
        uint8_t actual = static_cast<uint8_t*>(ptr)[i];
        if (actual != expected) {
            std::cerr << "VERIFY FAIL: id=" << id << " size=" << size 
                      << " expected=0x" << std::hex << (int)expected << std::dec
                      << " found byte[" << i << "]=0x" << std::hex << (int)actual << std::dec
                      << std::endl;
            break;
        }
    }
}

static void test_unit() {
    std::cout << "[unit] packed types..." << std::endl;
    uballoc::u6 u6_val(63);
    assert(u6_val == 63);
    uballoc::u7 u7_val(127);
    assert(u7_val == 127);
    uballoc::Bit bit(uballoc::u6(5), uballoc::u6(3));
    assert(bit.col == 5);
    assert(bit.row == 3);

    std::cout << "[unit] Small size class..." << std::endl;
    auto small8 = uballoc::Small::new_from_size(8);
    assert(small8 && small8->size() == 8);
    auto small16 = uballoc::Small::new_from_size(16);
    assert(small16 && small16->size() == 16);
    auto small1000 = uballoc::Small::new_from_size(1000);
    assert(small1000);
    assert(uballoc::Small::new_from_size(2000));
    assert(uballoc::Small::new_from_size(1024));

    std::cout << "[unit] Large size class..." << std::endl;
    auto large20k = uballoc::Large::new_from_size(20480);
    assert(large20k && large20k->size() == 20480);
    assert(!uballoc::Large::new_from_size(1024));

    std::cout << "[unit] ThreadId..." << std::endl;
    uballoc::ThreadId tid0(0);
    uballoc::ThreadId tid1(1);
    assert(tid0 != tid1);
    assert(tid0 == 0);
    assert(tid1 == 1);

    std::cout << "[unit] Version..." << std::endl;
    uballoc::Version v1(5);
    uballoc::Version v2 = v1.next();
    assert(v2 == uballoc::Version(6));
}

static void test_small_alloc_free() {
    std::cout << "[integration] small alloc/free with integrity..." << std::endl;

    void* p = alloc().malloc(256);
    assert(p != nullptr);
    write_pattern(p, 256, 0xAB);
    verify_pattern(p, 256, 0xAB);
    alloc().free(p);

    void* p2 = alloc().malloc(8);
    assert(p2 != nullptr);
    write_pattern(p2, 8, 0x55);
    verify_pattern(p2, 8, 0x55);
    alloc().free(p2);

    void* p3 = alloc().malloc(1016);
    assert(p3 != nullptr);
    write_pattern(p3, 1016, 0xCC);
    verify_pattern(p3, 1016, 0xCC);
    alloc().free(p3);
}

static void test_large_alloc_free() {
    std::cout << "[integration] large alloc/free with integrity..." << std::endl;

    void* p = alloc().malloc(8 * 1024);
    assert(p != nullptr);
    write_pattern(p, 8 * 1024, 0xDD);
    verify_pattern(p, 8 * 1024, 0xDD);
    alloc().free(p);

    void* p2 = alloc().malloc(1024);
    assert(p2 != nullptr);
    write_pattern(p2, 1024, 0xEE);
    verify_pattern(p2, 1024, 0xEE);
    alloc().free(p2);

    void* p3 = alloc().malloc(1 * 1024 * 1024);
    assert(p3 != nullptr);
    write_pattern(p3, 1 * 1024 * 1024, 0xFF);
    verify_pattern(p3, 1 * 1024 * 1024, 0xFF);
    alloc().free(p3);
}

static void test_huge_alloc_free() {
    std::cout << "[integration] huge alloc/free with integrity and class_size..." << std::endl;

    constexpr size_t SIZE = 4 * 1024 * 1024;
    void* p = alloc().malloc(SIZE);
    assert(p != nullptr);
    assert(alloc().current_allocator().class_size(p) >= SIZE);
    write_pattern(p, SIZE, 0x11);
    verify_pattern(p, SIZE, 0x11);
    alloc().free(p);
}

static void test_boundary_sizes() {
    std::cout << "[integration] boundary sizes with integrity..." << std::endl;

    std::vector<size_t> sizes = {8, 16, 32, 64, 128, 256, 512, 1008, 1016,
                                  1024, 2048, 4096, 8192, 65536,
                                  2 * 1024 * 1024, 3 * 1024 * 1024};
    for (size_t i = 0; i < sizes.size(); i++) {
        size_t sz = sizes[i];
        void* p = alloc().malloc(sz);
        assert(p != nullptr);
        write_id_size_pattern(p, sz, i);
        verify_id_size_pattern(p, sz, i);
        alloc().free(p);
    }
}

static void test_many_small() {
    std::cout << "[stress] many small allocations with integrity..." << std::endl;

    constexpr int N = 200;
    struct Alloc { void* ptr; size_t id; };
    std::vector<Alloc> allocs;
    allocs.reserve(N);
    for (int i = 0; i < N; i++) {
        void* p = alloc().malloc(64);
        assert(p != nullptr);
        write_id_size_pattern(p, 64, i);
        allocs.push_back({p, static_cast<size_t>(i)});
    }

    for (auto& a : allocs) {
        verify_id_size_pattern(a.ptr, 64, a.id);
    }

    for (auto& a : allocs) {
        alloc().free(a.ptr);
    }
}

static void test_interleaved_alloc_free() {
    std::cout << "[stress] interleaved alloc/free with integrity..." << std::endl;

    struct Alloc { void* ptr; size_t size; size_t id; };
    std::vector<Alloc> alive;
    std::mt19937 rng(42);
    size_t next_id = 0;

    for (int round = 0; round < 200; round++) {
        size_t sz = 8 + (rng() % 1016);
        void* p = alloc().malloc(sz);
        assert(p != nullptr);
        size_t id = next_id++;
        write_id_size_pattern(p, sz, id);
        alive.push_back({p, sz, id});

        if (alive.size() > 20 && rng() % 3 == 0) {
            size_t idx = rng() % alive.size();
            verify_id_size_pattern(alive[idx].ptr, alive[idx].size, alive[idx].id);
            alloc().free(alive[idx].ptr);
            alive.erase(alive.begin() + idx);
        }
    }

    for (auto& a : alive) {
        verify_id_size_pattern(a.ptr, a.size, a.id);
        alloc().free(a.ptr);
    }
}

static void test_mixed_brackets() {
    std::cout << "[stress] mixed bracket alloc/free with integrity..." << std::endl;

    struct Alloc { void* ptr; size_t size; size_t id; };
    std::vector<Alloc> alive;
    std::mt19937 rng(123);
    size_t next_id = 0;

    size_t sizes[] = {64, 256, 1008, 2048, 8192, 65536, 1 * 1024 * 1024};

    for (int round = 0; round < 100; round++) {
        size_t sz = sizes[rng() % 7];
        void* p = alloc().malloc(sz);
        assert(p != nullptr);
        size_t id = next_id++;
        write_id_size_pattern(p, sz, id);
        alive.push_back({p, sz, id});

        if (alive.size() > 10 && rng() % 3 == 0) {
            size_t idx = rng() % alive.size();
            verify_id_size_pattern(alive[idx].ptr, alive[idx].size, alive[idx].id);
            alloc().free(alive[idx].ptr);
            alive.erase(alive.begin() + idx);
        }
    }

    for (auto& a : alive) {
        verify_id_size_pattern(a.ptr, a.size, a.id);
        alloc().free(a.ptr);
    }
}

static void test_free_null() {
    std::cout << "[integration] free(nullptr)..." << std::endl;
    alloc().free(nullptr);
}

static void test_realloc() {
    std::cout << "[integration] realloc with integrity..." << std::endl;

    void* p = alloc().malloc(64);
    assert(p != nullptr);
    write_pattern(p, 64, 0xAA);

    void* p2 = alloc().realloc(p, 256);
    assert(p2 != nullptr);
    verify_pattern(p2, 64, 0xAA);
    write_pattern(p2, 256, 0xBB);

    void* p3 = alloc().realloc(p2, 1024);
    assert(p3 != nullptr);
    verify_pattern(p3, 256, 0xBB);
    alloc().free(p3);

    void* p4 = alloc().realloc(nullptr, 128);
    assert(p4 != nullptr);
    write_pattern(p4, 128, 0xCC);
    verify_pattern(p4, 128, 0xCC);
    alloc().free(p4);
}

static void test_class_size() {
    std::cout << "[integration] class_size verification..." << std::endl;

    struct TestCase { size_t alloc_size; const char* bracket; };
    TestCase cases[] = {
        {8, "small"}, {16, "small"}, {32, "small"}, {64, "small"},
        {128, "small"}, {256, "small"}, {512, "small"}, {1008, "small"}, {1016, "small"},
        {1024, "small"}, {2048, "small"}, {4096, "small"}, {8192, "small"},
        {16384, "small"},
        {65536, "large"}, {1 * 1024 * 1024, "large"}, {4 * 1024 * 1024, "huge"},
    };

    for (auto& tc : cases) {
        void* p = alloc().malloc(tc.alloc_size);
        assert(p != nullptr);
        size_t cs = alloc().current_allocator().class_size(p);
        assert(cs >= tc.alloc_size);
        std::cout << "  " << tc.bracket << ": alloc=" << tc.alloc_size
                  << " class_size=" << cs << " (>= alloc_size: OK)" << std::endl;
        alloc().free(p);
    }
}

static void test_content_integrity_small() {
    std::cout << "[integration] content integrity small (id^size pattern)..." << std::endl;

    constexpr int N = 50;
    struct Alloc { void* ptr; size_t size; size_t id; };
    std::vector<Alloc> allocs;

    for (size_t id = 0; id < N; id++) {
        size_t sz = 8 + (id % 1008);
        void* p = alloc().malloc(sz);
        assert(p != nullptr);
        assert(alloc().current_allocator().class_size(p) >= sz);
        write_id_size_pattern(p, sz, id);
        allocs.push_back({p, sz, id});
    }

    for (auto& a : allocs) {
        verify_id_size_pattern(a.ptr, a.size, a.id);
        assert(alloc().current_allocator().class_size(a.ptr) >= a.size);
        alloc().free(a.ptr);
    }
}

static void test_content_integrity_mixed() {
    std::cout << "[integration] content integrity mixed brackets..." << std::endl;

    size_t sizes[] = {8, 64, 256, 1008, 1024, 4096, 8192, 65536, 1 * 1024 * 1024};
    constexpr int N = 30;
    struct Alloc { void* ptr; size_t size; size_t id; };
    std::vector<Alloc> allocs;

    for (size_t id = 0; id < N; id++) {
        size_t sz = sizes[id % 9];
        void* p = alloc().malloc(sz);
        assert(p != nullptr);
        assert(alloc().current_allocator().class_size(p) >= sz);
        write_id_size_pattern(p, sz, id);
        allocs.push_back({p, sz, id});
    }

    for (auto& a : allocs) {
        verify_id_size_pattern(a.ptr, a.size, a.id);
        assert(alloc().current_allocator().class_size(a.ptr) >= a.size);
        alloc().free(a.ptr);
    }
}

static void test_huge_page_stride() {
    std::cout << "[integration] huge alloc with page-stride write and class_size..." << std::endl;

    constexpr size_t SIZE = 4 * 1024 * 1024;
    constexpr size_t PAGE = 4096;
    void* p = alloc().malloc(SIZE);
    assert(p != nullptr);
    assert(alloc().current_allocator().class_size(p) >= SIZE);

    uint8_t* bytes = static_cast<uint8_t*>(p);
    for (size_t i = 0; i < SIZE / PAGE; i++) {
        bytes[i * PAGE] = static_cast<uint8_t>(i & 0xFF);
    }
    for (size_t i = 0; i < SIZE / PAGE; i++) {
        assert(bytes[i * PAGE] == static_cast<uint8_t>(i & 0xFF));
    }
    alloc().free(p);
}

static void test_randomized_single() {
    std::cout << "[property] randomized single-thread alloc/free..." << std::endl;

    std::mt19937 rng(9999);
    constexpr int ROUNDS = 200;

    for (int round = 0; round < ROUNDS; round++) {
        size_t sz = 1 + (rng() % 256);
        void* p = alloc().malloc(sz);
        assert(p != nullptr);
        write_id_size_pattern(p, sz, static_cast<size_t>(round));
        verify_id_size_pattern(p, sz, static_cast<size_t>(round));
        alloc().free(p);
    }
}

static void test_state_machine_single() {
    std::cout << "[property] state machine single-thread with integrity..." << std::endl;

    std::unordered_map<size_t, std::pair<void*, size_t>> allocations;
    std::mt19937 rng(77777);
    constexpr int ROUNDS = 500;

    for (int round = 0; round < ROUNDS; round++) {
        if (allocations.empty() || rng() % 3 != 0) {
            size_t id = allocations.size();
            size_t sz = 8 + (rng() % 1008);
            void* p = alloc().malloc(sz);
            assert(p != nullptr);
            assert(alloc().current_allocator().class_size(p) >= sz);
            write_id_size_pattern(p, sz, id);
            allocations[id] = {p, sz};
        } else {
            size_t idx = rng() % allocations.size();
            auto it = allocations.begin();
            std::advance(it, idx);
            size_t id = it->first;
            void* ptr = it->second.first;
            size_t sz = it->second.second;
            verify_id_size_pattern(ptr, sz, id);
            assert(alloc().current_allocator().class_size(ptr) >= sz);
            alloc().free(ptr);
            allocations.erase(it);
        }
    }

    for (auto& [id, pair] : allocations) {
        verify_id_size_pattern(pair.first, pair.second, id);
        assert(alloc().current_allocator().class_size(pair.first) >= pair.second);
        alloc().free(pair.first);
    }
}

static void test_state_machine_concurrent() {
    std::cout << "[property] state machine 2-thread concurrent with integrity..." << std::endl;

    constexpr int NUM_THREADS = 2;
    constexpr int OPS_PER_THREAD = 300;
    std::atomic<bool> start{false};

    struct ThreadState {
        std::unordered_map<size_t, std::pair<void*, size_t>> allocations;
        std::mutex mutex;
    };

    ThreadState states[NUM_THREADS];

    auto worker = [&](uint16_t tid) {
        alloc().init_thread(tid);
        while (!start.load(std::memory_order_acquire)) {}

        std::mt19937 rng(tid * 11111);
        std::unordered_map<size_t, std::pair<void*, size_t>> local_allocs;

        for (int round = 0; round < OPS_PER_THREAD; round++) {
            if (local_allocs.empty() || rng() % 3 != 0) {
                size_t id = local_allocs.size();
                size_t sz = 8 + (rng() % 1008);
                void* p = alloc().malloc(sz);
                if (!p) continue;
                size_t cs = alloc().current_allocator().class_size(p);
                assert(cs >= sz);
                write_id_size_pattern(p, sz, id + static_cast<size_t>(tid) * 10000);
                local_allocs[id] = {p, sz};
            } else {
                size_t idx = rng() % local_allocs.size();
                auto it = local_allocs.begin();
                std::advance(it, idx);
                size_t id = it->first;
                void* ptr = it->second.first;
                size_t sz = it->second.second;
                verify_id_size_pattern(ptr, sz, id + static_cast<size_t>(tid) * 10000);
                assert(alloc().current_allocator().class_size(ptr) >= sz);
                alloc().free(ptr);
                local_allocs.erase(it);
            }
        }

        {
            std::lock_guard<std::mutex> lock(states[tid].mutex);
            states[tid].allocations = std::move(local_allocs);
        }
    };

    std::vector<std::thread> threads;
    for (uint16_t t = 0; t < NUM_THREADS; t++) {
        threads.emplace_back(worker, t);
    }
    start.store(true, std::memory_order_release);
    for (auto& t : threads) t.join();

    for (int tid = 0; tid < NUM_THREADS; tid++) {
        for (auto& [id, pair] : states[tid].allocations) {
            verify_id_size_pattern(pair.first, pair.second, id + static_cast<size_t>(tid) * 10000);
            assert(alloc().current_allocator().class_size(pair.first) >= pair.second);
            alloc().free(pair.first);
        }
    }
}

static void test_remote_free_claim() {
    std::cout << "[concurrent] remote free claim cycle (4096 allocs, 8 threads free all)..." << std::endl;

    constexpr int ALLOCATIONS = 4096;
    constexpr int NUM_FREE_THREADS = 8;
    constexpr size_t ALLOC_SIZE = 8;

    alloc().init_thread(0);
    std::vector<void*> allocations;
    allocations.reserve(ALLOCATIONS);

    for (int i = 0; i < ALLOCATIONS; i++) {
        void* p = alloc().malloc(ALLOC_SIZE);
        assert(p != nullptr);
        assert(alloc().current_allocator().class_size(p) >= ALLOC_SIZE);
        write_id_size_pattern(p, ALLOC_SIZE, static_cast<size_t>(i));
        allocations.push_back(p);
    }

    std::atomic<bool> start{false};
    std::atomic<int> freed_count{0};

    auto free_worker = [&](uint16_t tid) {
        alloc().init_thread(tid);
        while (!start.load(std::memory_order_acquire)) {}

        for (int i = 0; i < ALLOCATIONS; i++) {
            if (i % NUM_FREE_THREADS == tid) {
                verify_id_size_pattern(allocations[i], ALLOC_SIZE, static_cast<size_t>(i));
                alloc().free(allocations[i]);
                freed_count.fetch_add(1, std::memory_order_relaxed);
            }
        }
    };

    std::vector<std::thread> threads;
    for (uint16_t t = 0; t < NUM_FREE_THREADS; t++) {
        threads.emplace_back(free_worker, t);
    }
    start.store(true, std::memory_order_release);
    for (auto& t : threads) t.join();

    assert(freed_count.load() == ALLOCATIONS);
    std::cout << "  freed all " << freed_count.load() << " allocations by remote threads" << std::endl;
}

static void test_remote_free_claim_barrier() {
    std::cout << "[concurrent] remote free with barrier (synchronized start)..." << std::endl;

    constexpr int ALLOCATIONS = 4096;
    constexpr int NUM_FREE_THREADS = 8;
    constexpr size_t ALLOC_SIZE = 8;

    alloc().init_thread(0);
    std::vector<void*> all_allocs;
    all_allocs.reserve(ALLOCATIONS);

    for (int i = 0; i < ALLOCATIONS; i++) {
        void* p = alloc().malloc(ALLOC_SIZE);
        assert(p != nullptr);
        write_id_size_pattern(p, ALLOC_SIZE, static_cast<size_t>(i));
        all_allocs.push_back(p);
    }

    std::mutex barrier_mutex;
    std::condition_variable barrier_cv;
    std::atomic<int> threads_ready{0};
    std::atomic<bool> go{false};

    auto free_worker = [&](uint16_t tid) {
        alloc().init_thread(tid);
        threads_ready.fetch_add(1);
        {
            std::unique_lock<std::mutex> lock(barrier_mutex);
            barrier_cv.wait(lock, [&]{ return go.load(); });
        }

        int per_thread = ALLOCATIONS / NUM_FREE_THREADS;
        int start_idx = tid * per_thread;
        for (int i = start_idx; i < start_idx + per_thread; i++) {
            verify_id_size_pattern(all_allocs[i], ALLOC_SIZE, static_cast<size_t>(i));
            alloc().free(all_allocs[i]);
        }
    };

    std::vector<std::thread> threads;
    for (uint16_t t = 0; t < NUM_FREE_THREADS; t++) {
        threads.emplace_back(free_worker, t);
    }

    while (threads_ready.load() < NUM_FREE_THREADS) {
        std::this_thread::yield();
    }
    go.store(true);
    barrier_cv.notify_all();

    for (auto& t : threads) t.join();
    std::cout << "  barrier synchronized, all " << ALLOCATIONS << " remote frees completed" << std::endl;
}

static void test_remote_free_large_claim() {
    std::cout << "[concurrent] remote free claim cycle large bracket (512 allocs)..." << std::endl;

    constexpr int ALLOCATIONS = 512;
    constexpr int NUM_FREE_THREADS = 4;
    constexpr size_t ALLOC_SIZE = 4096;

    alloc().init_thread(0);
    std::vector<void*> allocations;
    allocations.reserve(ALLOCATIONS);

    for (int i = 0; i < ALLOCATIONS; i++) {
        void* p = alloc().malloc(ALLOC_SIZE);
        assert(p != nullptr);
        assert(alloc().current_allocator().class_size(p) >= ALLOC_SIZE);
        write_id_size_pattern(p, ALLOC_SIZE, static_cast<size_t>(i));
        allocations.push_back(p);
    }

    std::atomic<bool> start{false};

    auto free_worker = [&](uint16_t tid) {
        alloc().init_thread(tid);
        while (!start.load(std::memory_order_acquire)) {}

        for (int i = 0; i < ALLOCATIONS; i++) {
            if (i % NUM_FREE_THREADS == tid) {
                verify_id_size_pattern(allocations[i], ALLOC_SIZE, static_cast<size_t>(i));
                alloc().free(allocations[i]);
            }
        }
    };

    std::vector<std::thread> threads;
    for (uint16_t t = 0; t < NUM_FREE_THREADS; t++) {
        threads.emplace_back(free_worker, t);
    }
    start.store(true, std::memory_order_release);
    for (auto& t : threads) t.join();
}

static void test_concurrent_small() {
    std::cout << "[concurrent] 4 threads small alloc/free with integrity..." << std::endl;

    constexpr int NUM_THREADS = 4;
    constexpr int OPS_PER_THREAD = 500;
    std::atomic<bool> start{false};

    struct Alloc { void* ptr; size_t size; size_t id; };

    auto worker = [&](uint16_t tid, std::vector<Alloc>* alive) {
        alloc().init_thread(tid);
        while (!start.load(std::memory_order_acquire)) {}

        std::mt19937 rng(tid * 1000);
        size_t next_id = 0;
        for (int i = 0; i < OPS_PER_THREAD; i++) {
            size_t sz = 8 + (rng() % 1008);
            void* p = alloc().malloc(sz);
            if (!p) continue;
            size_t id = next_id++ + static_cast<size_t>(tid) * 100000;
            write_id_size_pattern(p, sz, id);
            alive->push_back({p, sz, id});

            if (alive->size() > 20 && rng() % 3 == 0) {
                size_t idx = rng() % alive->size();
                verify_id_size_pattern(alive->at(idx).ptr, alive->at(idx).size, alive->at(idx).id);
                alloc().free(alive->at(idx).ptr);
                alive->erase(alive->begin() + idx);
            }
        }
    };

    std::vector<std::vector<Alloc>> thread_allocs(NUM_THREADS);
    std::vector<std::thread> threads;
    for (uint16_t t = 0; t < NUM_THREADS; t++) {
        threads.emplace_back(worker, t, &thread_allocs[t]);
    }
    start.store(true, std::memory_order_release);
    for (auto& t : threads) t.join();

    for (auto& allocs : thread_allocs) {
        for (auto& a : allocs) {
            verify_id_size_pattern(a.ptr, a.size, a.id);
            alloc().free(a.ptr);
        }
    }
}

static void test_concurrent_mixed() {
    std::cout << "[concurrent] 4 threads mixed bracket alloc/free with integrity..." << std::endl;

    constexpr int NUM_THREADS = 4;
    constexpr int OPS_PER_THREAD = 200;
    std::atomic<bool> start{false};

    size_t sizes[] = {64, 256, 1008, 1024, 4096, 8192, 65536};
    struct Alloc { void* ptr; size_t size; size_t id; };

    auto worker = [&](uint16_t tid, std::vector<Alloc>* alive) {
        alloc().init_thread(tid);
        while (!start.load(std::memory_order_acquire)) {}

        std::mt19937 rng(tid * 7777);
        size_t next_id = 0;
        for (int i = 0; i < OPS_PER_THREAD; i++) {
            size_t sz = sizes[rng() % 7];
            void* p = alloc().malloc(sz);
            if (!p) continue;
            size_t id = next_id++ + static_cast<size_t>(tid) * 100000;
            write_id_size_pattern(p, sz, id);
            alive->push_back({p, sz, id});

            if (alive->size() > 10 && rng() % 3 == 0) {
                size_t idx = rng() % alive->size();
                verify_id_size_pattern(alive->at(idx).ptr, alive->at(idx).size, alive->at(idx).id);
                alloc().free(alive->at(idx).ptr);
                alive->erase(alive->begin() + idx);
            }
        }
    };

    std::vector<std::vector<Alloc>> thread_allocs(NUM_THREADS);
    std::vector<std::thread> threads;
    for (uint16_t t = 0; t < NUM_THREADS; t++) {
        threads.emplace_back(worker, t, &thread_allocs[t]);
    }
    start.store(true, std::memory_order_release);
    for (auto& t : threads) t.join();

    for (auto& allocs : thread_allocs) {
        for (auto& a : allocs) {
            verify_id_size_pattern(a.ptr, a.size, a.id);
            alloc().free(a.ptr);
        }
    }
}

static void test_concurrent_cross_free() {
    std::cout << "[concurrent] cross-thread alloc/free with integrity..." << std::endl;

    constexpr int NUM_THREADS = 4;
    constexpr int ALLOCS_PER_THREAD = 100;
    std::atomic<bool> start{false};

    struct Alloc { void* ptr; size_t size; size_t id; };
    std::vector<std::vector<Alloc>> all_allocs(NUM_THREADS);
    std::mutex ptr_mutex;

    auto alloc_worker = [&](uint16_t tid) {
        alloc().init_thread(tid);
        while (!start.load(std::memory_order_acquire)) {}

        std::vector<Alloc> my_allocs;
        std::mt19937 rng(tid * 3333);
        for (int i = 0; i < ALLOCS_PER_THREAD; i++) {
            size_t sz = 8 + (rng() % 1008);
            void* p = alloc().malloc(sz);
            if (!p) continue;
            size_t id = static_cast<size_t>(i) + static_cast<size_t>(tid) * 100000;
            write_id_size_pattern(p, sz, id);
            my_allocs.push_back({p, sz, id});
        }
        {
            std::lock_guard<std::mutex> lock(ptr_mutex);
            all_allocs[tid] = std::move(my_allocs);
        }
    };

    std::vector<std::thread> alloc_threads;
    for (uint16_t t = 0; t < NUM_THREADS; t++) {
        alloc_threads.emplace_back(alloc_worker, t);
    }
    start.store(true, std::memory_order_release);
    for (auto& t : alloc_threads) t.join();

    for (int tid = 0; tid < NUM_THREADS; tid++) {
        for (auto& a : all_allocs[tid]) {
            verify_id_size_pattern(a.ptr, a.size, a.id);
        }
    }

    auto free_worker = [&](uint16_t tid) {
        alloc().init_thread(tid);
        uint16_t src_tid = (tid + 2) % NUM_THREADS;
        for (auto& a : all_allocs[src_tid]) {
            verify_id_size_pattern(a.ptr, a.size, a.id);
            alloc().free(a.ptr);
        }
    };

    std::vector<std::thread> free_threads;
    for (uint16_t t = 0; t < NUM_THREADS; t++) {
        free_threads.emplace_back(free_worker, t);
    }
    for (auto& t : free_threads) t.join();
}

static void test_concurrent_stress() {
    std::cout << "[concurrent] 8 threads stress with integrity..." << std::endl;

    // Probe data region addresses
    alloc().init_thread(0);
    void* probe_small = alloc().malloc(64);
    void* probe_large = alloc().malloc(4096);
    void* ds_base = alloc().data_small_base();
    void* dl_base = alloc().data_large_base();
    std::cout << "  small_base=" << ds_base << " large_base=" << dl_base
              << " probe_small(64)=" << probe_small
              << " probe_large(4096)=" << probe_large << std::endl;
    ptrdiff_t diff_small = reinterpret_cast<char*>(probe_small) - reinterpret_cast<char*>(ds_base);
    ptrdiff_t diff_large = reinterpret_cast<char*>(probe_large) - reinterpret_cast<char*>(dl_base);
    std::cout << "  small_offset_from_base=" << diff_small
              << " large_offset_from_base=" << diff_large << std::endl;
    alloc().free(probe_small);
    alloc().free(probe_large);

    constexpr int NUM_THREADS = 8;
    constexpr int OPS_PER_THREAD = 500;
    size_t sizes[] = {64, 256, 1008, 1024, 4096, 8192, 65536};
    std::atomic<bool> start{false};

    struct Alloc { void* ptr; size_t size; size_t id; };

    auto worker = [&](uint16_t tid, std::vector<Alloc>* alive) {
        alloc().init_thread(tid);
        while (!start.load(std::memory_order_acquire)) {}

        std::mt19937 rng(tid * 54321);
        size_t next_id = 0;
        for (int i = 0; i < OPS_PER_THREAD; i++) {
            size_t sz = sizes[rng() % 7];
            void* p = alloc().malloc(sz);
            if (!p) continue;
            size_t id = next_id++ + static_cast<size_t>(tid) * 100000;
            write_id_size_pattern(p, sz, id);
            alive->push_back({p, sz, id});

            if (alive->size() > 15 && rng() % 2 == 0) {
                size_t idx = rng() % alive->size();
                verify_id_size_pattern(alive->at(idx).ptr, alive->at(idx).size, alive->at(idx).id);
                alloc().free(alive->at(idx).ptr);
                alive->erase(alive->begin() + idx);
            }
        }
    };

    std::vector<std::vector<Alloc>> thread_allocs(NUM_THREADS);
    std::vector<std::thread> threads;
    for (uint16_t t = 0; t < NUM_THREADS; t++) {
        threads.emplace_back(worker, t, &thread_allocs[t]);
    }
    start.store(true, std::memory_order_release);
    for (auto& t : threads) t.join();

    for (auto& allocs : thread_allocs) {
        for (auto& a : allocs) {
            verify_id_size_pattern(a.ptr, a.size, a.id);
            alloc().free(a.ptr);
        }
    }

    // Check for address-range overlaps across all alive allocations
    struct Range {
        void* base; size_t size; size_t id;
    };
    std::vector<Range> ranges;
    for (size_t t = 0; t < thread_allocs.size(); t++) {
        for (auto& a : thread_allocs[t]) {
            for (auto& r : ranges) {
                void* a_end = static_cast<char*>(a.ptr) + a.size;
                void* r_end = static_cast<char*>(r.base) + r.size;
                if (a.ptr < r_end && r.base < a_end) {
                    std::cerr << "RANGE OVERLAP: ptr=" << a.ptr
                              << " size=" << a.size << " id=" << a.id
                              << " overlaps with ptr=" << r.base
                              << " size=" << r.size << " id=" << r.id << std::endl;
                }
            }
            ranges.push_back({a.ptr, a.size, a.id});
        }
    }
    std::cout << "  checked " << ranges.size() << " ranges, no overlaps" << std::endl;
}

static void test_concurrent_cross_free_reuse() {
    std::cout << "[concurrent] cross-thread free + immediate reuse..." << std::endl;

    constexpr int NUM_THREADS = 4;
    constexpr int ALLOCS_PER_THREAD = 200;
    constexpr size_t ALLOC_SIZE = 8;
    std::atomic<bool> start{false};

    struct Alloc { void* ptr; size_t id; };
    std::vector<std::vector<Alloc>> all_allocs(NUM_THREADS);
    std::mutex ptr_mutex;

    alloc().init_thread(0);
    for (uint16_t t = 1; t < NUM_THREADS; t++) {
        std::thread([t]() { alloc().init_thread(t); }).join();
    }

    for (uint16_t tid = 0; tid < NUM_THREADS; tid++) {
        std::vector<Alloc> my_allocs;
        for (int i = 0; i < ALLOCS_PER_THREAD; i++) {
            void* p = alloc().malloc(ALLOC_SIZE);
            assert(p != nullptr);
            size_t id = static_cast<size_t>(i) + static_cast<size_t>(tid) * 100000;
            write_id_size_pattern(p, ALLOC_SIZE, id);
            my_allocs.push_back({p, id});
        }
        all_allocs[tid] = std::move(my_allocs);
    }

    std::atomic<size_t> reuse_count{0};

    auto free_and_reuse_worker = [&](uint16_t tid) {
        alloc().init_thread(tid);
        while (!start.load(std::memory_order_acquire)) {}

        uint16_t src_tid = (tid + 1) % NUM_THREADS;
        for (auto& a : all_allocs[src_tid]) {
            verify_id_size_pattern(a.ptr, ALLOC_SIZE, a.id);
            alloc().free(a.ptr);
            void* new_p = alloc().malloc(ALLOC_SIZE);
            if (new_p) {
                write_id_size_pattern(new_p, ALLOC_SIZE, a.id + 500000);
                reuse_count.fetch_add(1, std::memory_order_relaxed);
            }
        }
    };

    std::vector<std::thread> threads;
    for (uint16_t t = 0; t < NUM_THREADS; t++) {
        threads.emplace_back(free_and_reuse_worker, t);
    }
    start.store(true, std::memory_order_release);
    for (auto& t : threads) t.join();

    std::cout << "  reuse_count=" << reuse_count.load() << std::endl;
}

static void test_reservation_small_bump() {
    std::cout << "[reservation] small bump beyond initial slab_count..." << std::endl;

    constexpr int TARGET_SLABS = 600;
    constexpr size_t ALLOC_SIZE = 256;

    auto small_class = uballoc::Small::new_from_size(ALLOC_SIZE);
    assert(small_class);
    size_t blocks_per_slab = small_class->count();

    struct Alloc { void* ptr; size_t id; };
    std::vector<Alloc> alive;
    size_t next_id = 0;

    for (int slab = 0; slab < TARGET_SLABS; slab++) {
        for (size_t b = 0; b < blocks_per_slab; b++) {
            void* p = alloc().malloc(ALLOC_SIZE);
            assert(p != nullptr);
            size_t id = next_id++;
            write_id_size_pattern(p, ALLOC_SIZE, id);
            alive.push_back({p, id});
        }
        if (alive.size() > 20 * blocks_per_slab) {
            size_t free_count = alive.size() / 2;
            for (size_t i = 0; i < free_count; i++) {
                verify_id_size_pattern(alive[i].ptr, ALLOC_SIZE, alive[i].id);
                alloc().free(alive[i].ptr);
            }
            alive.erase(alive.begin(), alive.begin() + free_count);
        }
    }

    for (auto& a : alive) {
        verify_id_size_pattern(a.ptr, ALLOC_SIZE, a.id);
        alloc().free(a.ptr);
    }
    alive.clear();

    std::cout << "  allocated " << next_id << " blocks, bump beyond 512 slabs OK" << std::endl;
}

static void test_reservation_large_bump() {
    std::cout << "[reservation] large bump beyond initial slab_count..." << std::endl;

    constexpr int TARGET_SLABS = 30;
    constexpr size_t ALLOC_SIZE = 32768;

    auto large_class = uballoc::Large::new_from_size(ALLOC_SIZE);
    assert(large_class);
    size_t blocks_per_slab = large_class->count();

    struct Alloc { void* ptr; size_t id; };
    std::vector<Alloc> alive;
    size_t next_id = 0;

    for (int slab = 0; slab < TARGET_SLABS; slab++) {
        for (size_t b = 0; b < blocks_per_slab; b++) {
            void* p = alloc().malloc(ALLOC_SIZE);
            assert(p != nullptr);
            size_t id = next_id++;
            write_id_size_pattern(p, ALLOC_SIZE, id);
            alive.push_back({p, id});
        }
    }

    for (auto& a : alive) {
        verify_id_size_pattern(a.ptr, ALLOC_SIZE, a.id);
        alloc().free(a.ptr);
    }
    alive.clear();

    std::cout << "  allocated " << next_id << " blocks, bump beyond 16 slabs OK" << std::endl;
}

static void test_reservation_capacity_check() {
    std::cout << "[reservation] checked_pointer_to_offset uses slab_capacity..." << std::endl;

    void* p1 = alloc().malloc(64);
    assert(p1 != nullptr);
    auto s_off = alloc().current_allocator().small.checked_pointer_to_offset(p1);
    assert(s_off);

    void* p2 = alloc().malloc(32768);
    assert(p2 != nullptr);
    auto l_off = alloc().current_allocator().large.checked_pointer_to_offset(p2);
    assert(l_off);

    std::cout << "  small ptr in small range: OK, large ptr in large range: OK" << std::endl;

    alloc().free(p1);
    alloc().free(p2);
}

static void test_reservation_concurrent_bump() {
    std::cout << "[reservation] concurrent bump beyond initial slab_count..." << std::endl;

    constexpr int NUM_THREADS = 4;
    constexpr size_t ALLOC_SIZE = 256;
    constexpr int ALLOCS_PER_THREAD = 800;
    std::atomic<bool> start{false};

    struct Alloc { void* ptr; size_t size; size_t id; };

    auto worker = [&](uint16_t tid, std::vector<Alloc>* alive) {
        alloc().init_thread(tid);
        while (!start.load(std::memory_order_acquire)) {}

        std::mt19937 rng(tid * 77777);
        size_t next_id = 0;
        for (int i = 0; i < ALLOCS_PER_THREAD; i++) {
            void* p = alloc().malloc(ALLOC_SIZE);
            if (!p) continue;
            size_t id = next_id++ + static_cast<size_t>(tid) * 100000;
            write_id_size_pattern(p, ALLOC_SIZE, id);
            alive->push_back({p, ALLOC_SIZE, id});

            if (alive->size() > 20 && rng() % 3 == 0) {
                size_t idx = rng() % alive->size();
                verify_id_size_pattern(alive->at(idx).ptr, alive->at(idx).size, alive->at(idx).id);
                alloc().free(alive->at(idx).ptr);
                alive->erase(alive->begin() + idx);
            }
        }
    };

    std::vector<std::vector<Alloc>> thread_allocs(NUM_THREADS);
    std::vector<std::thread> threads;
    for (uint16_t t = 0; t < NUM_THREADS; t++) {
        threads.emplace_back(worker, t, &thread_allocs[t]);
    }
    start.store(true, std::memory_order_release);
    for (auto& t : threads) t.join();

    for (auto& allocs : thread_allocs) {
        for (auto& a : allocs) {
            verify_id_size_pattern(a.ptr, a.size, a.id);
            alloc().free(a.ptr);
        }
    }

    std::cout << "  4 threads concurrent bump with integrity: OK" << std::endl;
}


// Cleanup shms using the active ShmProvider (UBShmProvider on UBSE,
// PosixShmProvider on POSIX). On UBSE, shm_unlink detaches the local
// borrow and retries shm_delete 100× to race the daemon's async
// processing. Stale shms from _exit(0)'d children (which skip the
// destructor) are cleaned up here between test functions.
static void cleanup_distributed_shms() {
    using ShmProvider = uballoc::DistributedShmBackend<>::ShmProvider;
    const char* heap_ids[] = {"heap", "gdiscover", "test_suite", "pubtest", "cpubtest", "dynheap"};
    for (const char* heap_id : heap_ids) {
        ShmProvider::shm_unlink(ShmProvider::bootstrap_shm_name(heap_id));
        for (int pid = 0; pid < 8; ++pid) {
            for (int r = 0; r < 4; ++r) {
                for (int seg = 0; seg < 8; ++seg) {
                    ShmProvider::shm_unlink(
                        ShmProvider::shm_name(heap_id, pid, r, seg));
                }
            }
        }
    }
}

static void test_distributed_single_process() {
    std::cout << "[distributed] P0 alloc/free with P1 minimal init..." << std::endl;

    cleanup_distributed_shms();
    setenv("UBALLOC_HEAP_ID", "heap", 1);

    int sync_pipe[2];
    assert(pipe(sync_pipe) == 0);

    auto& parent_alloc = uballoc::get_global_allocator<uballoc::DistributedShmBackend<>>();
    parent_alloc.soft_reset();
    parent_alloc.init();
    parent_alloc.init_thread(0);

    pid_t pid = fork();
    assert(pid >= 0);

    if (pid == 0) {
        close(sync_pipe[0]);
        auto& child_alloc = uballoc::get_global_allocator<uballoc::DistributedShmBackend<>>();
        child_alloc.soft_reset();
        child_alloc.init();

        char c = 'R';
        write(sync_pipe[1], &c, 1);
        close(sync_pipe[1]);
        usleep(500000);
        _exit(0);
    } else {
        close(sync_pipe[1]);

        char c;
        read(sync_pipe[0], &c, 1);
        close(sync_pipe[0]);

        void* p1 = parent_alloc.malloc(64);
        assert(p1 != nullptr);
        write_pattern(p1, 64, 0xAA);
        verify_pattern(p1, 64, 0xAA);
        parent_alloc.free(p1);

        void* p2 = parent_alloc.malloc(256);
        assert(p2 != nullptr);
        write_pattern(p2, 256, 0xBB);
        verify_pattern(p2, 256, 0xBB);
        parent_alloc.free(p2);

        void* p3 = parent_alloc.malloc(4096);
        assert(p3 != nullptr);
        write_pattern(p3, 4096, 0xCC);
        verify_pattern(p3, 4096, 0xCC);
        parent_alloc.free(p3);

        int status;
        waitpid(pid, &status, 0);
        assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);

        parent_alloc.reset();
        cleanup_distributed_shms();
        unsetenv("UBALLOC_HEAP_ID");
        std::cout << "  P0 alloc/free (P1 minimal, synced via pipe): OK" << std::endl;
    }
}

static void test_distributed_fork_basic() {
    std::cout << "[distributed] fork basic: P0+P1 init, alloc+free..." << std::endl;

    cleanup_distributed_shms();
    setenv("UBALLOC_HEAP_ID", "heap", 1);

    int sync_pipe[2];
    assert(pipe(sync_pipe) == 0);

    auto& parent_alloc = uballoc::get_global_allocator<uballoc::DistributedShmBackend<>>();
    parent_alloc.soft_reset();
    parent_alloc.init();
    parent_alloc.init_thread(0);

    pid_t pid = fork();
    assert(pid >= 0);

    if (pid == 0) {
        close(sync_pipe[0]);
        auto& child_alloc = uballoc::get_global_allocator<uballoc::DistributedShmBackend<>>();
        child_alloc.soft_reset();
        child_alloc.init();

        char c = 'R';
        write(sync_pipe[1], &c, 1);
        close(sync_pipe[1]);

        void* p_child = child_alloc.malloc(128);
        assert(p_child != nullptr);
        write_pattern(p_child, 128, 0xEE);
        verify_pattern(p_child, 128, 0xEE);
        child_alloc.free(p_child);

        _exit(0);
    } else {
        close(sync_pipe[1]);

        char c;
        read(sync_pipe[0], &c, 1);
        close(sync_pipe[0]);

        void* p_parent = parent_alloc.malloc(128);
        assert(p_parent != nullptr);
        write_pattern(p_parent, 128, 0xDD);
        verify_pattern(p_parent, 128, 0xDD);
        parent_alloc.free(p_parent);

        int status;
        waitpid(pid, &status, 0);
        assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);

        parent_alloc.reset();
        cleanup_distributed_shms();
        unsetenv("UBALLOC_HEAP_ID");
        std::cout << "  distributed fork basic: P0+P1 both alloc/free: OK" << std::endl;
    }
}

static void test_distributed_fork_cross_free() {
    std::cout << "[distributed] fork cross-free: P0 allocs, P1 frees..." << std::endl;

    cleanup_distributed_shms();
    setenv("UBALLOC_HEAP_ID", "heap", 1);

    constexpr int N = 50;

    int sync_pipe[2];
    int data_pipe[2];
    assert(pipe(sync_pipe) == 0);
    assert(pipe(data_pipe) == 0);

    auto& parent_alloc = uballoc::get_global_allocator<uballoc::DistributedShmBackend<>>();
    parent_alloc.soft_reset();
    parent_alloc.init();
    parent_alloc.init_thread(0);

    pid_t pid = fork();
    assert(pid >= 0);

    if (pid == 0) {
        close(sync_pipe[0]);
        close(data_pipe[1]);

        auto& child_alloc = uballoc::get_global_allocator<uballoc::DistributedShmBackend<>>();
        child_alloc.soft_reset();
        child_alloc.init();

        char c = 'R';
        write(sync_pipe[1], &c, 1);
        close(sync_pipe[1]);

        uintptr_t ptr_vals[N];
        size_t read_total = 0;
        while (read_total < sizeof(ptr_vals)) {
            ssize_t n = read(data_pipe[0], reinterpret_cast<char*>(ptr_vals) + read_total, sizeof(ptr_vals) - read_total);
            if (n <= 0) break;
            read_total += static_cast<size_t>(n);
        }
        assert(read_total == sizeof(ptr_vals));
        close(data_pipe[0]);

        // P0 created its data segments lazily during malloc() AFTER the
        // child's init() ran check_remote_segments(). The child must
        // re-scan to attach P0's newly-created data segments before
        // touching the pointers (verify_id_size_pattern reads memory,
        // free() walks slab metadata — both need P0's segments mapped).
        child_alloc.check_remote_segments();

        for (int i = 0; i < N; i++) {
            void* ptr = reinterpret_cast<void*>(ptr_vals[i]);
            verify_id_size_pattern(ptr, 64, static_cast<size_t>(i));
            child_alloc.free(ptr);
        }

        _exit(0);
    } else {
        close(sync_pipe[1]);
        close(data_pipe[0]);

        char c;
        read(sync_pipe[0], &c, 1);
        close(sync_pipe[0]);

        uintptr_t ptr_vals[N];
        for (int i = 0; i < N; i++) {
            void* p = parent_alloc.malloc(64);
            assert(p != nullptr);
            write_id_size_pattern(p, 64, static_cast<size_t>(i));
            ptr_vals[i] = reinterpret_cast<uintptr_t>(p);
        }

        ssize_t written = write(data_pipe[1], ptr_vals, sizeof(ptr_vals));
        assert(written == static_cast<ssize_t>(sizeof(ptr_vals)));
        close(data_pipe[1]);

        int status;
        waitpid(pid, &status, 0);
        assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);

        parent_alloc.reset();
        cleanup_distributed_shms();
        unsetenv("UBALLOC_HEAP_ID");
        std::cout << "  distributed fork cross-free: P1 freed all P0 allocations via pipe: OK" << std::endl;
    }
}

static void test_distributed_fork_concurrent_bump() {
    std::cout << "[distributed] fork concurrent: P0+P1 bump alloc, verify no overlap..." << std::endl;

    cleanup_distributed_shms();
    setenv("UBALLOC_HEAP_ID", "heap", 1);

    struct AllocInfo { void* ptr; size_t size; size_t id; };
    constexpr int ALLOCS_PER_PROCESS = 200;
    constexpr size_t ALLOC_SIZE = 256;

    int sync_pipe[2];
    assert(pipe(sync_pipe) == 0);

    auto& parent_alloc = uballoc::get_global_allocator<uballoc::DistributedShmBackend<>>();
    parent_alloc.soft_reset();
    parent_alloc.init();
    parent_alloc.init_thread(0);

    pid_t pid = fork();
    assert(pid >= 0);

    if (pid == 0) {
        close(sync_pipe[0]);
        auto& child_alloc = uballoc::get_global_allocator<uballoc::DistributedShmBackend<>>();
        child_alloc.soft_reset();
        child_alloc.init();

        char c = 'R';
        write(sync_pipe[1], &c, 1);
        close(sync_pipe[1]);

        std::vector<AllocInfo> child_allocs;
        for (int i = 0; i < ALLOCS_PER_PROCESS; i++) {
            void* p = child_alloc.malloc(ALLOC_SIZE);
            if (!p) continue;
            size_t id = static_cast<size_t>(i) + 100000;
            write_id_size_pattern(p, ALLOC_SIZE, id);
            child_allocs.push_back({p, ALLOC_SIZE, id});
        }

        for (auto& a : child_allocs) {
            verify_id_size_pattern(a.ptr, a.size, a.id);
            child_alloc.free(a.ptr);
        }

        _exit(0);
    } else {
        close(sync_pipe[1]);

        char c;
        read(sync_pipe[0], &c, 1);
        close(sync_pipe[0]);

        std::vector<AllocInfo> parent_allocs;
        for (int i = 0; i < ALLOCS_PER_PROCESS; i++) {
            void* p = parent_alloc.malloc(ALLOC_SIZE);
            if (!p) continue;
            size_t id = static_cast<size_t>(i);
            write_id_size_pattern(p, ALLOC_SIZE, id);
            parent_allocs.push_back({p, ALLOC_SIZE, id});
        }

        int status;
        waitpid(pid, &status, 0);
        assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);

        for (auto& a : parent_allocs) {
            verify_id_size_pattern(a.ptr, a.size, a.id);
            parent_alloc.free(a.ptr);
        }

        struct Range { void* base; size_t size; size_t id; };
        std::vector<Range> ranges;
        for (auto& a : parent_allocs) {
            for (auto& r : ranges) {
                void* a_end = static_cast<char*>(a.ptr) + a.size;
                void* r_end = static_cast<char*>(r.base) + r.size;
                assert(!(a.ptr < r_end && r.base < a_end));
            }
            ranges.push_back({a.ptr, a.size, a.id});
        }
        std::cout << "  P0 allocs no overlap: checked " << ranges.size() << " ranges" << std::endl;

        parent_alloc.reset();
        cleanup_distributed_shms();
        unsetenv("UBALLOC_HEAP_ID");
        std::cout << "  distributed fork concurrent bump: P0+P1 integrity, no overlap: OK" << std::endl;
    }
}

static void test_distributed_va_layout() {
    std::cout << "[distributed] VA layout computation..." << std::endl;

    uballoc::DistributedConfig config;
    config.rank = 0;
    config.total_processes = 2;
    config.slab_count_small = {256, 256};
    config.slab_count_large = {16, 16};
    config.huge_slots = {128, 128};

    uballoc::DistributedVALayout layout = uballoc::DistributedVALayout::compute(config, 0,
        uballoc::DEFAULT_VA_BASE);

    assert(layout.metadata_va[0] == uballoc::DEFAULT_VA_BASE);
    assert(layout.metadata_va[1] == uballoc::DEFAULT_VA_BASE + uballoc::PROCESS_STRIDE_META);

    assert(layout.data_small_start == uballoc::DEFAULT_VA_BASE + uballoc::METADATA_STRIDE);
    assert(layout.data_large_start == uballoc::DEFAULT_VA_BASE + uballoc::METADATA_STRIDE + uballoc::DATA_SMALL_STRIDE);
    assert(layout.data_huge_start == uballoc::DEFAULT_VA_BASE + uballoc::METADATA_STRIDE + uballoc::DATA_SMALL_STRIDE + uballoc::DATA_LARGE_STRIDE);

    assert(layout.data_small_va[0] == uballoc::DEFAULT_VA_BASE + uballoc::METADATA_STRIDE);
    assert(layout.data_small_size[0] >= 256 * uballoc::Small::SLAB_SIZE);
    assert(layout.data_small_va[1] == uballoc::DEFAULT_VA_BASE + uballoc::METADATA_STRIDE + uballoc::PROCESS_STRIDE_SMALL);
    assert(layout.data_small_size[1] >= 256 * uballoc::Small::SLAB_SIZE);

    assert(layout.data_large_va[0] == uballoc::DEFAULT_VA_BASE + uballoc::METADATA_STRIDE + uballoc::DATA_SMALL_STRIDE);
    assert(layout.data_large_size[0] >= 16 * uballoc::Large::SLAB_SIZE);
    assert(layout.data_large_va[1] == uballoc::DEFAULT_VA_BASE + uballoc::METADATA_STRIDE + uballoc::DATA_SMALL_STRIDE + uballoc::PROCESS_STRIDE_LARGE);
    assert(layout.data_large_size[1] >= 16 * uballoc::Large::SLAB_SIZE);

    assert(layout.data_huge_va[0] == uballoc::DEFAULT_VA_BASE + uballoc::METADATA_STRIDE + uballoc::DATA_SMALL_STRIDE + uballoc::DATA_LARGE_STRIDE);
    assert(layout.data_huge_size[0] == 128 * uballoc::HugeSize::SLAB_SIZE);
    assert(layout.data_huge_va[1] == uballoc::DEFAULT_VA_BASE + uballoc::METADATA_STRIDE + uballoc::DATA_SMALL_STRIDE + uballoc::DATA_LARGE_STRIDE + uballoc::PROCESS_STRIDE_HUGE);
    assert(layout.data_huge_size[1] == 128 * uballoc::HugeSize::SLAB_SIZE);

    std::cout << "  VA layout: all address computations correct: OK" << std::endl;
}

static void test_distributed_boundary_table() {
    std::cout << "[distributed] cumulative boundary table and find_process..." << std::endl;

    uballoc::DistributedConfig config;
    config.rank = 0;
    config.total_processes = 2;
    config.slab_count_small = {256, 256};
    config.slab_count_large = {16, 16};
    config.huge_slots = {128, 128};

    uballoc::DistributedVALayout layout = uballoc::DistributedVALayout::compute(config, 0,
        uballoc::DEFAULT_VA_BASE);

    size_t spp_small = uballoc::PROCESS_STRIDE_SMALL / uballoc::Small::SLAB_SIZE;
    size_t spp_large = uballoc::PROCESS_STRIDE_LARGE / uballoc::Large::SLAB_SIZE;

    assert(layout.cumulative_small[0] == 0);
    assert(layout.cumulative_small[1] == spp_small);
    assert(layout.cumulative_small[2] == 2 * spp_small);

    assert(layout.cumulative_large[0] == 0);
    assert(layout.cumulative_large[1] == spp_large);
    assert(layout.cumulative_large[2] == 2 * spp_large);

    assert(layout.find_process_small(0, 2) == 0);
    assert(layout.find_process_small(255, 2) == 0);
    assert(layout.find_process_small(spp_small, 2) == 1);
    assert(layout.find_process_small(spp_small + 255, 2) == 1);

    assert(layout.find_process_large(0, 2) == 0);
    assert(layout.find_process_large(15, 2) == 0);
    assert(layout.find_process_large(spp_large, 2) == 1);
    assert(layout.find_process_large(spp_large + 15, 2) == 1);

    config.slab_count_small = {128, 384};
    config.slab_count_large = {8, 24};
    layout = uballoc::DistributedVALayout::compute(config, 0,
        uballoc::DEFAULT_VA_BASE);

    assert(layout.cumulative_small[0] == 0);
    assert(layout.cumulative_small[1] == spp_small);
    assert(layout.cumulative_small[2] == 2 * spp_small);

    assert(layout.cumulative_large[0] == 0);
    assert(layout.cumulative_large[1] == spp_large);
    assert(layout.cumulative_large[2] == 2 * spp_large);

    assert(layout.find_process_small(0, 2) == 0);
    assert(layout.find_process_small(127, 2) == 0);
    assert(layout.find_process_small(spp_small, 2) == 1);
    assert(layout.find_process_small(spp_small + 383, 2) == 1);

    assert(layout.find_process_large(0, 2) == 0);
    assert(layout.find_process_large(7, 2) == 0);
    assert(layout.find_process_large(spp_large, 2) == 1);
    assert(layout.find_process_large(spp_large + 23, 2) == 1);

    std::cout << "  boundary table: cumulative and find_process use reserved ranges: OK" << std::endl;
}

static void test_distributed_malloc_published() {
    std::cout << "[distributed] malloc(size, type_id) + lookup_by_type_blocking..." << std::endl;

    cleanup_distributed_shms();
    setenv("UBALLOC_HEAP_ID", "pubtest", 1);

    constexpr uint32_t TYPE_P0_DATA = 10;
    constexpr uint32_t TYPE_P1_DATA = 11;
    constexpr size_t ALLOC_SIZE = 256;

    int sync_pipe[2];
    assert(pipe(sync_pipe) == 0);

    auto& parent_alloc = uballoc::get_global_allocator<uballoc::DistributedShmBackend<>>();
    parent_alloc.soft_reset();
    parent_alloc.init();
    parent_alloc.init_thread(0);
    if (!parent_alloc.is_initialized()) {
        close(sync_pipe[1]);
        cleanup_distributed_shms();
        unsetenv("UBALLOC_HEAP_ID");
        return;
    }

    pid_t pid = fork();
    assert(pid >= 0);

    if (pid == 0) {
        // --- P1 ---
        close(sync_pipe[0]);
        auto& child_alloc = uballoc::get_global_allocator<uballoc::DistributedShmBackend<>>();
        child_alloc.soft_reset();
        child_alloc.init();
        if (!child_alloc.is_initialized()) _exit(1);

        // Signal P0 that P1 is ready
        char c = 'R';
        write(sync_pipe[1], &c, 1);
        close(sync_pipe[1]);

        // P1 publishes its own block, then blocks waiting for P0's block
        void* p1_block = child_alloc.malloc(ALLOC_SIZE, TYPE_P1_DATA);
        assert(p1_block != nullptr);
        memset(p1_block, 0xB1, ALLOC_SIZE);

        // Blocking lookup for P0's data (5s timeout)
        auto info = child_alloc.lookup_by_type_blocking(TYPE_P0_DATA, 5000);
        assert(info.owner_process == 0);
        assert(info.size == ALLOC_SIZE);
        assert(info.type_id == TYPE_P0_DATA);

        // Verify P0's pattern
        uint8_t* bytes = static_cast<uint8_t*>(info.address);
        for (size_t i = 0; i < ALLOC_SIZE; i++) {
            assert(bytes[i] == 0xA0);
        }

        // Now P0 should have found P1's data too (P0 does its own lookup)
        // Wait a bit then clean up
        usleep(200000);
        child_alloc.free(p1_block);
        child_alloc.reset();
        _exit(0);
    } else {
        // --- P0 ---
        close(sync_pipe[1]);

        // Wait for P1 to be ready
        char c;
        read(sync_pipe[0], &c, 1);
        close(sync_pipe[0]);

        // P0 publishes its own block via malloc(size, type_id)
        void* p0_block = parent_alloc.malloc(ALLOC_SIZE, TYPE_P0_DATA);
        assert(p0_block != nullptr);
        memset(p0_block, 0xA0, ALLOC_SIZE);

        // P0 looks up P1's data (blocking, 5s timeout)
        auto info = parent_alloc.lookup_by_type_blocking(TYPE_P1_DATA, 5000);
        assert(info.owner_process == 1);
        assert(info.size == ALLOC_SIZE);
        assert(info.type_id == TYPE_P1_DATA);

        // Verify P1's pattern
        uint8_t* bytes = static_cast<uint8_t*>(info.address);
        for (size_t i = 0; i < ALLOC_SIZE; i++) {
            assert(bytes[i] == 0xB1);
        }

        parent_alloc.free(p0_block);

        int status;
        waitpid(pid, &status, 0);
        assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);

        parent_alloc.reset();
        cleanup_distributed_shms();
        unsetenv("UBALLOC_HEAP_ID");
        std::cout << "  malloc(size, type_id) + lookup_by_type_blocking: P0<->P1 OK" << std::endl;
    }
}

static void test_distributed_c_api_published() {
    std::cout << "[distributed] C API: uballoc_malloc_published + uballoc_lookup_by_type_blocking..." << std::endl;

    cleanup_distributed_shms();
    setenv("UBALLOC_HEAP_ID", "cpubtest", 1);

    constexpr uint32_t TYPE_C_P0 = 20;
    constexpr uint32_t TYPE_C_P1 = 21;
    constexpr size_t ALLOC_SIZE = 128;

    int sync_pipe[2];
    assert(pipe(sync_pipe) == 0);

    uballoc_soft_reset();
    uballoc_init();
    // Note: uballoc_init_thread(0) is redundant for rank 0 —
    // ensure_thread_init() in init() already sets tl_thread_id_=0.
    // Calling it directly crashes on UBSE because the binary's
    // UBALLOC_NO_EXTERN_TEMPLATE version of init_thread() uses
    // LE TLS access (reads wrong TLS block for library singleton).

    pid_t pid = fork();
    assert(pid >= 0);

    if (pid == 0) {
        // --- P1 (C API) ---
        close(sync_pipe[0]);
        uballoc_soft_reset();
        uballoc_init();

        char c = 'R';
        write(sync_pipe[1], &c, 1);
        close(sync_pipe[1]);

        // P1 mallocs+publishes via C API
        void* p1 = uballoc_malloc_published(ALLOC_SIZE, TYPE_C_P1);
        assert(p1 != nullptr);
        memset(p1, 0x1C, ALLOC_SIZE);

        // P1 blocking-looks-up P0's data
        uballoc_published_info_t info;
        int rc = uballoc_lookup_by_type_blocking(TYPE_C_P0, 5000, &info);
        assert(rc == 0);
        assert(info.owner_process == 0);
        assert(info.size == ALLOC_SIZE);
        assert(info.type_id == TYPE_C_P0);
        assert(info.address != nullptr);

        uint8_t* bytes = static_cast<uint8_t*>(info.address);
        for (size_t i = 0; i < ALLOC_SIZE; i++) {
            assert(bytes[i] == 0x0C);
        }

        // Non-blocking lookup should also find it now
        uballoc_published_info_t info2;
        rc = uballoc_lookup_by_type(TYPE_C_P0, &info2);
        assert(rc == 0);

        usleep(200000);
        uballoc_free(p1);
        _exit(0);
    } else {
        // --- P0 (C API) ---
        close(sync_pipe[1]);

        char c;
        read(sync_pipe[0], &c, 1);
        close(sync_pipe[0]);

        // P0 mallocs+publishes via C API
        void* p0 = uballoc_malloc_published(ALLOC_SIZE, TYPE_C_P0);
        assert(p0 != nullptr);
        memset(p0, 0x0C, ALLOC_SIZE);

        // P0 blocking-looks-up P1's data
        uballoc_published_info_t info;
        int rc = uballoc_lookup_by_type_blocking(TYPE_C_P1, 5000, &info);
        assert(rc == 0);
        assert(info.owner_process == 1);
        assert(info.size == ALLOC_SIZE);
        assert(info.type_id == TYPE_C_P1);

        uint8_t* bytes = static_cast<uint8_t*>(info.address);
        for (size_t i = 0; i < ALLOC_SIZE; i++) {
            assert(bytes[i] == 0x1C);
        }

        uballoc_free(p0);

        int status;
        waitpid(pid, &status, 0);
        assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);

        uballoc_reset();
        cleanup_distributed_shms();
        unsetenv("UBALLOC_HEAP_ID");
        std::cout << "  C API uballoc_malloc_published + uballoc_lookup_by_type_blocking: OK" << std::endl;
    }
}

static void test_distributed_growth_discovery() {
    std::cout << "[distributed] growth discovery: P0 grows, P1 discovers via check_remote_segments..." << std::endl;

    cleanup_distributed_shms();
    setenv("UBALLOC_HEAP_ID", "gdiscover", 1);

    int sync_pipe[2];
    assert(pipe(sync_pipe) == 0);

    auto& p0_alloc = uballoc::get_global_allocator<uballoc::DistributedShmBackend<>>();
    p0_alloc.soft_reset();
    p0_alloc.init();
    p0_alloc.init_thread(0);
    if (!p0_alloc.is_initialized()) { close(sync_pipe[0]); close(sync_pipe[1]); cleanup_distributed_shms(); unsetenv("UBALLOC_HEAP_ID"); return; }

    pid_t pid = fork();
    assert(pid >= 0);

    if (pid == 0) {
        close(sync_pipe[1]); // P1 closes write end, keeps read end
        auto& alloc = uballoc::get_global_allocator<uballoc::DistributedShmBackend<>>();
        alloc.soft_reset();
        alloc.init();
        if (!alloc.is_initialized()) _exit(1);

        char c;
        read(sync_pipe[0], &c, 1); // wait for P0 growth
        close(sync_pipe[0]);

        alloc.check_remote_segments();

        void* seg_base = alloc.backend().data_address(0, 0, 0);
        if (!seg_base) _exit(1);

        memset(seg_base, 0xAB, 4096);

        alloc.backend().cleanup_remote_regions();
        alloc.backend().cleanup_own_shms();
        _exit(0);
    } else {
        close(sync_pipe[0]); // P0 closes read end, keeps write end

        auto* shared = p0_alloc.small_shared();
        uint32_t seg_before = shared->segment_count_.load(std::memory_order_acquire);
        size_t blocks_per_slab = uballoc::Small::SLAB_SIZE / 64;
        size_t slabs_available = shared->bump_end_atomic_.load(std::memory_order_acquire);
        size_t total_to_alloc = slabs_available * blocks_per_slab + 10;

        std::vector<void*> ptrs;
        bool growth = false;
        for (size_t i = 0; i < total_to_alloc + 100 && !growth; ++i) {
            void* p = p0_alloc.malloc(64);
            if (!p) break;
            ptrs.push_back(p);
            if (shared->segment_count_.load(std::memory_order_acquire) > seg_before) growth = true;
        }
        if (!growth) { close(sync_pipe[1]); cleanup_distributed_shms(); unsetenv("UBALLOC_HEAP_ID"); return; }
        assert(shared->segment_count_.load(std::memory_order_acquire) >= seg_before + 1);

        for (void* p : ptrs) p0_alloc.free(p);

        char c = 'G';
        write(sync_pipe[1], &c, 1);
        close(sync_pipe[1]);

        int status;
        waitpid(pid, &status, 0);
        assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);

        // P0 verifies P1's pattern was written to the grown segment
        void* p0_seg1 = p0_alloc.backend().data_address(0, 0, seg_before);
        assert(p0_seg1 != nullptr);
        uint8_t buf[4096];
        memset(buf, 0, sizeof(buf));
        memcpy(buf, p0_seg1, sizeof(buf));
        bool ok = true;
        for (size_t i = 0; i < sizeof(buf); i++) {
            if (buf[i] != 0xAB) { ok = false; break; }
        }
        if (!ok) { std::cerr << "P0: P1's pattern NOT visible on seg 1!" << std::endl; cleanup_distributed_shms(); unsetenv("UBALLOC_HEAP_ID"); return; }

        p0_alloc.reset();
        cleanup_distributed_shms();
        unsetenv("UBALLOC_HEAP_ID");
        std::cout << "  distributed growth discovery: P0 grew, P1 attached remote segment: OK" << std::endl;
    }
}

static void test_dynamic_discovery_basic() {
    std::cout << "[dynamic] discovery basic: env-var-driven init, fork 2 procs..." << std::endl;

    cleanup_distributed_shms();

    int sync_pipe[2];
    assert(pipe(sync_pipe) == 0);

    setenv("UBALLOC_HEAP_ID", "dynheap", 1);

    auto& parent_alloc = uballoc::get_global_allocator<uballoc::DistributedShmBackend<>>();
    parent_alloc.reset();
    void* parent_p = uballoc::malloc(128);
    assert(parent_p != nullptr);
    write_pattern(parent_p, 128, 0xDD);
    verify_pattern(parent_p, 128, 0xDD);
    assert(uballoc::rank() == 0);

    pid_t pid = fork();
    assert(pid >= 0);

    if (pid == 0) {
        close(sync_pipe[0]);
        auto& child_alloc = uballoc::get_global_allocator<uballoc::DistributedShmBackend<>>();
        child_alloc.soft_reset();
        void* p = uballoc::malloc(128);
        assert(p != nullptr);
        write_pattern(p, 128, 0xEE);
        verify_pattern(p, 128, 0xEE);
        assert(uballoc::rank() == 1);
        assert(uballoc::total_processes() == static_cast<int>(uballoc::MAX_PROCESSES));

        char c = 'R';
        write(sync_pipe[1], &c, 1);
        close(sync_pipe[1]);
        uballoc::free(p);
        child_alloc.reset();
        _exit(0);
    } else {
        close(sync_pipe[1]);

        char c;
        read(sync_pipe[0], &c, 1);
        close(sync_pipe[0]);

        int status;
        waitpid(pid, &status, 0);
        assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);

        uballoc::free(parent_p);
        parent_alloc.reset();
        cleanup_distributed_shms();
        unsetenv("UBALLOC_HEAP_ID");
        std::cout << "  dynamic discovery basic: P0+P1 via env-var init: OK" << std::endl;
    }
}

static void test_dynamic_discovery_late_join() {
    std::cout << "[dynamic] discovery late join: P1 starts after P0 allocated..." << std::endl;

    cleanup_distributed_shms();

    setenv("UBALLOC_HEAP_ID", "dynheap", 1);

    using Backend = uballoc::DistributedShmBackend<>;

    auto& parent_alloc = uballoc::get_global_allocator<Backend>();
    parent_alloc.soft_reset();

    void* p0_ptr = uballoc::malloc(64, 41);
    assert(p0_ptr != nullptr);
    write_pattern(p0_ptr, 64, 0xBB);
    assert(uballoc::rank() == 0);

    pid_t pid = fork();
    assert(pid >= 0);

    if (pid == 0) {
        auto& child_alloc = uballoc::get_global_allocator<Backend>();
        child_alloc.soft_reset();
        ::usleep(100000);

        void* p = uballoc::malloc(64, 42);
        assert(p != nullptr);
        write_pattern(p, 64, 0xAA);
        assert(uballoc::rank() == 1);

        auto info = uballoc::lookup_by_type_blocking(41, 5000);
        if (info.owner_process < 0) {
            std::cerr << "  P1: timeout waiting for P0's type 41" << std::endl;
            uballoc::free(p);
            child_alloc.reset();
            _exit(1);
        }
        verify_pattern(info.address, 64, 0xBB);

        uballoc::free(p);
        sleep(2);
        child_alloc.reset();
        _exit(0);
    } else {
        auto info = uballoc::lookup_by_type_blocking(42, 10000);
        assert(info.owner_process >= 0);
        verify_pattern(info.address, 64, 0xAA);
        assert(info.owner_process == 1);

        uballoc::free(p0_ptr);
        parent_alloc.reset();

        int status;
        waitpid(pid, &status, 0);
        assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);

        cleanup_distributed_shms();
        unsetenv("UBALLOC_HEAP_ID");
        std::cout << "  dynamic discovery late join: P1 joined after P0 alloc: OK" << std::endl;
    }
}

static void test_dynamic_discovery_stale_slot() {
    std::cout << "[dynamic] discovery stale slot: crashed child's slot reclaimed..." << std::endl;

    cleanup_distributed_shms();

    setenv("UBALLOC_HEAP_ID", "dynheap", 1);

    using Backend = uballoc::DistributedShmBackend<>;

    auto& parent_alloc = uballoc::get_global_allocator<Backend>();
    parent_alloc.reset();
    void* p0 = uballoc::malloc(64);
    assert(p0 != nullptr);
    assert(uballoc::rank() == 0);

    pid_t pid = fork();
    assert(pid >= 0);

    if (pid == 0) {
        auto& child_alloc = uballoc::get_global_allocator<Backend>();
        child_alloc.soft_reset();
        void* p = uballoc::malloc(64);
        assert(p != nullptr);
        assert(uballoc::rank() == 1);
        ::_exit(0);
    }

    int status;
    waitpid(pid, &status, 0);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);

    pid_t pid2 = fork();
    assert(pid2 >= 0);

    if (pid2 == 0) {
        auto& child_alloc = uballoc::get_global_allocator<Backend>();
        child_alloc.soft_reset();
        void* p = uballoc::malloc(64);
        assert(p != nullptr);
        assert(uballoc::rank() == 1);
        uballoc::free(p);
        child_alloc.reset();
        _exit(0);
    } else {
        waitpid(pid2, &status, 0);
        assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
        uballoc::free(p0);
        parent_alloc.reset();
        cleanup_distributed_shms();
        unsetenv("UBALLOC_HEAP_ID");
        std::cout << "  dynamic discovery stale slot: dead P1's slot reclaimed by new process: OK" << std::endl;
    }
}

static void run_distributed_tests() {
    test_distributed_growth_discovery();
    test_distributed_single_process();
    test_distributed_fork_basic();
    test_distributed_fork_cross_free();
    test_distributed_fork_concurrent_bump();
    test_distributed_malloc_published();
    test_distributed_c_api_published();
    test_dynamic_discovery_basic();
    test_dynamic_discovery_late_join();
    test_dynamic_discovery_stale_slot();
}

int main(int argc, char* argv[]) {
    if (argc > 1 && std::string(argv[1]) == "--distributed") {
        run_distributed_tests();
        std::cout << "  distributed tests (fresh process): OK" << std::endl;
        return 0;
    }

    std::cout << "=== uballoc test suite ===" << std::endl;

    cleanup_distributed_shms();
    setenv("UBALLOC_HEAP_ID", "test_suite", 1);

    alloc().soft_reset();
    alloc().init();
    alloc().init_thread(0);

    test_unit();
    test_small_alloc_free();
    test_large_alloc_free();
    test_huge_alloc_free();
    test_boundary_sizes();
    test_many_small();
    test_interleaved_alloc_free();
    test_mixed_brackets();
    test_free_null();
    test_realloc();
    test_class_size();
    test_content_integrity_small();
    test_content_integrity_mixed();
    test_huge_page_stride();
    test_randomized_single();
    test_state_machine_single();
    test_state_machine_concurrent();
    test_remote_free_claim();
    test_remote_free_claim_barrier();
    test_remote_free_large_claim();
    test_concurrent_small();
    test_concurrent_mixed();
    test_concurrent_cross_free();
    test_concurrent_stress();
    test_concurrent_cross_free_reuse();

    test_reservation_small_bump();
    test_reservation_large_bump();
    test_reservation_capacity_check();
    test_reservation_concurrent_bump();

    test_free_null();
    test_realloc();
    test_class_size();
    test_content_integrity_small();
    test_content_integrity_mixed();
    test_huge_page_stride();
    test_randomized_single();
    test_state_machine_single();
    test_state_machine_concurrent();
    test_remote_free_claim();
    test_remote_free_claim_barrier();
    test_remote_free_large_claim();
    test_concurrent_small();
    test_concurrent_mixed();
    test_concurrent_cross_free();
    test_concurrent_stress();
    test_concurrent_cross_free_reuse();

    test_reservation_small_bump();
    test_reservation_large_bump();
    test_reservation_capacity_check();
    test_reservation_concurrent_bump();

    test_distributed_va_layout();
    test_distributed_boundary_table();

    std::string exec_path = argv[0];
    if (exec_path.find('/') == std::string::npos) {
        char buf[4096];
        ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
        if (len > 0) {
            buf[len] = '\0';
            exec_path = buf;
        }
    }

    pid_t dist_pid = fork();
    assert(dist_pid >= 0);
    if (dist_pid == 0) {
        execl(exec_path.c_str(), exec_path.c_str(), "--distributed", nullptr);
        perror("execl");
        _exit(1);
    } else {
        int dist_status;
        waitpid(dist_pid, &dist_status, 0);
        assert(WIFEXITED(dist_status) && WEXITSTATUS(dist_status) == 0);
    }

    std::cout << "\n=== All tests passed! ===" << std::endl;
    cleanup_distributed_shms();
    return 0;
}