// SPDX-License-Identifier: Apache-2.0
//
// Test: Huge allocator large-allocation and capacity ceiling.
// Covers two bugs: (1) slots array capped at 1024 (2GB), (2) single-if grow
// couldn't satisfy malloc(>128MB) needing multiple segments.
//
// Pattern: parent inits as P0 before fork; child soft_reset+init as non-P0.

#include "uballoc.hpp"
#include "test_helpers.hpp"
#include <iostream>
#include <cassert>
#include <cstring>
#include <vector>
#include <unistd.h>
#include <sys/wait.h>

using Backend = uballoc::DistributedShmBackend<>;

// Min physical memory (8GB). Peak: T2=2.5GB, T3=2GB every-page write.
static constexpr size_t MIN_PHYSICAL_MEMORY_BYTES = 8ULL * 1024 * 1024 * 1024;

// Returns available memory.
static size_t available_memory_bytes() {
    FILE* f = fopen("/proc/meminfo", "r");
    if (!f) return 0;
    size_t mem_available_kb = 0;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "MemAvailable: %zu kB", &mem_available_kb) == 1) break;
    }
    fclose(f);
    return mem_available_kb * 1024;
}

// Test-scenario sizes. 
static constexpr size_t ONE_GB          = 1ULL * 1024 * 1024 * 1024;
static constexpr size_t TWO_GB          = 2ULL * 1024 * 1024 * 1024;
static constexpr size_t FIVE_HUNDRED_MB = 512ULL * 1024 * 1024;
static constexpr size_t ONE_SEG_CAP     = uballoc::SEGMENT_VA_SIZE;   // 128MB
static constexpr size_t TWO_MB          = uballoc::HUGE_SLAB_SIZE;    // 2MB

static void fill_pattern(void* p, size_t size, uint8_t pattern) {
    char* cp = static_cast<char*>(p);
    for (size_t off = 0; off < size; off += uballoc::SIZE_PAGE) cp[off] = pattern;
    if (size > 0) cp[size - 1] = pattern;
}

static bool verify_pattern(void* p, size_t size, uint8_t pattern) {
    char* cp = static_cast<char*>(p);
    for (size_t off = 0; off < size; off += uballoc::SIZE_PAGE) {
        if (static_cast<uint8_t>(cp[off]) != pattern) return false;
    }
    if (size > 0 && static_cast<uint8_t>(cp[size - 1]) != pattern) return false;
    return true;
}

static bool child_init() {
    auto& alloc = uballoc::get_global_allocator<Backend>();
    alloc.soft_reset();
    alloc.init();
    if (!alloc.is_initialized()) { std::cerr << "  init failed" << std::endl; return false; }
    return true;
}

// assert() is no-op under NDEBUG; null ptr would crash on deref. Use explicit check.
static void* check_alloc(void* p, const char* what, size_t size) {
    if (!p) {
        std::cerr << "  FAIL: " << what << " (size=" << (size >> 20) << "MB) returned null"
                  << std::endl;
        _exit(1);
    }
    return p;
}

// Local cleanup: test_helpers.hpp's cleanup_all_distributed_shms(1, id) only
// covers p0 and seg<8, but fork-children run as rank>=1 and huge segs grow
// past s7 (observed s27). Covers all ranks + seg<32. shm_unlink on non-existent
// names is a no-op, so over-scanning is safe.
static void cleanup_grow2g_shms(const std::string& heap_id) {
    using ShmProvider = Backend::ShmProvider;
    ShmProvider::provider_init();
    ShmProvider::shm_unlink(ShmProvider::bootstrap_shm_name(heap_id));
    constexpr int SEG_CAP = 32;
    for (int pid = 0; pid < static_cast<int>(uballoc::MAX_PROCESSES); ++pid) {
        for (int r = 0; r < 4; ++r) {
            for (int seg = 0; seg < SEG_CAP; ++seg) {
                ShmProvider::shm_unlink(
                    ShmProvider::shm_name(heap_id, pid, r, seg));
            }
        }
    }
}

