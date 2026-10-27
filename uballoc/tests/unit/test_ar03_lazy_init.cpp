// SPDX-License-Identifier: Apache-2.0
 
/**
 * @file test_ar03_lazy_init.cpp
 * @brief AR-03 DT: Lazy Initialization
 *
 * Cases:
 *   DT03-1 R1,R2: 2 procs, same UBALLOC_HEAP_ID, neither calls init() —
 *                 both discover each other (first malloc triggers lazy init)
 *   DT03-2 R3:    P1 starts first, then P0 — order-independent discovery
 *   DT03-3 R4:    lazy-init processes malloc / free cross-process
 *   DT03-4 R5:    lazy-init + explicit-init coexist on different heap IDs
 */
 
#include "uballoc.hpp"
#include "test_helpers.hpp"
 
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
 
#include <sys/wait.h>
 
namespace {
 
constexpr uint32_t TID_A = 30;
constexpr uint32_t TID_B = 31;
 
// -----------------------------------------------------------------------------
// DT03-1 R1,R2: neither calls init() — first malloc triggers lazy init
// -----------------------------------------------------------------------------
void dt03_1_no_explicit_init(const std::string& heap_id) {
    std::cout << "[DT03-1] neither calls init(); first malloc lazy-inits..." << std::endl;
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
 
        // NO init() — directly malloc. Lazy init must trigger.
        assert(!uballoc::is_initialized());
        void* p = uballoc::malloc(64);
        assert(p);
        assert(uballoc::is_initialized());
        assert(uballoc::rank() == 1);
        std::memset(p, 0x33, 64);
        assert(uballoc::publish(p, 64, TID_B) >= 0);
 
        // Look up P0's published object via lazy-init'd allocator.
        auto info = uballoc::lookup_by_type_blocking(TID_A, 5000);
        assert(info.owner_process == 0);
        assert(info.size == 64);
        auto* bytes = static_cast<const uint8_t*>(info.address);
        for (int i = 0; i < 64; ++i) assert(bytes[i] == 0xA3);
 
        ar_write_byte(rp[1], 'C');
        _exit(0);
    }
 
    ::close(fds[0]);
    ::close(rp[1]);
 
    ar_set_heap_id(heap_id);
    // NO init() — directly malloc. Lazy init must trigger here too.
    assert(!uballoc::is_initialized());
    void* p = uballoc::malloc(64);
    assert(p);
    assert(uballoc::is_initialized());
    assert(uballoc::rank() == 0);
    std::memset(p, 0xA3, 64);
    assert(uballoc::publish(p, 64, TID_A) >= 0);
    ar_write_byte(fds[1], 'G');
 
    assert(ar_read_byte(rp[0]) == 'C');
    auto info = uballoc::lookup_by_type_blocking(TID_B, 5000);
    assert(info.owner_process == 1);
    assert(info.size == 64);
    auto* bytes = static_cast<const uint8_t*>(info.address);
    for (int i = 0; i < 64; ++i) assert(bytes[i] == 0x33);
 
    int st;
    waitpid(pid, &st, 0);
    check_child(st);
    ::close(fds[1]);
    ::close(rp[0]);
    ar_alloc().reset();
    std::cout << "  OK: both lazy-init'd, mutual publish/lookup works" << std::endl;
}
 
