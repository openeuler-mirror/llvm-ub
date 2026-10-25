// SPDX-License-Identifier: Apache-2.0

/**
 * @file test_ar13_userfaultfd.cpp
 * @brief AR-13 DT: userfaultfd on-demand segment mapping
 *
 * Cases:
 *   DT13-1  R1: uffd enable + SIGSEGV safety net coexist
 *   DT13-2  R2: remote fault recovery (uffd transparent loading)
 *   DT13-3  R3,R4: multi-publish same segment (one mmap, data consistent)
 *   DT13-4  R4: whole-segment mmap (second page no re-fault)
 *   DT13-5  R6: seg_not_in_dir → uffd handler fires + uffd_poison called
 *   DT13-6  R6: (reserved) ZEROPAGE fallback — not reachable (LOG_ERROR aborts before ioctl)
 *   DT13-7  R7: disable_userfaultfd + re-enable SIGSEGV mode
 *   DT13-8  R5: eager+uffd coexist (eager segment no uffd fault)
 *   DT13-9  R8: fork child soft_reset → uffd_uninstall (no std::terminate)
 *
 * All DT13 cases hard-require uffd. On hardened hosts where uffd is
 * unavailable (vm.unprivileged_userfaultfd=0 or non-root), the suite
 * probes once at start, asserts the documented SIGSEGV fallback
 * (g_uffd_active==false && g_fh_active==true), and skips all cases.
 */

#include "uballoc.hpp"
#include "test_helpers.hpp"

#include <atomic>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>

#include <poll.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

constexpr uint32_t TID_A = 230;
constexpr uint32_t TID_B = 231;

// count uffd-attached DATA segments (region_type >= 1)
static int uffd_data_attached_count() {
    std::lock_guard<std::mutex> lk(uballoc::g_uffd_attached_mtx);
    int n = 0;
    for (const auto& e : uballoc::g_uffd_attached) {
        if (e.region_type >= 1) ++n;
    }
    return n;
}

static int uffd_all_attached_count() {
    std::lock_guard<std::mutex> lk(uballoc::g_uffd_attached_mtx);
    return static_cast<int>(uballoc::g_uffd_attached.size());
}

// Probe whether userfaultfd is available on this host. On hardened kernels
// (vm.unprivileged_userfaultfd=0 — the openEuler/RHEL default for non-root),
// the uffd syscall fails and the library degrades to the SIGSEGV handler.
// All DT13 cases hard-require uffd, so we probe once and skip the entire
// suite when unavailable, asserting the documented fallback instead.
static bool uffd_available() {
    ar_set_heap_id("ar13_probe");
    ar_parent_init();
    bool uffd_ok = uballoc::g_uffd_active.load(std::memory_order_acquire);
    bool fh_ok = uballoc::g_fh_active.load(std::memory_order_acquire);
    // Documented fallback: uffd unavailable → SIGSEGV handler must be active.
    assert(fh_ok && "g_fh_active must be true (uffd or SIGSEGV fallback)");
    if (!uffd_ok) {
        std::cout << "[probe] uffd unavailable — fallback active"
                  << " (g_uffd_active=false, g_fh_active=true)" << std::endl;
    }
    uballoc::reset();
    cleanup_all_distributed_shms(static_cast<int>(uballoc::MAX_PROCESSES),
                                 "ar13_probe");
    return uffd_ok;
}

// ---------------------------------------------------------------------------
// DT13-1 R1: uffd enable + SIGSEGV safety net coexist
// ---------------------------------------------------------------------------
void dt13_1_uffd_enable(const std::string& heap_id) {
    std::cout << "[DT13-1] uffd enable + SIGSEGV safety net..." << std::endl;
    ar_set_heap_id(heap_id);
    ar_parent_init();

    assert(uballoc::g_uffd_active.load(std::memory_order_acquire));
    assert(uballoc::g_fh_active.load(std::memory_order_acquire));
    assert(uballoc::g_uffd_fd >= 0);
    std::cout << "  OK: g_uffd_active=true, g_fh_active=true, fd=" << uballoc::g_uffd_fd << std::endl;

    uballoc::reset();
}

