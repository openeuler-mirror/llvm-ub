// SPDX-License-Identifier: Apache-2.0

/**
 * @file test_ar04_explicit_init.cpp
 * @brief AR-04 DT: Explicit Initialization
 *
 * DT design: doc/ar_dt/plans/AR-04_explicit_init.md
 * Cases:
 *   DT04-1 R1,R5: both processes explicit init(); cross-visible via publish/lookup
 *   DT04-2 R4: no env -> no abort, uninitialized (C++ + C API)
 *   DT04-3 R3: init() idempotent, existing allocations undisturbed
 *   DT04-4 R5: init -> publish/lookup self works (set_slot_ready external effect)
 *   DT04-5 R2: P0 explicit init; P1 lazy via first malloc — coexist
 *   DT04-6 R6: C API full chain interoperates with C++ API (same singleton)
 */

#include "uballoc.hpp"
#include "test_helpers.hpp"

#include <cstdint>
#include <cstring>
#include <iostream>

#include <uballoc.h>

#include <sys/wait.h>

namespace {

// -----------------------------------------------------------------------------
// DT04-1 R1,R5: both processes explicit init(); cross-visible
// -----------------------------------------------------------------------------
void dt04_1_both_explicit(const std::string& heap_id) {
    std::cout << "[DT04-1] both processes explicit init(); cross-visible..." << std::endl;
    int fds[2], rp[2];
    assert(pipe(fds) == 0);
    assert(pipe(rp) == 0);

    ar_set_heap_id(heap_id);
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        ::close(fds[1]);
        ::close(rp[0]);
        assert(ar_read_byte(fds[0]) == 'G');  // P0 ready

        ar_child_init();  // explicit init() in P1

        assert(uballoc::is_initialized());
        assert(uballoc::rank() == 1);

        auto info = uballoc::lookup_by_type_blocking(200, 5000);
        assert(info.owner_process == 0);
        assert(info.size == 64);
        auto* bytes = static_cast<const uint8_t*>(info.address);
        for (int i = 0; i < 64; ++i) assert(bytes[i] == 0xAA);

        void* p = uballoc::malloc(64);
        assert(p);
        std::memset(p, 0xBB, 64);
        assert(uballoc::publish(p, 64, 201) >= 0);

        ar_write_byte(rp[1], 'C');
        _exit(0);
    }

    ::close(fds[0]);
    ::close(rp[1]);
    ar_parent_init();

    assert(uballoc::is_initialized());
    assert(uballoc::rank() == 0);

    void* p = uballoc::malloc(64);
    assert(p);
    std::memset(p, 0xAA, 64);
    assert(uballoc::publish(p, 64, 200) >= 0);

    ar_write_byte(fds[1], 'G');
    assert(ar_read_byte(rp[0]) == 'C');  // P1 done

    auto info = uballoc::lookup_by_type_blocking(201, 5000);
    assert(info.owner_process == 1);
    assert(info.size == 64);
    auto* bytes = static_cast<const uint8_t*>(info.address);
    for (int i = 0; i < 64; ++i) assert(bytes[i] == 0xBB);

    int st;
    waitpid(pid, &st, 0);
    check_child(st);
    ::close(fds[1]);
    ar_alloc().reset();
}

// -----------------------------------------------------------------------------
// DT04-2 R4: no env — must not abort
// -----------------------------------------------------------------------------
void dt04_2_no_env_rejected(const std::string& heap_id) {
    std::cout << "[DT04-2] no env: C++ and C init are safe no-ops..." << std::endl;
    (void)heap_id;
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        ar_unset_heap_id();

        uballoc::init();
        assert(!uballoc::is_initialized());

        uballoc_init();
        assert(uballoc_is_initialized() == false);

        std::cout << "  OK: no abort, stays uninitialized" << std::endl;
        _exit(0);
    }
    int st;
    waitpid(pid, &st, 0);
    check_child(st);
}

// -----------------------------------------------------------------------------
// DT04-3 R3: idempotent re-init
// -----------------------------------------------------------------------------
void dt04_3_idempotent(const std::string& heap_id) {
    std::cout << "[DT04-3] repeated init() is a no-op..." << std::endl;
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        ar_set_heap_id(heap_id);
        uballoc::init();
        uballoc::init();
        uballoc::init();
        assert(uballoc::is_initialized());

        void* p = uballoc::malloc(64);
        assert(p);
        std::memset(p, 0x77, 64);

        uballoc::init();  // 4th call after allocation
        auto* bytes = static_cast<uint8_t*>(p);
        for (int i = 0; i < 64; ++i) {
            assert(bytes[i] == 0x77);
        }
        uballoc::free(p);
        std::cout << "  OK: idempotent, allocation undisturbed" << std::endl;
        _exit(0);
    }
    int st;
    waitpid(pid, &st, 0);
    check_child(st);
}