// T0: White-box unit test for slot value encoding (no fork, no init, no memory).
// Verifies P2-2 fix: 24-bit slot_count field round-trips large values (the old
// 16-bit field truncated slot_count>=65536 to 0, leaking allocations >=128GB).
static void test_slot_encoding() {
    std::cout << "[T0] Slot value encoding..." << std::endl;

    // used flag
    uint64_t v = uballoc::Huge::make_slot_value(uballoc::ThreadId(0), 1);
    assert((v & 1) == 1);

    // 65535 = 128GB - 2MB (old 16-bit field max, now well within range)
    uint64_t v1 = uballoc::Huge::make_slot_value(uballoc::ThreadId(0), 65535);
    assert((v1 & 1) == 1);
    assert(uballoc::Huge::get_slot_count(v1) == 65535);
    // tid via raw bits (get_owner has a separate +1 asymmetry bug, out of scope)
    assert(((v1 >> 32) & 0xFFFF) == uballoc::ThreadId(0).internal());
    std::cout << "  slot_count=65535 -> get_slot_count="
              << uballoc::Huge::get_slot_count(v1) << " (OK)" << std::endl;

    // 65536 = 128GB (first value the old 16-bit field truncated to 0)
    uint64_t v2 = uballoc::Huge::make_slot_value(uballoc::ThreadId(0), 65536);
    assert((v2 & 1) == 1);
    size_t sc2 = uballoc::Huge::get_slot_count(v2);
    std::cout << "  slot_count=65536 -> get_slot_count=" << sc2 << " (OK)" << std::endl;
    assert(sc2 == 65536);

    // 131072 = 256GB
    uint64_t v3 = uballoc::Huge::make_slot_value(uballoc::ThreadId(0), 131072);
    size_t sc3 = uballoc::Huge::get_slot_count(v3);
    std::cout << "  slot_count=131072 -> get_slot_count=" << sc3 << " (OK)" << std::endl;
    assert(sc3 == 131072);

    // 24-bit field max: 16777215 = (1<<24)-1 = 32TB at 2MB/slot
    uint64_t v5 = uballoc::Huge::make_slot_value(uballoc::ThreadId(0), 16777215);
    size_t sc5 = uballoc::Huge::get_slot_count(v5);
    std::cout << "  slot_count=16777215 -> get_slot_count=" << sc5
              << " (24-bit max, OK)" << std::endl;
    assert(sc5 == 16777215);

    // tid preserved at large slot_count (tid in bit32-47, slot_count in bit1-24)
    uint64_t v4 = uballoc::Huge::make_slot_value(uballoc::ThreadId(42), 65536);
    assert(((v4 >> 32) & 0xFFFF) == uballoc::ThreadId(42).internal());

    std::cout << "  Slot encoding test passed (24-bit field, P2-2 fixed)" << std::endl;
}

// T1: Single large malloc crossing multiple segments (Bug 2).
//   512MB=4 segs, 1GB=8 segs, 2GB=16 segs.
static void test_single_large_malloc() {
    std::cout << "[T1] Single large malloc crossing multiple segments..." << std::endl;

    pid_t pid = fork();
    assert(pid >= 0);

    if (pid == 0) {
        if (!child_init()) _exit(1);

        struct Case { const char* name; size_t size; };
        Case cases[] = {
            {"512MB", FIVE_HUNDRED_MB},
            {"1GB",   ONE_GB},
            {"2GB",   TWO_GB},
        };

        for (auto& c : cases) {
            void* p = check_alloc(uballoc::get_global_allocator<Backend>().malloc(c.size),
                                  c.name, c.size);
            fill_pattern(p, c.size, 0xAB);
            assert(verify_pattern(p, c.size, 0xAB));
            uballoc::get_global_allocator<Backend>().free(p);
            size_t expect_slots = (c.size + uballoc::HUGE_SLAB_SIZE - 1) / uballoc::HUGE_SLAB_SIZE;
            std::cout << "  " << c.name << " OK (" << expect_slots
                      << " slots)" << std::endl;
        }
        _exit(0);
    } else {
        int status;
        waitpid(pid, &status, 0);
        check_child(status);
        std::cout << "  Verified: single large malloc works across segments" << std::endl;
    }
}