// ---------------------------------------------------------------------------
// DT13-2 R2: remote fault recovery (uffd transparent loading)
// ---------------------------------------------------------------------------
void dt13_2_remote_fault_recovery(const std::string& heap_id) {
    std::cout << "[DT13-2] remote fault recovery (uffd transparent loading)..." << std::endl;
    int fds[2], rp[2];
    assert(pipe(fds) == 0);
    assert(pipe(rp) == 0);

    ar_set_heap_id(heap_id);
    // P0 init BEFORE fork — claim slot 0 first.
    ar_parent_init();

    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        ::close(fds[1]);
        ::close(rp[0]);

        // P1 init AFTER P0 — claims slot 1.
        ar_child_init();
        assert(uballoc::g_uffd_active.load(std::memory_order_acquire));

        ar_write_byte(rp[1], 'R');

        assert(ar_read_byte(fds[0]) == 'G');
        uint64_t addr_val = 0;
        ar_read_fd(fds[0], &addr_val, sizeof(addr_val));
        volatile uint8_t* p = reinterpret_cast<volatile uint8_t*>(addr_val);

        uint8_t v = p[0];  // uffd fault → handler mmaps whole segment
        assert(v == 0xA5 && "data should be 0xA5 after uffd recovery");
        for (int i = 0; i < 64; ++i) assert(p[i] == 0xA5);

        ar_write_byte(rp[1], 'D');
        _exit(0);
    }

    ::close(fds[0]);
    ::close(rp[1]);

    assert(ar_read_byte(rp[0]) == 'R');

    void* p = uballoc::malloc(64);
    assert(p);
    int slot = uballoc::publish(p, 64, TID_A);
    assert(slot >= 0);
    std::memset(p, 0xA5, 64);

    ar_write_byte(fds[1], 'G');
    uint64_t addr_val = reinterpret_cast<uintptr_t>(p);
    ar_write_fd(fds[1], &addr_val, sizeof(addr_val));

    assert(ar_read_byte(rp[0]) == 'D');

    int st;
    waitpid(pid, &st, 0);
    check_child(st);
    ::close(fds[1]);
    ::close(rp[0]);

    uballoc::unpublish(p);
    uballoc::free(p);
    uballoc::reset();
    std::cout << "  OK: uffd transparent loading — remote segment accessed, data consistent (0xA5)" << std::endl;
}

// ---------------------------------------------------------------------------
// DT13-3 R3: multi-publish fault recovery (same segment, one mmap)
// ---------------------------------------------------------------------------
void dt13_3_multi_publish_recovery(const std::string& heap_id) {
    std::cout << "[DT13-3] multi-publish fault recovery..." << std::endl;
    int fds[2], rp[2];
    assert(pipe(fds) == 0);
    assert(pipe(rp) == 0);

    ar_set_heap_id(heap_id);
    ar_parent_init();
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        ::close(fds[1]);
        ::close(rp[0]);
        ar_child_init();
        assert(uballoc::g_uffd_active.load(std::memory_order_acquire));

        // Signal P0 that P1's init (including check_remote) is done.
        ar_write_byte(rp[1], 'R');

        assert(ar_read_byte(fds[0]) == 'G');
        uint64_t a1 = 0, a2 = 0;
        ar_read_fd(fds[0], &a1, sizeof(a1));
        ar_read_fd(fds[0], &a2, sizeof(a2));

        volatile uint8_t* p1 = reinterpret_cast<volatile uint8_t*>(a1);
        volatile uint8_t* p2 = reinterpret_cast<volatile uint8_t*>(a2);

        uint8_t v1 = p1[0];  // uffd fault → handler mmap whole segment
        uint8_t v2 = p2[0];  // PTE exists, no uffd fault
        assert(v1 == 0xA5 && "seg1 data should be 0xA5");
        assert(v2 == 0x5A && "seg2 data should be 0x5A");
        for (int i = 0; i < 64; ++i) assert(p1[i] == 0xA5);
        for (int i = 0; i < 64; ++i) assert(p2[i] == 0x5A);

        // Same segment → at most 1 uffd DATA attach entry (one whole-segment mmap).
        int data_count = uffd_data_attached_count();
        assert(data_count <= 1 && "same segment should produce at most 1 uffd attach");

        ar_write_byte(rp[1], 'D');
        _exit(0);
    }

    ::close(fds[0]);
    ::close(rp[1]);

    assert(ar_read_byte(rp[0]) == 'R');

    void* p1 = uballoc::malloc(64);
    void* p2 = uballoc::malloc(64);
    assert(p1 && p2);
    assert(uballoc::publish(p1, 64, TID_A) >= 0);
    assert(uballoc::publish(p2, 64, TID_B) >= 0);
    std::memset(p1, 0xA5, 64);
    std::memset(p2, 0x5A, 64);

    ar_write_byte(fds[1], 'G');
    uint64_t a1 = reinterpret_cast<uintptr_t>(p1);
    uint64_t a2 = reinterpret_cast<uintptr_t>(p2);
    ar_write_fd(fds[1], &a1, sizeof(a1));
    ar_write_fd(fds[1], &a2, sizeof(a2));

    assert(ar_read_byte(rp[0]) == 'D');
    int st;
    waitpid(pid, &st, 0);
    check_child(st);
    ::close(fds[1]);
    ::close(rp[0]);

    uballoc::unpublish(p1);
    uballoc::unpublish(p2);
    uballoc::free(p1);
    uballoc::free(p2);
    uballoc::reset();
    std::cout << "  OK: two publishes same segment, 1 uffd attach, both data consistent" << std::endl;
}