// -----------------------------------------------------------------------------
// DT04-4 R5: init -> publish/lookup self works
// -----------------------------------------------------------------------------
void dt04_4_slot_ready(const std::string& heap_id) {
    std::cout << "[DT04-4] init -> publish/lookup self works..." << std::endl;
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        ar_set_heap_id(heap_id);
        ar_child_init();

        assert(uballoc::is_initialized());
        assert(uballoc::rank() >= 0);

        void* p = uballoc::malloc(64);
        assert(p);
        std::memset(p, 0x55, 64);
        assert(uballoc::publish(p, 64, 200) >= 0);

        auto info = uballoc::lookup_by_type(200);
        assert(info.address == p);
        assert(info.size == 64);
        assert(info.owner_process == uballoc::rank());
        auto* bytes = static_cast<const uint8_t*>(info.address);
        for (int i = 0; i < 64; ++i) assert(bytes[i] == 0x55);

        uballoc::free(p);
        std::cout << "  OK: publish/lookup works after init (slot ready)" << std::endl;
        _exit(0);
    }
    int st;
    waitpid(pid, &st, 0);
    check_child(st);
}

// -----------------------------------------------------------------------------
// DT04-5 R2: explicit init (P0) coexists with lazy init (P1)
// -----------------------------------------------------------------------------
void dt04_5_explicit_lazy_coexist(const std::string& heap_id) {
    std::cout << "[DT04-5] P0 explicit init; P1 lazy via first malloc..." << std::endl;
    int fds[2], rp[2];
    assert(pipe(fds) == 0);
    assert(pipe(rp) == 0);

    ar_set_heap_id(heap_id);
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        ::close(fds[1]);
        ::close(rp[0]);
        assert(ar_read_byte(fds[0]) == 'G');  // P0 ready

        // P1 never calls init(): the first malloc triggers lazy initialization.
        void* p = uballoc::malloc(64);
        assert(p);
        assert(uballoc::is_initialized());
        assert(uballoc::rank() == 1);
        assert(uballoc::publish(p, 64, 201) >= 0);
        std::memset(p, 0x99, 64);

        uint64_t v = reinterpret_cast<uintptr_t>(p);
        ar_write_fd(rp[1], &v, sizeof(v));
        _exit(0);
    }

    ::close(fds[0]);
    ::close(rp[1]);
    ar_parent_init();
    ar_write_byte(fds[1], 'G');

    uint64_t v = 0;
    ar_read_fd(rp[0], &v, sizeof(v));
    auto info = uballoc::lookup_by_type_blocking(201, 5000);
    assert(info.owner_process == 1);
    assert(info.address == reinterpret_cast<void*>(v));
    assert(info.size == 64);
    auto* bytes = static_cast<const uint8_t*>(info.address);
    for (int i = 0; i < 64; ++i) {
        assert(bytes[i] == 0x99);
    }

    int st;
    waitpid(pid, &st, 0);
    check_child(st);
    ::close(fds[1]);
    ar_alloc().reset();
}

// -----------------------------------------------------------------------------
// DT04-6 R6: C API full chain, interoperable with C++ API
// -----------------------------------------------------------------------------
void dt04_6_c_api_chain(const std::string& heap_id) {
    std::cout << "[DT04-6] C API chain: init/thread/malloc/realloc/free/reset..." << std::endl;
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        ar_set_heap_id(heap_id);

        uballoc_init();
        assert(uballoc_is_initialized());
        uballoc_init_thread(0);

        void* p = uballoc_malloc(64);
        assert(p);
        std::memset(p, 0xCC, 64);

        void* q = uballoc_realloc(p, 256);
        assert(q != nullptr);
        auto* bytes = static_cast<const uint8_t*>(q);
        for (int i = 0; i < 64; ++i) {
            assert(bytes[i] == 0xCC);
        }
        uballoc_free(q);

        // Interleave with the C++ API — same singleton.
        void* c = uballoc::malloc(32);
        assert(c);
        uballoc::free(c);

        uballoc_reset();
        assert(uballoc_is_initialized() == false);

        std::cout << "  OK: C and C++ API interoperable, reset clears state" << std::endl;
        _exit(0);
    }
    int st;
    waitpid(pid, &st, 0);
    check_child(st);
}

}  // namespace

int main() {
    std::string id = ar_heap_id("ar04");
    cleanup_all_distributed_shms(8, id);

    ar_pre_case_cleanup(id);

    dt04_1_both_explicit(id);
    ar_pre_case_cleanup(id);
    dt04_2_no_env_rejected(id);
    ar_pre_case_cleanup(id);
    dt04_3_idempotent(id);
    ar_pre_case_cleanup(id);
    dt04_4_slot_ready(id);
    ar_pre_case_cleanup(id);
    dt04_5_explicit_lazy_coexist(id);
    ar_pre_case_cleanup(id);
    dt04_6_c_api_chain(id);

    cleanup_all_distributed_shms(8, id);
    std::cout << "\n=== AR-04 DT passed ===" << std::endl;
    return 0;
}
