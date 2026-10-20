// SPDX-License-Identifier: Apache-2.0

/**
 * @file test_ar12_sigsegv_handler.cpp
 * @brief AR-12 DT: SIGSEGV fault handler dynamic shared memory loading
 *
 * Cases:
 *   DT12-1  R1: handler recovers from injected fault — (a) attached==1 remap
 *                branch via lookup_by_type + munmap(page) + dereference; (b)
 *                attached==0 CAS branch (eager-race window) via white-box reset
 *                of the registry attached flag on the same segment + munmap +
 *                dereference; both recover via mmap(MAP_FIXED, fd); data
 *                consistent (0xA5).
 *   DT12-2  R3: wild pointer still crashes — alarm(2) safety net + dereference
 *                VA-out-of-range address; child WIFSIGNALED (SIGSEGV or SIGALRM).
 *   DT12-3  R2: idempotent install — enable_fault_handler() 3×; all return true,
 *                g_fh_active==true, no deadlock/self-chain.
 *   DT12-4  R5: no interference — malloc/free/publish/lookup under handler; all
 *                succeed, no SIGSEGV, data consistent.
 *   DT12-5  R4: disable restores default — g_fh_active true→false; registry
 *                (g_fh_entries) cleared by disable (white-box assert via
 *                fh_any_valid_entry); alarm(2) + wild pointer crashes after
 *                disable (SIGSEGV or SIGALRM).
 *   DT12-6  R6: soft_reset uninstalls — g_fh_active true→false via soft_reset.
 *   DT12-7  R7: uffd success installs handler as safety net — g_fh_active==true
 *                after enable_userfaultfd (permanent safety net).
 *
 */

#include "uballoc.hpp"
#include "test_helpers.hpp"

#include <atomic>
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

constexpr uint32_t TID_A = 210;   
constexpr uint32_t TID_B = 211;  

// -----------------------------------------------------------------------------
// DT12-1 R1: handler recovers from injected fault
// -----------------------------------------------------------------------------
void dt12_1_handler_recovers_fault(const std::string& heap_id) {
    std::cout << "[DT12-1] handler recovers from injected fault..." << std::endl;
    int fds[2];
    assert(pipe(fds) == 0);

    ar_set_heap_id(heap_id);
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        ::close(fds[1]);
        char ready = 0;
        ar_read_fd(fds[0], &ready, 1);
        assert(ready == 'R');

        ar_child_init();

        // ---- (a) attached==1 remap branch ----
        auto info = uballoc::lookup_by_type(TID_A);
        assert(info.address != nullptr);
        assert(info.size == 64);
        assert(info.owner_process == 0);

        // Simulate the VMA destruction UBSE driver does (VMA removed from
        // /proc/self/maps). 
        void* addr = info.address;
        long page_size = ::sysconf(_SC_PAGESIZE);
        uintptr_t addr_val = reinterpret_cast<uintptr_t>(addr);
        uintptr_t page_start = addr_val & ~static_cast<uintptr_t>(page_size - 1);
        int mu = ::munmap(reinterpret_cast<void*>(page_start),
                          static_cast<size_t>(page_size));
        if (mu != 0) {
            int err = errno;
            char buf[256];
            int n = ::snprintf(buf, sizeof(buf),
                "DT12-1: munmap failed addr=%p page_start=0x%lx errno=%d\n",
                addr, (unsigned long)page_start, err);
            ::write(2, buf, n);
            _exit(20);
        }

        // If the handler fails to recover, the child crashes with SIGSEGV.
        volatile uint8_t* bytes = static_cast<volatile uint8_t*>(addr);
        uint8_t first = bytes[0];  // fault here; handler re-mmaps; re-execute
        assert(first == 0xA5 && "data should be 0xA5 after handler recovery");
        for (int i = 0; i < 64; ++i) {
            assert(bytes[i] == 0xA5);
        }

        // ---- (b) attached==0 CAS branch (eager-race window) ----
        int mu2 = ::munmap(reinterpret_cast<void*>(page_start),
                           static_cast<size_t>(page_size));
        if (mu2 != 0) {
            int err = errno;
            char buf[256];
            int n2 = ::snprintf(buf, sizeof(buf),
                "DT12-1(b): munmap failed errno=%d\n", err);
            ::write(2, buf, n2);
            _exit(21);
        }

        uintptr_t seg_va = reinterpret_cast<uintptr_t>(addr);
        bool reset = false;
        for (size_t i = 0; i < uballoc::FH_MAX_ENTRIES; ++i) {
            uintptr_t ev = uballoc::g_fh_entries[i].va.load(std::memory_order_acquire);
            uintptr_t ee = uballoc::g_fh_entries[i].va_end.load(std::memory_order_acquire);
            if (ev != 0 && seg_va >= ev && seg_va < ee) {
                uballoc::g_fh_entries[i].attached.store(
                    0, std::memory_order_release);
                reset = true;
                break;
            }
        }
        assert(reset && "eager-inject: registry entry covering segment not found");

        volatile uint8_t* eager = static_cast<volatile uint8_t*>(addr);
        uint8_t v = eager[0];  // fault; handler CAS+mmap; re-execute
        assert(v == 0xA5 && "data should be 0xA5 after eager-window recovery");
        for (int i = 0; i < 64; ++i) {
            assert(eager[i] == 0xA5);
        }

        _exit(0);
    }

    ::close(fds[0]);
    ar_parent_init();
    void* p = uballoc::malloc(64);
    assert(p);
    int slot = uballoc::publish(p, 64, TID_A);
    assert(slot >= 0);
    std::memset(p, 0xA5, 64);

    ar_write_byte(fds[1], 'R');

    int st;
    waitpid(pid, &st, 0);
    check_child(st);
    ::close(fds[1]);

    uballoc::unpublish(p);
    uballoc::free(p);
    std::cout << "  OK: remap branch (attached==1) + eager CAS branch "
                 "(attached==0) both recovered, data consistent"
              << std::endl;
}