// -----------------------------------------------------------------------------
// DT03-2 R3: P1 starts first, then P0 — order independence
// -----------------------------------------------------------------------------
void dt03_2_p1_first(const std::string& heap_id) {
    std::cout << "[DT03-2] P1 starts first, then P0 — order-independent..." << std::endl;
    int fds[2], rp[2];
    assert(pipe(fds) == 0);
    assert(pipe(rp) == 0);
 
    ar_set_heap_id(heap_id);
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        ::close(fds[1]);
        ::close(rp[0]);
 
        // P1 lazy-inits FIRST (before P0 exists). claim_rank should make P1
        // the bootstrap creator (rank 0) since no bootstrap exists yet.
        assert(!uballoc::is_initialized());
        void* p = uballoc::malloc(64);
        assert(p);
        assert(uballoc::is_initialized());
        // P1 (first process) claims rank 0.
        assert(uballoc::rank() == 0);
        std::memset(p, 0x57, 64);
        assert(uballoc::publish(p, 64, TID_A) >= 0);
 
        // Signal P0 (the real parent) that P1 is up.
        ar_write_byte(rp[1], 'R');
 
        // Wait for P0 to join.
        assert(ar_read_byte(fds[0]) == 'G');
        auto info = uballoc::lookup_by_type_blocking(TID_B, 5000);
        assert(info.owner_process == 1);
        assert(info.size == 64);
        auto* bytes = static_cast<const uint8_t*>(info.address);
        for (int i = 0; i < 64; ++i) assert(bytes[i] == 0x75);
 
        ar_write_byte(rp[1], 'C');
        _exit(0);
    }
 
    ::close(fds[0]);
    ::close(rp[1]);
    // Wait for P1 to come up first.
    assert(ar_read_byte(rp[0]) == 'R');
 
    ar_set_heap_id(heap_id);
    // P0 lazy-inits AFTER P1. P0 attaches to P1's bootstrap, claims rank 1.
    assert(!uballoc::is_initialized());
    void* p = uballoc::malloc(64);
    assert(p);
    assert(uballoc::is_initialized());
    assert(uballoc::rank() == 1);
    std::memset(p, 0x75, 64);
    assert(uballoc::publish(p, 64, TID_B) >= 0);
    ar_write_byte(fds[1], 'G');
 
    assert(ar_read_byte(rp[0]) == 'C');
    auto info = uballoc::lookup_by_type_blocking(TID_A, 5000);
    assert(info.owner_process == 0);
    assert(info.size == 64);
    auto* bytes = static_cast<const uint8_t*>(info.address);
    for (int i = 0; i < 64; ++i) assert(bytes[i] == 0x57);
 
    int st;
    waitpid(pid, &st, 0);
    check_child(st);
    ::close(fds[1]);
    ::close(rp[0]);
    ar_alloc().reset();
    std::cout << "  OK: P1 first (rank 0), P0 second (rank 1) — discovery works" << std::endl;
}
 
// -----------------------------------------------------------------------------
// DT03-3 R4: lazy-init processes malloc / free cross-process
// -----------------------------------------------------------------------------
void dt03_3_lazy_cross_malloc_free(const std::string& heap_id) {
    std::cout << "[DT03-3] lazy-init: cross-process malloc/free..." << std::endl;
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

        // P1 lazy-inits via malloc BEFORE accessing P0's pointer.
        assert(!uballoc::is_initialized());
        void* own = uballoc::malloc(64);
        assert(own);
        assert(uballoc::is_initialized());
        assert(uballoc::rank() == 1);

        // Now read P0's pointer and access it. lookup_by_type(TID_A) runs
        // check_remote_segments and attaches P0's data segment BEFORE the
        // deref. Without this discovery trigger, P0's segment is unmapped
        // and the raw deref below would SIGSEGV (the SIGSEGV handler only
        // covers the fd-open→mmap race, not the never-discovered case).
        uint64_t v = 0;
        ar_read_fd(fds[0], &v, sizeof(v));
        auto info = uballoc::lookup_by_type_blocking(TID_A, 5000);
        assert(info.owner_process == 0);
        assert(info.address == reinterpret_cast<void*>(v));
        auto* p = reinterpret_cast<uint8_t*>(v);
        for (int i = 0; i < 128; ++i) assert(p[i] == 0xC3);
        // Cross-process free of P0's allocation.
        uballoc::free(p);

        // P1 also allocates, publishes, and sends the pointer to P0.
        // publish+lookup_by_type is the contract for cross-process pointer
        // exchange (raw pipe-passed pointers require a discovery trigger
        // on the receiver side before deref).
        void* q = uballoc::malloc(64);
        assert(q);
        std::memset(q, 0x9E, 64);
        assert(uballoc::publish(q, 64, TID_B) >= 0);
        uint64_t qv = reinterpret_cast<uintptr_t>(q);
        ar_write_fd(rp[1], &qv, sizeof(qv));
        // Block until P0 acks so we don't _exit (and clear our slot) before
        // P0 has attached our segment and dereferenced q.
        assert(ar_read_byte(fds[0]) == 'A');
        _exit(0);
    }

    ::close(fds[0]);
    ::close(rp[1]);
    // P0 lazy-inits, allocates 128B, publishes + sends pointer to P1.
    assert(!uballoc::is_initialized());
    void* p = uballoc::malloc(128);
    assert(p);
    assert(uballoc::is_initialized());
    assert(uballoc::rank() == 0);
    std::memset(p, 0xC3, 128);
    assert(uballoc::publish(p, 128, TID_A) >= 0);
    ar_write_byte(fds[1], 'G');
    uint64_t v = reinterpret_cast<uintptr_t>(p);
    ar_write_fd(fds[1], &v, sizeof(v));

    // Receive P1's pointer. lookup_by_type(TID_B) triggers
    // check_remote_segments, attaching P1's data segment BEFORE we deref q.
    // Without this, q lives in P1's (never-attached) small-data segment and
    // the raw deref below would SIGSEGV (handler returns "no_segment").
    uint64_t qv = 0;
    ar_read_fd(rp[0], &qv, sizeof(qv));
    auto info_b = uballoc::lookup_by_type_blocking(TID_B, 5000);
    assert(info_b.owner_process == 1);
    assert(info_b.address == reinterpret_cast<void*>(qv));
    auto* q = reinterpret_cast<uint8_t*>(qv);
    for (int i = 0; i < 64; ++i) assert(q[i] == 0x9E);
    uballoc::free(q);

    // ACK P1 via fds[1] so it can exit cleanly (after we've attached + deref'd q).
    ar_write_byte(fds[1], 'A');
    int st;
    waitpid(pid, &st, 0);
    check_child(st);
    ::close(fds[1]);
    ::close(rp[0]);
    uballoc::unpublish(p);
    uballoc::free(p);
    ar_alloc().reset();
    std::cout << "  OK: bidirectional cross-process malloc/free works" << std::endl;
}
 