// T2: Cumulative alloc exceeding old 2GB ceiling (Bug 1). 128MB x 20 = 2.5GB.
static void test_cumulative_exceeds_2gb() {
    std::cout << "[T2] Cumulative alloc exceeding 2GB ceiling..." << std::endl;

    pid_t pid = fork();
    assert(pid >= 0);

    if (pid == 0) {
        if (!child_init()) _exit(1);

        auto& alloc = uballoc::get_global_allocator<Backend>();
        const int N = 20;
        const size_t sz = ONE_SEG_CAP;  // 128MB
        std::vector<void*> ptrs;
        std::vector<uint8_t> patterns;

        for (int i = 0; i < N; ++i) {
            void* p = check_alloc(alloc.malloc(sz), "cumulative malloc", sz);
            uint8_t pat = static_cast<uint8_t>(0xA0 | (i & 0x0F));
            fill_pattern(p, sz, pat);
            ptrs.push_back(p);
            patterns.push_back(pat);
        }
        assert(ptrs.size() == N);

        // Verify all data intact (catches slot corruption/overlap)
        bool ok = true;
        for (int i = 0; i < N; ++i) {
            if (!verify_pattern(ptrs[i], sz, patterns[i])) {
                std::cerr << "  VERIFY FAIL at i=" << i << std::endl;
                ok = false;
                break;
            }
        }
        assert(ok);

        for (void* p : ptrs) alloc.free(p);
        std::cout << "  128MB x 20 = 2.5GB OK" << std::endl;
        _exit(0);
    } else {
        int status;
        waitpid(pid, &status, 0);
        check_child(status);
        std::cout << "  Verified: cumulative > 2GB works" << std::endl;
    }
}

// T3: Data integrity — verify every page of 2GB block (catches stale shm/truncation).
static void test_data_integrity_2gb() {
    std::cout << "[T3] Data integrity: every page of 2GB block..." << std::endl;

    pid_t pid = fork();
    assert(pid >= 0);

    if (pid == 0) {
        if (!child_init()) _exit(1);

        auto& alloc = uballoc::get_global_allocator<Backend>();
        void* p = check_alloc(alloc.malloc(TWO_GB), "2GB malloc", TWO_GB);

        // Position-dependent pattern: each page gets its low byte
        char* cp = static_cast<char*>(p);
        for (size_t off = 0; off < TWO_GB; off += uballoc::SIZE_PAGE) {
            cp[off] = static_cast<char>((off / uballoc::SIZE_PAGE) & 0xFF);
        }

        bool ok = true;
        for (size_t off = 0; off < TWO_GB; off += uballoc::SIZE_PAGE) {
            uint8_t expected = static_cast<uint8_t>((off / uballoc::SIZE_PAGE) & 0xFF);
            if (static_cast<uint8_t>(cp[off]) != expected) {
                std::cerr << "  MISMATCH at off=0x" << std::hex << off
                          << " expected=" << (int)expected
                          << " got=" << (int)cp[off] << std::dec << std::endl;
                ok = false;
                break;
            }
        }
        assert(ok);

        alloc.free(p);
        std::cout << "  2GB every-page verify OK" << std::endl;
        _exit(0);
    } else {
        int status;
        waitpid(pid, &status, 0);
        check_child(status);
        std::cout << "  Verified: data integrity across 16 segments" << std::endl;
    }
}