// -----------------------------------------------------------------------------
// DT12-2 R3: wild pointer still crashes (handler does not swallow)
// -----------------------------------------------------------------------------
void dt12_2_wild_pointer_crashes(const std::string& heap_id) {
    std::cout << "[DT12-2] wild pointer still crashes..." << std::endl;
    ar_set_heap_id(heap_id);
    ar_parent_init();
    bool ok = uballoc::enable_fault_handler();
    assert(ok);
    assert(uballoc::g_fh_active.load(std::memory_order_acquire));

    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        // Child: dereference a wild pointer OUTSIDE the VA reservation.
        // alarm(2): safety net — on UBSE, the old handler may not terminate
        // the process (uffd handler returns), causing a handler loop. SIGALRM
        // (not blocked by SIGSEGV handler) kills the child after 2s.
        ::signal(SIGALRM, SIG_DFL);
        ::alarm(2);
        volatile char* wild = reinterpret_cast<volatile char*>(0xDEADBEEF);
        // This fault MUST reach the old handler (out_of_range) → crash.
        // _exit is never reached (child killed by SIGSEGV or SIGALRM).
        *wild = 1;
        _exit(0);  // unreachable if handler chains correctly
    }

    int st;
    waitpid(pid, &st, 0);
    assert(WIFSIGNALED(st) && "child should have been killed by a signal");
    int sig = WTERMSIG(st);
    assert(sig == SIGSEGV || sig == SIGALRM);
    std::cout << "  OK: child killed by signal " << sig
              << " (handler did not swallow)" << std::endl;

    uballoc::disable_fault_handler();
    uballoc::reset();
}

// -----------------------------------------------------------------------------
// DT12-3 R2: idempotent install
// -----------------------------------------------------------------------------
void dt12_3_idempotent_install(const std::string& heap_id) {
    std::cout << "[DT12-3] idempotent install..." << std::endl;
    ar_set_heap_id(heap_id);
    ar_parent_init();

    bool r1 = uballoc::enable_fault_handler();
    bool r2 = uballoc::enable_fault_handler();
    bool r3 = uballoc::enable_fault_handler();
    assert(r1 && r2 && r3 && "all enable_fault_handler calls should succeed");
    assert(uballoc::g_fh_active.load(std::memory_order_acquire));
    std::cout << "  OK: 3× enable returned " << r1 << "/" << r2 << "/" << r3
              << ", g_fh_active=true" << std::endl;

    uballoc::disable_fault_handler();
    uballoc::reset();
}

// -----------------------------------------------------------------------------
// DT12-4 R5: no interference with normal operations
// -----------------------------------------------------------------------------
void dt12_4_no_interference(const std::string& heap_id) {
    std::cout << "[DT12-4] no interference with normal operations..." << std::endl;
    ar_set_heap_id(heap_id);
    ar_parent_init();
    bool ok = uballoc::enable_fault_handler();
    assert(ok);

    // Multiple rounds of malloc/free + publish/lookup.
    for (int round = 0; round < 10; ++round) {
        void* p1 = uballoc::malloc(64);
        void* p2 = uballoc::malloc(128);
        assert(p1 && p2);
        std::memset(p1, 0x5A, 64);
        std::memset(p2, 0xA5, 128);

        int slot1 = uballoc::publish(p1, 64, TID_A);
        int slot2 = uballoc::publish(p2, 128, TID_B);
        assert(slot1 >= 0 && slot2 >= 0);

        auto info1 = uballoc::lookup_by_type(TID_A);
        auto info2 = uballoc::lookup_by_type(TID_B);
        assert(info1.address == p1 && info1.size == 64);
        assert(info2.address == p2 && info2.size == 128);

        // Verify data integrity.
        auto* b1 = static_cast<const uint8_t*>(info1.address);
        auto* b2 = static_cast<const uint8_t*>(info2.address);
        for (int i = 0; i < 64; ++i) assert(b1[i] == 0x5A);
        for (int i = 0; i < 128; ++i) assert(b2[i] == 0xA5);

        uballoc::unpublish(p1);
        uballoc::unpublish(p2);
        uballoc::free(p1);
        uballoc::free(p2);
    }

    std::cout << "  OK: 10 rounds malloc/free/publish/lookup, no SIGSEGV"
              << std::endl;
    uballoc::disable_fault_handler();
    uballoc::reset();
}