// ---------------------------------------------------------------------------
// DT13-4 R4: whole-segment mmap (second page no re-fault)
// ---------------------------------------------------------------------------
void dt13_4_whole_segment_mmap(const std::string& heap_id) {
    std::cout << "[DT13-4] whole-segment mmap (second page no re-fault)..." << std::endl;
    int fds[2], rp[2];
    assert(pipe(fds) == 0);
    assert(pipe(rp) == 0);

    ar_set_heap_id(heap_id);
    ar_parent_init();
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        ::close(fds[1]);
        ::close(rp[0]);
        ar_child_init();

        ar_write_byte(rp[1], 'R');

        assert(ar_read_byte(fds[0]) == 'G');
        uint64_t addr_val = 0;
        ar_read_fd(fds[0], &addr_val, sizeof(addr_val));
        volatile uint8_t* p = reinterpret_cast<volatile uint8_t*>(addr_val);

        uint8_t v = p[0];
        assert(v == 0xA5);
        int after_first = uffd_data_attached_count();

        uint8_t v2 = p[4096];
        assert(v2 == 0xB5 && "second page data should be 0xB5");
        int after_second = uffd_data_attached_count();
        assert(after_second == after_first &&
               "second page access should NOT add a new uffd attach (whole-segment mmap)");

        ar_write_byte(rp[1], 'D');
        _exit(0);
    }

    ::close(fds[0]);
    ::close(rp[1]);

    assert(ar_read_byte(rp[0]) == 'R');

    // Allocate 8192 bytes (2 pages) to verify whole-segment mmap across
    // page boundaries. Fill page 0 with 0xA5, page 1 with 0xB5.
    void* p = uballoc::malloc(8192);
    assert(p);
    assert(uballoc::publish(p, 8192, TID_A) >= 0);
    std::memset(p, 0xA5, 4096);
    std::memset(static_cast<char*>(p) + 4096, 0xB5, 4096);

    ar_write_byte(fds[1], 'G');
    uint64_t addr_val = reinterpret_cast<uintptr_t>(p);
    ar_write_fd(fds[1], &addr_val, sizeof(addr_val));

    assert(ar_read_byte(rp[0]) == 'D');
    int st;
    waitpid(pid, &st, 0);
    check_child(st);
    ::close(fds[1]);
    ::close(rp[0]);

    uballoc::unpublish(p);
    uballoc::free(p);
    uballoc::reset();
    std::cout << "  OK: first page triggered uffd, second page no new attach (whole-segment mmap)" << std::endl;
}

