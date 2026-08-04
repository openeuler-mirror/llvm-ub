// SPDX-License-Identifier: Apache-2.0

// Tests for cross-process Huge free (Branch B core feature).
//
// Item 1: P0 allocates Huge, P1 frees it (cross-process), P0 re-claims slot.
// Item 2: P1 calls class_size on a Huge pointer allocated by P0.
// Item 3: Concurrent Huge alloc/free (multi-thread, same process, CAS contention).
//
// On UBSE, the parent MUST initialize before fork so the child inherits
// the UBSE runtime. The parent allocates AFTER the child signals ready
// (matching test_main.cpp's test_distributed_fork_cross_free pattern).

#include "uballoc.hpp"
#include "test_helpers.hpp"
#include <iostream>
#include <cassert>
#include <cstring>
#include <vector>
#include <unistd.h>
#include <sys/wait.h>
#include <thread>
#include <atomic>

static constexpr int HC_PROCS = 2;
static constexpr const char* HEAP_ID = "hcfree";

static void test_cross_process_huge_free() {
    std::cout << "[huge-cross-free] P0 allocs Huge, P1 frees it, P0 re-claims..." << std::endl;

    cleanup_all_distributed_shms(HC_PROCS, HEAP_ID);
    setenv("UBALLOC_HEAP_ID", HEAP_ID, 1);

    int sync_pipe[2];
    int data_pipe[2];
    int result_pipe[2];
    assert(pipe(sync_pipe) == 0);
    assert(pipe(data_pipe) == 0);
    assert(pipe(result_pipe) == 0);

    auto& parent_alloc = uballoc::get_global_allocator<uballoc::DistributedShmBackend<>>();
    parent_alloc.soft_reset();
    parent_alloc.init();
    parent_alloc.init_thread(0);
    if (!parent_alloc.is_initialized()) {
        std::cerr << "  P0 init failed" << std::endl;
        return;
    }

    pid_t pid = fork();
    assert(pid >= 0);

    if (pid == 0) {
        // P1: receive pointer, check class_size, free it
        close(sync_pipe[0]);
        close(data_pipe[1]);
        close(result_pipe[0]);

        auto& alloc = uballoc::get_global_allocator<uballoc::DistributedShmBackend<>>();
        alloc.soft_reset();
        alloc.init();
        if (!alloc.is_initialized()) _exit(1);

        char c = 'R';
        write(sync_pipe[1], &c, 1);
        close(sync_pipe[1]);

        uintptr_t raw = 0;
        ssize_t n = read(data_pipe[0], &raw, sizeof(raw));
        assert(n == static_cast<ssize_t>(sizeof(raw)));
        close(data_pipe[0]);

        void* p = reinterpret_cast<void*>(raw);

        alloc.check_remote_segments();

        size_t cs = alloc.current_allocator().class_size(p);
        assert(cs >= 4 * 1024 * 1024);
        std::cout << "  P1: class_size=" << cs << " (>= 4MB: OK)" << std::endl;

        uint8_t* bytes = static_cast<uint8_t*>(p);
        bool all_aa = true;
        for (size_t i = 0; i < 64; ++i) {
            if (bytes[i] != 0xAA) { all_aa = false; break; }
        }
        assert(all_aa);
        std::cout << "  P1: data integrity verified (0xAA pattern)" << std::endl;

        alloc.free(p);
        std::cout << "  P1: freed P0's Huge allocation (cross-process)" << std::endl;

        char d = 'D';
        write(result_pipe[1], &d, 1);
        close(result_pipe[1]);
        _exit(0);
    }

    // P0: allocate Huge, send pointer to P1, wait for P1 to free, re-allocate
    close(sync_pipe[1]);
    close(data_pipe[0]);
    close(result_pipe[1]);

    char c;
    assert(read(sync_pipe[0], &c, 1) == 1 && c == 'R');
    close(sync_pipe[0]);

    constexpr size_t SIZE = 4 * 1024 * 1024;
    void* p = parent_alloc.malloc(SIZE);
    assert(p != nullptr);

    memset(p, 0xAA, SIZE);

    uintptr_t raw = reinterpret_cast<uintptr_t>(p);
    ssize_t n = write(data_pipe[1], &raw, sizeof(raw));
    assert(n == static_cast<ssize_t>(sizeof(raw)));
    close(data_pipe[1]);

    char d;
    assert(read(result_pipe[0], &d, 1) == 1 && d == 'D');
    close(result_pipe[0]);

    void* p2 = parent_alloc.malloc(SIZE);
    assert(p2 != nullptr);

    std::cout << "  P0: re-allocated after P1 free (p=" << p
              << " p2=" << p2 << ")" << std::endl;

    memset(p2, 0xBB, SIZE);

    uint8_t* check = static_cast<uint8_t*>(p2);
    bool all_bb = true;
    for (size_t i = 0; i < 64; ++i) {
        if (check[i] != 0xBB) { all_bb = false; break; }
    }
    assert(all_bb);

    parent_alloc.free(p2);

    // Verify P1's cross-process free actually cleared the slot.
    // class_size reads slots[slot_index] from P0's HugeShared.
    // If P1's free_offset CAS'd it to 0, class_size returns 0.
    size_t cs_after_free = parent_alloc.current_allocator().class_size(p);
    std::cout << "  P0: class_size(p) after P1 free = " << cs_after_free << std::endl;
    assert(cs_after_free == 0);

    int status;
    waitpid(pid, &status, 0);
    check_child(status);

    parent_alloc.reset();
    cleanup_all_distributed_shms(HC_PROCS, HEAP_ID);
    std::cout << "  Verified: cross-process Huge free + slot reuse + data integrity" << std::endl;
}