// T4: Free-then-reuse — 2GB alloc, free, alloc again (slot reclaim).
static void test_free_then_reuse() {
    std::cout << "[T4] Free-then-reuse: 2GB alloc, free, alloc again..." << std::endl;

    pid_t pid = fork();
    assert(pid >= 0);

    if (pid == 0) {
        if (!child_init()) _exit(1);

        auto& alloc = uballoc::get_global_allocator<Backend>();

        void* p1 = check_alloc(alloc.malloc(TWO_GB), "first 2GB alloc", TWO_GB);
        fill_pattern(p1, TWO_GB, 0x11);
        alloc.free(p1);

        // Reuse: scan_and_claim finds the freed 1024-slot run (no grow needed).
        void* p2 = check_alloc(alloc.malloc(TWO_GB), "reuse 2GB alloc", TWO_GB);
        fill_pattern(p2, TWO_GB, 0x22);
        assert(verify_pattern(p2, TWO_GB, 0x22));
        alloc.free(p2);

        void* p3 = check_alloc(alloc.malloc(TWO_GB), "third 2GB alloc", TWO_GB);
        alloc.free(p3);

        std::cout << "  2GB alloc/free/reuse x3 OK" << std::endl;
        _exit(0);
    } else {
        int status;
        waitpid(pid, &status, 0);
        check_child(status);
        std::cout << "  Verified: free-then-reuse works" << std::endl;
    }
}

// T5: Boundary sizes — 128MB (1 seg), 130MB (65 slots, minimal Bug 2 trigger), 256MB (2 segs).
static void test_boundary_sizes() {
    std::cout << "[T5] Boundary sizes: 128MB, 130MB, 256MB..." << std::endl;

    pid_t pid = fork();
    assert(pid >= 0);

    if (pid == 0) {
        if (!child_init()) _exit(1);

        auto& alloc = uballoc::get_global_allocator<Backend>();

        // 128MB = 64 slots = 1 segment
        {
            void* p = check_alloc(alloc.malloc(ONE_SEG_CAP), "128MB (1 seg)", ONE_SEG_CAP);
            fill_pattern(p, ONE_SEG_CAP, 0x01);
            assert(verify_pattern(p, ONE_SEG_CAP, 0x01));
            alloc.free(p);
            std::cout << "  128MB (1 seg) OK" << std::endl;
        }

        // 130MB = 65 slots = 2 segments (minimal Bug 2 trigger: 1 grow < 65)
        {
            size_t sz = 130ULL * 1024 * 1024;
            void* p = check_alloc(alloc.malloc(sz), "130MB (65 slots, 2 segs)", sz);
            fill_pattern(p, sz, 0x02);
            assert(verify_pattern(p, sz, 0x02));
            alloc.free(p);
            std::cout << "  130MB (65 slots, 2 segs) OK" << std::endl;
        }

        // 256MB = 128 slots = 2 segments
        {
            size_t sz = 256ULL * 1024 * 1024;
            void* p = check_alloc(alloc.malloc(sz), "256MB (2 segs)", sz);
            fill_pattern(p, sz, 0x03);
            assert(verify_pattern(p, sz, 0x03));
            alloc.free(p);
            std::cout << "  256MB (2 segs) OK" << std::endl;
        }
        _exit(0);
    } else {
        int status;
        waitpid(pid, &status, 0);
        check_child(status);
        std::cout << "  Verified: boundary sizes work" << std::endl;
    }
}

