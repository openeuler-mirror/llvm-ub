// SPDX-License-Identifier: Apache-2.0

/**
 * @file test_ar06_realloc.cpp
 * @brief AR-06 DT: realloc
 *
 * Cases (all single-process):
 *   DT06-1 R3: grow (new_size > class_size) -> new pointer + data kept
 *   DT06-2 R2: shrink -> same pointer, zero copy, data kept
 *   DT06-3 R4: cross-class grow (64 -> 100) moves; same-class (72 -> 80) in place
 *   DT06-4 R1: realloc(nullptr, n) == malloc(n)
 *   DT06-5 R6: realloc(p, 0) returns p, memory still occupied
 *   DT06-6 R5: huge class_size rounding (5MB -> 8MB), shrink in place
 *   DT06-7 R7: 64 <-> 4096 alternating loop, no corruption
 *
 */

#include "uballoc.hpp"
#include "test_helpers.hpp"

#include <cstdint>
#include <cstring>
#include <iostream>

#include <sys/wait.h>

namespace {

constexpr size_t M = 1024 * 1024;

// Fork wrapper: run a single-process case in an isolated child.
#define AR_FORK_CASE()                        \
    pid_t _pid = fork();                      \
    assert(_pid >= 0);                        \
    if (_pid == 0)

#define AR_FORK_END(heap_id)                  \
    int _st;                                  \
    waitpid(_pid, &_st, 0);                   \
    check_child(_st);                         \
    (void)(heap_id)

// -----------------------------------------------------------------------------
// DT06-1 R3
// -----------------------------------------------------------------------------
void dt06_1_grow_moves(const std::string& heap_id) {
    std::cout << "[DT06-1] grow beyond class_size -> new pointer, data kept..." << std::endl;
    AR_FORK_CASE() {
        ar_set_heap_id(heap_id);
        ar_child_init();

        void* p = uballoc::malloc(64);
        assert(p);
        std::memset(p, 0xAA, 64);

        void* q = uballoc::realloc(p, 256);
        assert(q != nullptr && q != p);
        auto* bytes = static_cast<const uint8_t*>(q);
        for (int i = 0; i < 64; ++i) assert(bytes[i] == 0xAA);

        uballoc::free(q);
        std::cout << "  OK: grown into a new block" << std::endl;
        _exit(0);
    }
    AR_FORK_END(heap_id);
}

// -----------------------------------------------------------------------------
// DT06-2 R2
// -----------------------------------------------------------------------------
void dt06_2_shrink_in_place(const std::string& heap_id) {
    std::cout << "[DT06-2] shrink -> same pointer, data kept..." << std::endl;
    AR_FORK_CASE() {
        ar_set_heap_id(heap_id);
        ar_child_init();

        void* p = uballoc::malloc(256);
        assert(p);
        std::memset(p, 0xBB, 256);

        void* q = uballoc::realloc(p, 64);
        assert(q == p);
        auto* bytes = static_cast<const uint8_t*>(q);
        for (int i = 0; i < 64; ++i) assert(bytes[i] == 0xBB);

        uballoc::free(q);  
        std::cout << "  OK: shrink in place" << std::endl;
        _exit(0);
    }
    AR_FORK_END(heap_id);
}

// -----------------------------------------------------------------------------
// DT06-3 R3/R4: growth granularity is class_size(old) — 64B is the 64
// class, so 64->100 moves; same-class in-place is 72->80.
// -----------------------------------------------------------------------------
void dt06_3_same_class_grow(const std::string& heap_id) {
    std::cout << "[DT06-3] 64->100 moves (cross class); 72->80 stays in place..." << std::endl;
    AR_FORK_CASE() {
        ar_set_heap_id(heap_id);
        ar_child_init();

        // Cross-class growth: 64 -> 100 moves (100 exceeds the 64 class).
        void* p = uballoc::malloc(64);
        assert(p);
        std::memset(p, 0xCC, 64);

        void* q = uballoc::realloc(p, 100);
        assert(q != p);  // moved: new_size exceeds class_size(old)
        auto* bytes = static_cast<const uint8_t*>(q);
        for (int i = 0; i < 64; ++i) assert(bytes[i] == 0xCC);  
        std::memset(q, 0xCE, 100);
        for (int i = 0; i < 100; ++i) assert(static_cast<uint8_t*>(q)[i] == 0xCE);

        // Same-class growth: malloc(72) lands in the 80 class, so realloc to
        // any size <= 80 is in place with zero copy.
        void* r = uballoc::malloc(72);
        assert(r);
        std::memset(r, 0xDD, 72);
        void* s = uballoc::realloc(r, 80);
        assert(s == r);  // class_size(old)=80 >= 80 -> in place
        bytes = static_cast<const uint8_t*>(s);
        for (int i = 0; i < 72; ++i) assert(bytes[i] == 0xDD);

        uballoc::free(q);
        uballoc::free(s);
        std::cout << "  OK: cross-class grows move; same-class (72->80) stays in place" << std::endl;
        _exit(0);
    }
    AR_FORK_END(heap_id);
}

// -----------------------------------------------------------------------------
// DT06-4 R1: realloc(nullptr, n) == malloc(n)
// -----------------------------------------------------------------------------
void dt06_4_nullptr_malloc(const std::string& heap_id) {
    std::cout << "[DT06-4] realloc(nullptr, 128) == malloc(128)..." << std::endl;
    AR_FORK_CASE() {
        ar_set_heap_id(heap_id);
        ar_child_init();

        void* p = uballoc::realloc(nullptr, 128);
        assert(p != nullptr);
        std::memset(p, 0xDD, 128);
        auto* bytes = static_cast<const uint8_t*>(p);
        for (int i = 0; i < 128; ++i) assert(bytes[i] == 0xDD);
        uballoc::free(p);
        std::cout << "  OK" << std::endl;
        _exit(0);
    }
    AR_FORK_END(heap_id);
}

// -----------------------------------------------------------------------------
// DT06-5 R6 — deviation fix: NOT equivalent to free
// -----------------------------------------------------------------------------
void dt06_5_zero_size(const std::string& heap_id) {
    std::cout << "[DT06-5] realloc(p, 0) returns p, memory still occupied..." << std::endl;
    AR_FORK_CASE() {
        ar_set_heap_id(heap_id);
        ar_child_init();

        void* p = uballoc::malloc(64);
        assert(p);
        std::memset(p, 0x11, 64);

        void* q = uballoc::realloc(p, 0);
        assert(q == p);  // current implementation: old_size >= 0 is always true

        // The block is still owned and writable.
        auto* bytes = static_cast<uint8_t*>(q);
        bytes[0] = 0x12;
        assert(bytes[0] == 0x12);

        // Exactly one free succeeds afterwards; no double free.
        uballoc::free(q);

        // Heap must be healthy afterwards.
        void* t = uballoc::malloc(64);
        assert(t != nullptr);
        uballoc::free(t);
        std::cout << "  OK: deviation fixed (returns original pointer)" << std::endl;
        _exit(0);
    }
    AR_FORK_END(heap_id);
}

// -----------------------------------------------------------------------------
// DT06-6 R5: huge class_size rounds up to 4MB slots
// -----------------------------------------------------------------------------
void dt06_6_huge_realloc(const std::string& heap_id) {
    std::cout << "[DT06-6] 5MB huge: 6MB in place; 9MB moves..." << std::endl;
    AR_FORK_CASE() {
        ar_set_heap_id(heap_id);
        ar_child_init();

        constexpr size_t kAlloc = 5 * M;
        void* p = uballoc::malloc(kAlloc);
        assert(p);

        auto* bytes = static_cast<uint8_t*>(p);
        for (size_t i = 0; i < kAlloc; ++i) bytes[i] = static_cast<uint8_t>(i * 131073ULL);

        // 6MB <= class_size(8MB) -> shrink semantics, in place.
        void* q = uballoc::realloc(p, 6 * M);
        assert(q == p);

        // 9MB > class_size(8MB) -> move.
        void* r = uballoc::realloc(q, 9 * M);
        assert(r != nullptr && r != q);
        auto* moved = static_cast<const uint8_t*>(r);
        for (size_t i = 0; i < kAlloc; ++i) {
            assert(moved[i] == static_cast<uint8_t>(i * 131073ULL));
        }

        uballoc::free(r);
        std::cout << "  OK: huge rounding and move semantics verified" << std::endl;
        _exit(0);
    }
    AR_FORK_END(heap_id);
}

// -----------------------------------------------------------------------------
// DT06-7 R7: repeated grow/shrink cycles
// -----------------------------------------------------------------------------
void dt06_7_alternating_loop(const std::string& heap_id) {
    std::cout << "[DT06-7] 64 <-> 4096 alternating, 200 rounds..." << std::endl;
    AR_FORK_CASE() {
        ar_set_heap_id(heap_id);
        ar_child_init();

        constexpr size_t kSmall = 64;
        constexpr size_t kLarge = 4096;

        void* p = uballoc::malloc(kSmall);
        assert(p);
        size_t written = kSmall;
        uint8_t stamp = 1;
        std::memset(p, 0x21, written);
        static_cast<uint8_t*>(p)[0] = stamp;

        for (int round = 0; round < 200; ++round) {
            size_t next = (round % 2 == 0) ? kLarge : kSmall;
            uint8_t prev_stamp = stamp;
            stamp = static_cast<uint8_t>(round + 2);

            void* q = uballoc::realloc(p, next);
            assert(q != nullptr);

            auto* bytes = static_cast<const uint8_t*>(q);
            assert(bytes[0] == prev_stamp);
            for (size_t i = 1; i < written; ++i) {
                assert(bytes[i] == 0x21);
            }

            std::memset(q, 0x21, next);
            static_cast<uint8_t*>(q)[0] = stamp;
            written = next;
            p = q;
        }
        uballoc::free(p);
        std::cout << "  OK: 200 rounds without corruption" << std::endl;
        _exit(0);
    }
    AR_FORK_END(heap_id);
}

}  // namespace

int main() {
    std::string id = ar_heap_id("ar06");
    cleanup_all_distributed_shms(8, id);

    ar_pre_case_cleanup(id);

    dt06_1_grow_moves(id);
    ar_pre_case_cleanup(id);
    dt06_2_shrink_in_place(id);
    ar_pre_case_cleanup(id);
    dt06_3_same_class_grow(id);
    ar_pre_case_cleanup(id);
    dt06_4_nullptr_malloc(id);
    ar_pre_case_cleanup(id);
    dt06_5_zero_size(id);
    ar_pre_case_cleanup(id);
    dt06_6_huge_realloc(id);
    ar_pre_case_cleanup(id);
    dt06_7_alternating_loop(id);

    cleanup_all_distributed_shms(8, id);
    std::cout << "\n=== AR-06 DT passed ===" << std::endl;
    return 0;
}