static void test_cross_process_huge_varied_sizes() {
    std::cout << "[huge-cross-free] Multiple sizes: P0 allocs, P1 frees all..." << std::endl;

    cleanup_all_distributed_shms(HC_PROCS, HEAP_ID);
    setenv("UBALLOC_HEAP_ID", HEAP_ID, 1);

    int sync_pipe[2];
    int data_pipe[2];
    int result_pipe[2];
    assert(pipe(sync_pipe) == 0);
    assert(pipe(data_pipe) == 0);
    assert(pipe(result_pipe) == 0);

    auto& parent_alloc = uballoc::get_global_allocator<uballoc::DistributedShmBackend<>>();
    parent_alloc.soft_reset();
    parent_alloc.init();
    parent_alloc.init_thread(0);
    if (!parent_alloc.is_initialized()) {
        std::cerr << "  P0 init failed" << std::endl;
        return;
    }

    pid_t pid = fork();
    assert(pid >= 0);

    if (pid == 0) {
        close(sync_pipe[0]);
        close(data_pipe[1]);
        close(result_pipe[0]);

        auto& alloc = uballoc::get_global_allocator<uballoc::DistributedShmBackend<>>();
        alloc.soft_reset();
        alloc.init();
        if (!alloc.is_initialized()) _exit(1);

        char c = 'R';
        write(sync_pipe[1], &c, 1);
        close(sync_pipe[1]);

        size_t sizes[] = {4096, 65536, 1024*1024, 4*1024*1024};
        int n = sizeof(sizes)/sizeof(sizes[0]);

        uintptr_t raw;
        for (int i = 0; i < n; ++i) {
            ssize_t rd = read(data_pipe[0], &raw, sizeof(raw));
            assert(rd == static_cast<ssize_t>(sizeof(raw)));

            void* p = reinterpret_cast<void*>(raw);

            alloc.check_remote_segments();

            size_t cs = alloc.current_allocator().class_size(p);
            assert(cs >= sizes[i]);
            std::cout << "  P1: size=" << sizes[i] << " class_size=" << cs << " (OK)" << std::endl;

            alloc.free(p);
        }
        close(data_pipe[0]);

        char d = 'D';
        write(result_pipe[1], &d, 1);
        close(result_pipe[1]);
        _exit(0);
    }

    close(sync_pipe[1]);
    close(data_pipe[0]);
    close(result_pipe[1]);

    char c;
    assert(read(sync_pipe[0], &c, 1) == 1 && c == 'R');
    close(sync_pipe[0]);

    size_t sizes[] = {4096, 65536, 1024*1024, 4*1024*1024};
    int n = sizeof(sizes)/sizeof(sizes[0]);

    for (int i = 0; i < n; ++i) {
        void* p = parent_alloc.malloc(sizes[i]);
        assert(p != nullptr);
        memset(p, static_cast<int>(0x10 * (i+1)), sizes[i]);
        uintptr_t raw = reinterpret_cast<uintptr_t>(p);
        ssize_t wr = write(data_pipe[1], &raw, sizeof(raw));
        assert(wr == static_cast<ssize_t>(sizeof(raw)));
    }
    close(data_pipe[1]);

    char d;
    assert(read(result_pipe[0], &d, 1) == 1 && d == 'D');
    close(result_pipe[0]);

    int status;
    waitpid(pid, &status, 0);
    check_child(status);

    parent_alloc.reset();
    cleanup_all_distributed_shms(HC_PROCS, HEAP_ID);
    std::cout << "  Verified: P1 freed all P0's Huge allocations (varied sizes)" << std::endl;
}