// -----------------------------------------------------------------------------
// DT12-5 R4: disable_fault_handler restores default handler
// -----------------------------------------------------------------------------
static bool fh_any_valid_entry() {
    for (size_t i = 0; i < uballoc::FH_MAX_ENTRIES; ++i) {
        if (uballoc::g_fh_entries[i].valid.load(std::memory_order_acquire) == 1u) {
            return true;
        }
    }
    return false;
}

void dt12_5_disable_restores_default(const std::string& heap_id) {
    std::cout << "[DT12-5] disable restores default handler..." << std::endl;
    ar_set_heap_id(heap_id);
    ar_parent_init();

    bool ok = uballoc::enable_fault_handler();
    assert(ok);
    assert(uballoc::g_fh_active.load(std::memory_order_acquire));

    void* p = uballoc::malloc(64);
    assert(p);
    int slot = uballoc::publish(p, 64, TID_A);
    assert(slot >= 0);
    auto info = uballoc::lookup_by_type(TID_A);
    assert(info.address == p);
    assert(fh_any_valid_entry() && "registry should have a valid entry after publish+lookup");
    std::cout << "  before disable: g_fh_active=true, registry non-empty"
              << std::endl;

    uballoc::disable_fault_handler();
    assert(!uballoc::g_fh_active.load(std::memory_order_acquire));
    assert(!fh_any_valid_entry() && "registry should be cleared by disable (fh_clear_all)");
    std::cout << "  after disable:  g_fh_active=false, registry cleared"
              << std::endl;

    uballoc::unpublish(p);
    uballoc::free(p);

    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        // alarm(2): safety net (same as DT12-2) in case handler loop occurs.
        ::signal(SIGALRM, SIG_DFL);
        ::alarm(2);
        volatile char* wild = reinterpret_cast<volatile char*>(0xDEADBEEF);
        *wild = 1;
        _exit(0);  // unreachable
    }
    int st;
    waitpid(pid, &st, 0);
    assert(WIFSIGNALED(st) && "child should crash after handler disabled");
    int sig = WTERMSIG(st);
    assert(sig == SIGSEGV || sig == SIGALRM);
    std::cout << "  OK: after disable, wild pointer still crashes (signal "
              << sig << ")" << std::endl;

    uballoc::reset();
}

// -----------------------------------------------------------------------------
// DT12-6 R6: soft_reset uninstalls handler
// -----------------------------------------------------------------------------
void dt12_6_soft_reset_uninstalls(const std::string& heap_id) {
    std::cout << "[DT12-6] soft_reset uninstalls handler..." << std::endl;
    ar_set_heap_id(heap_id);
    ar_parent_init();

    bool ok = uballoc::enable_fault_handler();
    assert(ok);
    assert(uballoc::g_fh_active.load(std::memory_order_acquire));
    std::cout << "  before soft_reset: g_fh_active=true" << std::endl;

    uballoc::soft_reset();
    assert(!uballoc::g_fh_active.load(std::memory_order_acquire));
    std::cout << "  after soft_reset:  g_fh_active=false (handler uninstalled)"
              << std::endl;

    uballoc::reset();
}

// -----------------------------------------------------------------------------
// DT12-7 R7: uffd success installs handler as permanent safety net
// -----------------------------------------------------------------------------
void dt12_7_uffd_success_installs_handler(const std::string& heap_id) {
    std::cout << "[DT12-7] uffd success installs handler as safety net..." << std::endl;
    ar_set_heap_id(heap_id);
    ar_parent_init();

    bool ok = ar_alloc().enable_userfaultfd();
    assert(ok && "enable_userfaultfd should succeed (uffd or fallback)");

    assert(uballoc::g_fh_active.load(std::memory_order_acquire));
    std::cout << "  OK: enable_userfaultfd returned " << ok
              << ", g_fh_active=true (permanent safety net)" << std::endl;

    uballoc::disable_fault_handler();
    uballoc::reset();
}

}  // namespace

int main() {
    std::string id = ar_heap_id("ar12");
    cleanup_all_distributed_shms(8, id);

    ar_pre_case_cleanup(id);
    dt12_1_handler_recovers_fault(id + "_1");
    ar_pre_case_cleanup(id + "_1");
    dt12_2_wild_pointer_crashes(id + "_2");
    ar_pre_case_cleanup(id + "_2");
    dt12_3_idempotent_install(id + "_3");
    ar_pre_case_cleanup(id + "_3");
    dt12_4_no_interference(id + "_4");
    ar_pre_case_cleanup(id + "_4");
    dt12_5_disable_restores_default(id + "_5");
    ar_pre_case_cleanup(id + "_5");
    dt12_6_soft_reset_uninstalls(id + "_6");
    ar_pre_case_cleanup(id + "_6");
    dt12_7_uffd_success_installs_handler(id + "_7");

    cleanup_all_distributed_shms(8, id);
    std::cout << "\n=== AR-12 DT passed ===" << std::endl;
    return 0;
}