// ---------------------------------------------------------------------------
// DT13-5 R6: seg_not_in_dir → uffd handler fires + uffd_poison called
// DT13-6 R6: (reserved) old kernel ZEROPAGE fallback (kernel < 6.6)
//
// P0 init but NO malloc (dir->count==0). P1 accesses P0's segment-0 VA
// → uffd handler classifies seg_not_in_dir → calls uffd_poison().
//
// NOTE: uffd_poison() uses LOG_ERROR which calls std::abort() after emitting
// the log line — the UFFDIO_POISON ioctl never executes. So this test
// verifies that the uffd handler fires, classifies seg_not_in_dir, and
// enters uffd_poison() (confirmed via stderr log capture). It does NOT
// verify the actual POISON ioctl / SIGBUS delivery — that path is dead
// code behind LOG_ERROR's abort. After abort, the handler thread dies and
// the faulting main thread blocks in kernel waiting for a uffd response
// that never comes; the parent sends SIGKILL to reap the child (~100ms).
// ---------------------------------------------------------------------------
void dt13_5_6_seg_not_in_dir(const std::string& heap_id) {
    std::cout << "[DT13-5/6] seg_not_in_dir -> uffd_poison (handler fires)..." << std::endl;

    // Pipe to capture child stderr (uffd_poison LOG_ERROR goes to stderr).
    int logpipe[2];
    assert(pipe(logpipe) == 0);

    ar_set_heap_id(heap_id);
    // P0 init but NO malloc — dir->count stays 0.
    ar_parent_init();

    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        // Redirect stderr → logpipe so parent can detect uffd_poison log.
        ::dup2(logpipe[1], STDERR_FILENO);
        ::close(logpipe[0]);
        ::close(logpipe[1]);

        ar_child_init();
        assert(uballoc::g_uffd_active.load(std::memory_order_acquire));

        // Disable SIGSEGV safety net to avoid handler loop / soft lockup
        // if any spurious signal is delivered. uffd itself stays active.
        ar_alloc().disable_fault_handler();
        assert(!uballoc::g_fh_active.load(std::memory_order_acquire));
        assert(uballoc::g_uffd_active.load(std::memory_order_acquire));

        // P0 (k=0) data-small segment-0 VA.
        uintptr_t va_base = uballoc::g_uffd_va_base.load(std::memory_order_acquire);
        uintptr_t p0_seg0 = va_base + uballoc::METADATA_STRIDE;

        // uffd handler: fi.s=0 >= dir_count=0 → seg_not_in_dir → uffd_poison.
        // uffd_poison emits LOG_ERROR (stderr) then aborts handler thread;
        // main thread blocks in kernel waiting for uffd response.
        volatile uint8_t* p = reinterpret_cast<volatile uint8_t*>(p0_seg0);
        uint8_t v = *p;
        (void)v;
        // Unreachable: handler aborts, main thread blocks → parent SIGKILL.
        _exit(0);
    }

    ::close(logpipe[1]);

    // Parent: poll child stderr for "uffd_poison" with 10s timeout.
    bool poison_seen = false;
    std::string acc;
    char buf[4096];
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        struct pollfd pfd{logpipe[0], POLLIN, 0};
        int pr = ::poll(&pfd, 1, 500);
        if (pr > 0 && (pfd.revents & POLLIN)) {
            ssize_t n = ::read(logpipe[0], buf, sizeof(buf));
            if (n <= 0) break;  // pipe closed
            acc.append(buf, buf + n);
            if (acc.find("uffd_poison") != std::string::npos) {
                poison_seen = true;
                break;
            }
        }
    }
    ::close(logpipe[0]);

    // Reap child: handler thread aborted, main thread blocks in kernel.
    // SIGKILL reaps in ~100ms (no fh → no soft lockup).
    ::kill(pid, SIGKILL);
    int child_st = 0;
    ::waitpid(pid, &child_st, 0);

    if (poison_seen) {
        std::cout << "  DT13-5 PASS: uffd handler fired + classified seg_not_in_dir"
                  << " + entered uffd_poison (log confirmed)" << std::endl;
        std::cout << "  DT13-6 SKIP: POISON ioctl not reached (LOG_ERROR aborts first)"
                  << std::endl;
    } else {
        std::cerr << "  FAIL: no uffd_poison log captured. stderr was:" << std::endl
                  << acc << std::endl;
        assert(0);
    }

    uballoc::reset();
}