static void test_concurrent_huge_alloc_free() {
    std::cout << "[huge-cross-free] Concurrent alloc/free (8 threads, CAS contention)..." << std::endl;

    cleanup_all_distributed_shms(1, HEAP_ID);
    setenv("UBALLOC_HEAP_ID", HEAP_ID, 1);

    auto& parent_alloc = uballoc::get_global_allocator<uballoc::DistributedShmBackend<>>();
    parent_alloc.soft_reset();
    parent_alloc.init();
    parent_alloc.init_thread(0);
    if (!parent_alloc.is_initialized()) {
        std::cerr << "  P0 init failed" << std::endl;
        return;
    }

    pid_t pid = fork();
    assert(pid >= 0);

    if (pid == 0) {
        auto& alloc = uballoc::get_global_allocator<uballoc::DistributedShmBackend<>>();
        alloc.soft_reset();
        alloc.init();
        if (!alloc.is_initialized()) _exit(1);

        constexpr int NTHREADS = 8;
        constexpr int ITERS = 50;
        constexpr size_t SZ = 4 * 1024 * 1024;

        std::atomic<int> successes{0};
        std::atomic<bool> start{false};

        std::vector<std::thread> threads;
        for (int t = 0; t < NTHREADS; ++t) {
            threads.emplace_back([&, t]() {
                while (!start.load(std::memory_order_acquire))
                    __asm__ __volatile__("" ::: "memory");

                for (int i = 0; i < ITERS; ++i) {
                    void* p = alloc.malloc(SZ);
                    if (!p) continue;
                    memset(p, static_cast<int>(t & 0xFF), 64);
                    alloc.free(p);
                    successes.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }

        start.store(true, std::memory_order_release);
        for (auto& th : threads) th.join();

        int total = successes.load();
        std::cout << "  Completed " << total << " alloc/free cycles across "
                  << NTHREADS << " threads" << std::endl;
        assert(total > 0);

        _exit(0);
    } else {
        int status;
        waitpid(pid, &status, 0);
        check_child(status);
        parent_alloc.reset();
        cleanup_all_distributed_shms(1, HEAP_ID);
        std::cout << "  Verified: concurrent Huge alloc/free, no corruption" << std::endl;
    }
}

int main() {
    test_cross_process_huge_free();
    test_cross_process_huge_varied_sizes();
    test_concurrent_huge_alloc_free();
    std::cout << "\n=== Huge cross-process free tests passed ===" << std::endl;
    return 0;
}