// -----------------------------------------------------------------------------
// DT03-4 R5: lazy-init + explicit-init coexist on different heap IDs
// -----------------------------------------------------------------------------
void dt03_4_lazy_and_explicit_coexist(const std::string& lazy_heap) {
    std::cout << "[DT03-4] lazy + explicit on different heap IDs coexist..." << std::endl;
    std::string explicit_heap = lazy_heap + "-explicit";
 
    int fds[2], rp[2];
    assert(pipe(fds) == 0);
    assert(pipe(rp) == 0);
 
    // Parent runs the EXPLICIT path on its own heap ID.
    ar_set_heap_id(explicit_heap);
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        ::close(fds[1]);
        ::close(rp[0]);
        assert(ar_read_byte(fds[0]) == 'G');
 
        // Child uses LAZY init on a DIFFERENT heap ID.
        ar_set_heap_id(lazy_heap);
        assert(!uballoc::is_initialized());
        void* p = uballoc::malloc(64);
        assert(p);
        assert(uballoc::is_initialized());
        std::memset(p, 0xE4, 64);
        assert(uballoc::publish(p, 64, TID_A) >= 0);
 
        ar_write_byte(rp[1], 'C');
        _exit(0);
    }
 
    ::close(fds[0]);
    ::close(rp[1]);
    ar_parent_init();  // explicit init on explicit_heap
    assert(uballoc::is_initialized());
    assert(uballoc::rank() == 0);
    void* p = uballoc::malloc(64);
    assert(p);
    std::memset(p, 0x4E, 64);
    assert(uballoc::publish(p, 64, TID_B) >= 0);
    ar_write_byte(fds[1], 'G');
 
    assert(ar_read_byte(rp[0]) == 'C');
 
    // Verify isolation: parent's TID_B is on explicit_heap; child's TID_A is
    // on lazy_heap. Looking up TID_A from the parent's heap must NOT find it
    // (different heap, different bootstrap).
    auto info = uballoc::lookup_by_type(TID_A);
    assert(info.owner_process == -1);
 
    auto info_b = uballoc::lookup_by_type(TID_B);
    assert(info_b.owner_process == 0);
    assert(info_b.size == 64);
 
    int st;
    waitpid(pid, &st, 0);
    check_child(st);
    ::close(fds[1]);
    ::close(rp[0]);
    uballoc::unpublish(p);
    uballoc::free(p);
    ar_alloc().reset();
 
    // Cleanup the lazy heap's shms too.
    cleanup_all_distributed_shms(8, lazy_heap);
    std::cout << "  OK: lazy and explicit on different heap IDs coexist (isolated)" << std::endl;
}
 
}  // namespace
 
int main() {
    std::string id = ar_heap_id("ar03");
    cleanup_all_distributed_shms(8, id);
 
    ar_pre_case_cleanup(id);
    dt03_1_no_explicit_init(id);
    ar_pre_case_cleanup(id);
    dt03_2_p1_first(id);
    ar_pre_case_cleanup(id);
    dt03_3_lazy_cross_malloc_free(id);
    ar_pre_case_cleanup(id);
    dt03_4_lazy_and_explicit_coexist(id);
 
    cleanup_all_distributed_shms(8, id);
    std::cout << "\n=== AR-03 DT passed ===" << std::endl;
    return 0;
}