// ---------------------------------------------------------------------------
// DT13-7 R7: disable_userfaultfd degrades to SIGSEGV
// ---------------------------------------------------------------------------
void dt13_7_disable_degrades_to_sigsegv(const std::string& heap_id) {
    std::cout << "[DT13-7] disable_userfaultfd degrades to SIGSEGV..." << std::endl;
    ar_set_heap_id(heap_id);
    ar_parent_init();

    // init() enables uffd + SIGSEGV safety net.
    assert(uballoc::g_uffd_active.load(std::memory_order_acquire));
    assert(uballoc::g_fh_active.load(std::memory_order_acquire));

    // disable_userfaultfd → uffd_uninstall → g_uffd_active=false,
    // also clears fh registry (g_fh_active=false).
    ar_alloc().disable_userfaultfd();
    assert(!uballoc::g_uffd_active.load(std::memory_order_acquire));
    assert(uballoc::g_uffd_fd == -1);
    assert(!uballoc::g_fh_active.load(std::memory_order_acquire));
    std::cout << "  after disable_userfaultfd: g_uffd_active=false, g_fh_active=false" << std::endl;

    // Re-enable SIGSEGV handler — degrades to SIGSEGV-only mode.
    bool ok = uballoc::enable_fault_handler();
    assert(ok);
    assert(uballoc::g_fh_active.load(std::memory_order_acquire));
    assert(!uballoc::g_uffd_active.load(std::memory_order_acquire));
    std::cout << "  after enable_fault_handler: g_fh_active=true, g_uffd_active=false (SIGSEGV mode)" << std::endl;

    // Verify malloc/publish/lookup still work in SIGSEGV-only mode.
    void* p = uballoc::malloc(64);
    assert(p);
    assert(uballoc::publish(p, 64, TID_A) >= 0);
    std::memset(p, 0xA5, 64);
    auto info = uballoc::lookup_by_type(TID_A);
    assert(info.address == p && info.size == 64);
    auto* b = static_cast<const uint8_t*>(info.address);
    for (int i = 0; i < 64; ++i) assert(b[i] == 0xA5);

    uballoc::unpublish(p);
    uballoc::free(p);
    std::cout << "  OK: disable_userfaultfd → enable_fault_handler, "
                 "malloc/publish/lookup work in SIGSEGV mode" << std::endl;

    uballoc::disable_fault_handler();
    uballoc::reset();
}

// ---------------------------------------------------------------------------
// DT13-8 R5: eager+uffd coexist (eager-attached segment has no uffd fault)
// ---------------------------------------------------------------------------
void dt13_8_eager_uffd_coexist(const std::string& heap_id) {
    std::cout << "[DT13-8] eager+uffd coexist (eager segment no uffd fault)..." << std::endl;
    int rp[2];
    assert(pipe(rp) == 0);

    ar_set_heap_id(heap_id);
    // P0 publishes BEFORE forking P1 → P1 init eager-attaches.
    ar_parent_init();
    void* p = uballoc::malloc(64);
    assert(p);
    assert(uballoc::publish(p, 64, TID_A) >= 0);
    std::memset(p, 0xA5, 64);

    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        ::close(rp[0]);
        ar_child_init();
        assert(uballoc::g_uffd_active.load(std::memory_order_acquire));

        int before = uffd_all_attached_count();

        // P0 already published before P1 init → P1's init() check_remote
        // eager-attaches P0's data segment. lookup_by_type returns the
        // eager-attached address (PTE already exists → no uffd fault).
        auto info = uballoc::lookup_by_type(TID_A);
        assert(info.address != nullptr);
        assert(info.size == 64);

        volatile uint8_t* p2 = static_cast<volatile uint8_t*>(info.address);
        uint8_t v = p2[0];  // no uffd fault (eager-attached)
        assert(v == 0xA5);
        for (int i = 0; i < 64; ++i) assert(p2[i] == 0xA5);

        // uffd handler never triggered → g_uffd_attached unchanged.
        int after = uffd_all_attached_count();
        assert(after == before && "eager-attached segment should NOT add uffd entry");

        ar_write_byte(rp[1], 'D');
        _exit(0);
    }

    ::close(rp[1]);

    int st;
    waitpid(pid, &st, 0);
    check_child(st);
    ::close(rp[0]);

    uballoc::unpublish(p);
    uballoc::free(p);
    uballoc::reset();
    std::cout << "  OK: eager-attached segment accessed with no uffd fault (g_uffd_attached unchanged)" << std::endl;
}