// T6: Mixed sizes — interleaved small/large/huge (bracket isolation).
static void test_mixed_sizes() {
    std::cout << "[T6] Mixed sizes: small + huge interleaved..." << std::endl;

    pid_t pid = fork();
    assert(pid >= 0);

    if (pid == 0) {
        if (!child_init()) _exit(1);

        auto& alloc = uballoc::get_global_allocator<Backend>();
        std::vector<void*> ptrs;
        std::vector<size_t> sizes;
        std::vector<uint8_t> patterns;

        // Interleave small/large/huge
        struct Item { size_t size; uint8_t pat; };
        Item items[] = {
            {64, 0x10},
            {256 * 1024, 0x20},
            {TWO_MB, 0x30},
            {ONE_SEG_CAP, 0x40},      // 128MB
            {64, 0x11},
            {FIVE_HUNDRED_MB, 0x50},  // 512MB
            {256 * 1024, 0x21},
            {TWO_MB, 0x31},
        };

        for (auto& it : items) {
            void* p = check_alloc(alloc.malloc(it.size), "mixed malloc", it.size);
            fill_pattern(p, it.size, it.pat);
            ptrs.push_back(p);
            sizes.push_back(it.size);
            patterns.push_back(it.pat);
        }

        // Verify all
        bool ok = true;
        for (size_t i = 0; i < ptrs.size(); ++i) {
            if (!verify_pattern(ptrs[i], sizes[i], patterns[i])) {
                std::cerr << "  VERIFY FAIL at i=" << i << std::endl;
                ok = false;
                break;
            }
        }
        assert(ok);

        for (void* p : ptrs) alloc.free(p);
        std::cout << "  Mixed sizes OK" << std::endl;
        _exit(0);
    } else {
        int status;
        waitpid(pid, &status, 0);
        check_child(status);
        std::cout << "  Verified: mixed sizes work" << std::endl;
    }
}

// T7: Realloc huge — 128MB -> 512MB (copy old data).
static void test_realloc_huge() {
    std::cout << "[T7] Realloc huge: 128MB -> 512MB..." << std::endl;

    pid_t pid = fork();
    assert(pid >= 0);

    if (pid == 0) {
        if (!child_init()) _exit(1);

        auto& alloc = uballoc::get_global_allocator<Backend>();

        void* p1 = check_alloc(alloc.malloc(ONE_SEG_CAP), "initial 128MB alloc", ONE_SEG_CAP);
        fill_pattern(p1, ONE_SEG_CAP, 0xCC);

        // realloc to 512MB — must copy old data
        void* p2 = check_alloc(alloc.realloc(p1, FIVE_HUNDRED_MB), "realloc to 512MB", FIVE_HUNDRED_MB);

        // Verify old data (first 128MB)
        char* cp = static_cast<char*>(p2);
        bool ok = true;
        for (size_t off = 0; off < ONE_SEG_CAP; off += uballoc::SIZE_PAGE) {
            if (static_cast<uint8_t>(cp[off]) != 0xCC) {
                ok = false;
                break;
            }
        }
        assert(ok && "realloc did not preserve old data");

        alloc.free(p2);
        std::cout << "  Realloc 128MB -> 512MB OK" << std::endl;
        _exit(0);
    } else {
        int status;
        waitpid(pid, &status, 0);
        check_child(status);
        std::cout << "  Verified: realloc huge works" << std::endl;
    }
}

int main() {
    test_slot_encoding();

    // Skip on small CI environments (< 8GB RAM).
    size_t avail = available_memory_bytes();
    if (avail < MIN_PHYSICAL_MEMORY_BYTES) {
        std::cout << "[SKIP] test_grow_2gb: available memory "
                  << (avail >> 30) << "GB < required "
                  << (MIN_PHYSICAL_MEMORY_BYTES >> 30) << "GB" << std::endl;
        return 0;
    }

    std::string id = ar_heap_id("grow2g");
    cleanup_grow2g_shms(id);
    setenv("UBALLOC_HEAP_ID", id.c_str(), 1);

    auto& parent_alloc = uballoc::get_global_allocator<Backend>();
    parent_alloc.soft_reset();
    parent_alloc.init();
    parent_alloc.init_thread(0);
    if (!parent_alloc.is_initialized()) {
        std::cerr << "Parent init failed" << std::endl;
        return 1;
    }

    test_single_large_malloc();        // T1
    test_cumulative_exceeds_2gb();     // T2
    test_data_integrity_2gb();         // T3
    test_free_then_reuse();            // T4
    test_boundary_sizes();             // T5
    test_mixed_sizes();                // T6
    test_realloc_huge();               // T7

    parent_alloc.reset();
    cleanup_grow2g_shms(id);
    std::cout << "\n=== grow_2gb tests passed ===" << std::endl;
    return 0;
}