// ---------------------------------------------------------------------------
// DT13-9 R8: fork child soft_reset uninstall (no std::terminate)
// ---------------------------------------------------------------------------
void dt13_9_fork_child_uninstall(const std::string& heap_id) {
    std::cout << "[DT13-9] fork child soft_reset uninstall..." << std::endl;
    int rp[2];
    assert(pipe(rp) == 0);

    ar_set_heap_id(heap_id);
    // P0 init BEFORE fork — enables uffd (g_uffd_active=true) so the
    // child inherits it. The handler thread does NOT survive fork, which
    // is exactly the unexpected scenario we want to test.
    ar_parent_init();
    assert(uballoc::g_uffd_active.load(std::memory_order_acquire));

    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        ::close(rp[0]);
        // Child calls soft_reset() WITHOUT parent having done so first.
        // soft_reset → uffd_uninstall(false) → join() throws (thread
        // doesn't exist in child) → catch → detach → no std::terminate.
        ar_alloc().soft_reset();

        // If we reach here, no std::terminate occurred.
        assert(!uballoc::g_uffd_active.load(std::memory_order_acquire));
        assert(uballoc::g_uffd_fd == -1);

        ar_write_byte(rp[1], 'D');
        _exit(0);
    }

    ::close(rp[1]);

    int st;
    waitpid(pid, &st, 0);
    check_child(st);
    ::close(rp[0]);

    std::cout << "  OK: fork child soft_reset → uffd_uninstall(false), "
                 "join throws → detach, no std::terminate (g_uffd_active=false, fd=-1)" << std::endl;
    uballoc::reset();
}

}  // namespace

int main() {
    std::string id = ar_heap_id("ar13");

    if (!uffd_available()) {
        std::cout << "[AR-13] SKIP all DT13 cases: userfaultfd unavailable on"
                     " this host (vm.unprivileged_userfaultfd=0 or non-root)."
                  << std::endl;
        std::cout << "  Verified fallback: g_uffd_active=false,"
                     "g_fh_active=true (SIGSEGV handler active)."
                  << std::endl;
        std::cout << "\n=== AR-13 DT skipped (uffd unavailable,"
                     " fallback verified) ===" << std::endl;
        return 0;
    }

    ar_pre_case_cleanup(id);
    dt13_1_uffd_enable(id);
    ar_pre_case_cleanup(id);
    dt13_2_remote_fault_recovery(id);
    ar_pre_case_cleanup(id);
    dt13_3_multi_publish_recovery(id);
    ar_pre_case_cleanup(id);
    dt13_4_whole_segment_mmap(id);
    ar_pre_case_cleanup(id);
    dt13_5_6_seg_not_in_dir(id);
    ar_pre_case_cleanup(id);
    dt13_7_disable_degrades_to_sigsegv(id);
    ar_pre_case_cleanup(id);
    dt13_8_eager_uffd_coexist(id);
    ar_pre_case_cleanup(id);
    dt13_9_fork_child_uninstall(id);

    cleanup_all_distributed_shms(static_cast<int>(uballoc::MAX_PROCESSES), id);
    std::cout << "\n=== AR-13 DT passed ===" << std::endl;
    return 0;
}
