// SPDX-License-Identifier: Apache-2.0

/**
 * @file test_ar01_va_layout.cpp
 * @brief AR-01 DT: VA Reservation & Layout
 *
 * Cases:
 *   DT01-1  R2       no env -> init is a no-op, no reservation at candidates
 *   DT01-2  R1,R3,R7 env -> ~28TB coverage in maps; fully released after reset()
 *   DT01-3  R3       malloc 64B / 1MB / 4MB land in distinct VA regions
 *   DT01-4  R6       MAP_NORESERVE: VmSize >= 28TB, VmRSS < 512MB
 *   DT01-5  R4       conflict fallback via early constructor + exec
 *   DT01-5b R5       fork-inherited VA adopted (full coverage persists)
 *   DT01-6  R8       global pointer contract — same VA dereferences in P1
 *
 */

#include "uballoc.hpp"
#include "test_helpers.hpp"

#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

// ---------------------------------------------------------------------------
// Early VA hog (constructor priority 100 — before uballoc's 101).
//
// Forces candidate[0] to be occupied when the exec'd child's constructor(101)
// runs, so DT01-5 can exercise the fallback path end-to-end. Gated by the
// AR01_HOG_CANDIDATE env var (hex base address); no-op when unset.
//
// ---------------------------------------------------------------------------
__attribute__((constructor(100)))
static void ar01_early_hog() {
    const char* spec = ::getenv("AR01_HOG_CANDIDATE");
    if (spec == nullptr || *spec == '\0') return;
    char* end = nullptr;
    unsigned long long base = ::strtoull(spec, &end, 16);
    if (end == spec || *end != '\0') return;  // parse error
    void* p = ::mmap(reinterpret_cast<void*>(static_cast<uintptr_t>(base)),
                    uballoc::VA_RESERVATION_SIZE, PROT_NONE,
                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE,
                    -1, 0);
    (void)p;  // intentionally leaked; the process exits shortly after
}

namespace {

// True if UBALLOC_HEAP_ID was already set at startup: constructor(101)
// reserves before main, so pristine-VA cases are skipped in that situation.
bool g_env_at_startup = false;

// Path of this test binary, for fork+exec self-invocation.
static std::string self_exe_path() {
    char buf[4096];
    ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    assert(n > 0);
    buf[n] = '\0';
    return std::string(buf);
}

// --child <heap_id>: UBALLOC_HEAP_ID + AR01_HOG_CANDIDATE are set in the
// exec environment. constructor(100) occupies candidate[0]; constructor(101)
// must fall back to the next candidate. init() then adopts that base.
//
// Exit codes:
//   0  = fallback + init + malloc all succeeded
//   10 = init failed (is_initialized() == false)
//   11 = malloc returned nullptr
//   12 = data mismatch
//   20 = hog did NOT occupy candidate[0] (MAP_FIXED_NOREPLACE unsupported,
//        or candidate already occupied by something else) → parent skips
static int child_mode(const char* heap_id) {
    (void)heap_id;  // UBALLOC_HEAP_ID already in the exec environment
    uballoc::init();
    if (!uballoc::is_initialized()) return 10;

    void* p = uballoc::malloc(64);
    if (!p) return 11;

    // Verify the constructor fell back: the malloc pointer must NOT lie in
    // candidate[0]'s range (which the early hog occupies). If it does, the
    // hog failed and no fallback was exercised.
    uintptr_t pv = reinterpret_cast<uintptr_t>(p);
    uintptr_t c0 = uballoc::kCandidateBases[0];
    if (pv >= c0 && pv < c0 + uballoc::VA_RESERVATION_SIZE) {
        uballoc::free(p);
        uballoc::reset();
        return 20;  // no fallback → skip
    }

    std::memset(p, 0xA5, 64);
    if (static_cast<uint8_t*>(p)[0] != 0xA5) return 12;
    uballoc::free(p);
    uballoc::reset();
    return 0;
}

// -------------------------------------------------------------------------
// DT01-1 R2: no env -> init is a no-op, no reservation at candidates
// -------------------------------------------------------------------------
void dt01_1_no_env() {
    std::cout << "[DT01-1] no env: init is a no-op, no reservation..." << std::endl;
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        if (g_env_at_startup) {
            std::cout << "  [skip] UBALLOC_HEAP_ID set at startup" << std::endl;
            _exit(0);
        }
        ar_unset_heap_id();
        uballoc::init();

        assert(!uballoc::is_initialized());
        assert(uballoc::rank() == 0);              // default before init
        assert(uballoc::total_processes() == 1);   // default before init。

        auto regions = ar_parse_maps();
        for (uintptr_t base : uballoc::kCandidateBases) {
            assert(!ar_maps_contains(regions, base));
        }
        std::cout << "  OK: uninitialized, no reservation at any candidate" << std::endl;
        _exit(0);
    }
    int st;
    waitpid(pid, &st, 0);
    check_child(st);
}

// -------------------------------------------------------------------------
// DT01-2 R1,R3,R7: env -> ~28TB coverage in maps; released after reset()
// -------------------------------------------------------------------------
void dt01_2_reserve_and_release(const std::string& heap_id) {
    std::cout << "[DT01-2] ~28TB coverage in maps; fully released after reset..." << std::endl;
    ar_pre_case_cleanup(heap_id);
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        if (g_env_at_startup) {
            std::cout << "  [skip] UBALLOC_HEAP_ID set at startup" << std::endl;
            _exit(0);
        }
        ar_set_heap_id(heap_id);
        uballoc::init();

        assert(uballoc::is_initialized());
        assert(uballoc::rank() >= 0);
        assert(uballoc::total_processes() >= 1);

        // Locate the ~28TB VA reservation from /proc/self/maps: a contiguous
        // span >= 1TB is unique to uballoc's reservation (stack/heap/vdso are
        // far smaller).
        auto regions = ar_parse_maps();
        uintptr_t va_base = ar_find_large_span_start(regions, 1ULL << 40);  // >= 1TB
        assert(va_base != 0);

        uint64_t covered = ar_maps_covered_bytes(
            regions, va_base, va_base + uballoc::VA_RESERVATION_SIZE);
        assert(covered == uballoc::VA_RESERVATION_SIZE);

        uballoc::reset();

        // Verify the FULL range is released — catches partial leaks across
        // the 28TB span.
        regions = ar_parse_maps();
        assert(ar_maps_covered_bytes(
            regions, va_base, va_base + uballoc::VA_RESERVATION_SIZE) == 0);

        std::cout << "  OK: ~28TB covered, fully released after reset" << std::endl;
        _exit(0);
    }
    int st;
    waitpid(pid, &st, 0);
    check_child(st);
}

// -------------------------------------------------------------------------
// DT01-3 R3: malloc 64B / 1MB / 4MB land in distinct VA regions
// -------------------------------------------------------------------------
void dt01_3_bracket_routing(const std::string& heap_id) {
    std::cout << "[DT01-3] malloc 64B / 1MB / 4MB -> distinct VA regions..." << std::endl;
    ar_pre_case_cleanup(heap_id);
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        ar_set_heap_id(heap_id);
        ar_child_init();

        // 64B -> Small, 1MB -> Large, 4MB -> Huge (Large rejects size >= 4MB).
        void* ps = uballoc::malloc(64);
        void* pl = uballoc::malloc(1 * 1024 * 1024);
        void* ph = uballoc::malloc(4 * 1024 * 1024);
        assert(ps && pl && ph);

        // Small/Large/Huge brackets are >= 8TB apart at rank 0, so >= 1GB
        // is a conservative separation threshold.
        auto diff = [](uintptr_t a, uintptr_t b) -> uint64_t {
            return (a > b) ? (a - b) : (b - a);
        };
        assert(diff((uintptr_t)ps, (uintptr_t)pl) >= (1ULL << 30));  // >= 1GB
        assert(diff((uintptr_t)pl, (uintptr_t)ph) >= (1ULL << 30));
        assert(diff((uintptr_t)ps, (uintptr_t)ph) >= (1ULL << 30));

        std::memset(ps, 0xA1, 64);
        std::memset(pl, 0xA2, 1 * 1024 * 1024);
        std::memset(ph, 0xA3, 4 * 1024 * 1024);
        assert(static_cast<uint8_t*>(ps)[0] == 0xA1);
        assert(static_cast<uint8_t*>(pl)[0] == 0xA2);
        assert(static_cast<uint8_t*>(ph)[0] == 0xA3);

        uballoc::free(ps);
        uballoc::free(pl);
        uballoc::free(ph);
        std::cout << "  OK: three allocations in distinct VA regions" << std::endl;
        _exit(0);
    }
    int st;
    waitpid(pid, &st, 0);
    check_child(st);
}

// -------------------------------------------------------------------------
// DT01-4 R6: reservation does not commit physical memory
// -------------------------------------------------------------------------
void dt01_4_no_reserve_oom(const std::string& heap_id) {
    std::cout << "[DT01-4] MAP_NORESERVE: VmSize >= 28TB, VmRSS < 512MB..." << std::endl;
    ar_pre_case_cleanup(heap_id);
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        ar_set_heap_id(heap_id);
        ar_child_init();

        long vm_size = ar_status_kB("VmSize:");
        long vm_rss = ar_status_kB("VmRSS:");
        assert(vm_size >= static_cast<long>(uballoc::VA_RESERVATION_SIZE / 1024));
        assert(vm_rss >= 0 && vm_rss < 512L * 1024);

        void* p = uballoc::malloc(1024 * 1024);
        assert(p);
        std::memset(p, 0xA5, 1024 * 1024);
        long vm_rss2 = ar_status_kB("VmRSS:");
        assert(vm_rss2 >= vm_rss && vm_rss2 < 512L * 1024);

        uballoc::free(p);
        std::cout << "  OK: VmSize=" << vm_size << "kB VmRSS=" << vm_rss2
                  << "kB (reservation not committed)" << std::endl;
        _exit(0);
    }
    int st;
    waitpid(pid, &st, 0);
    check_child(st);
}

// -------------------------------------------------------------------------
// DT01-5 R4: conflict fallback — early constructor occupies candidate[0]
// -------------------------------------------------------------------------
void dt01_5_conflict_fallback() {
    std::cout << "[DT01-5] conflict fallback via early constructor + exec..." << std::endl;
    if (g_env_at_startup) {
        std::cout << "  [skip] UBALLOC_HEAP_ID set at startup" << std::endl;
        return;
    }

    std::string exe = self_exe_path();
    std::string heap_id = ar_heap_id("ar01exec");
    ar_pre_case_cleanup(heap_id);

    // Fork + exec --child with both env vars set. The child's constructor(100)
    // will occupy candidate[0] (0x200000000000) before constructor(101) runs.
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        ::setenv("UBALLOC_HEAP_ID", heap_id.c_str(), 1);
        ::setenv("AR01_HOG_CANDIDATE", "0x200000000000", 1);
        ::execl(exe.c_str(), exe.c_str(), "--child", heap_id.c_str(), nullptr);
        _exit(127);
    }

    int st;
    ::waitpid(pid, &st, 0);

    // Exit 20 = hog didn't occupy candidate[0] (MAP_FIXED_NOREPLACE
    // unsupported, or candidate already occupied by PIE) → skip.
    if (WIFEXITED(st) && WEXITSTATUS(st) == 20) {
        std::cout << "  [skip] hog could not occupy candidate[0]" << std::endl;
        ar_pre_case_cleanup(heap_id);
        return;
    }

    // Exit 0 = fallback + init + malloc all succeeded.
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
        std::cerr << "  child failed: ";
        if (WIFSIGNALED(st)) std::cerr << "signal " << WTERMSIG(st);
        else std::cerr << "exit " << WEXITSTATUS(st);
        std::cerr << std::endl;
        assert(0);
    }

    ar_pre_case_cleanup(heap_id);
    std::cout << "  OK: constructor fell back to another candidate, init+malloc succeeded" << std::endl;
}

// -------------------------------------------------------------------------
// DT01-5b R5: fork-inherited VA adopted (full coverage persists)
// -------------------------------------------------------------------------
void dt01_5b_fork_inherited(const std::string& heap_id) {
    std::cout << "[DT01-5b] fork-inherited VA adopted..." << std::endl;
    if (g_env_at_startup) {
        std::cout << "  [skip] UBALLOC_HEAP_ID set at startup" << std::endl;
        return;
    }
    ar_pre_case_cleanup(heap_id);
    ar_set_heap_id(heap_id);

    ar_parent_init();

    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        ar_child_init();

        assert(uballoc::is_initialized());

        auto regions = ar_parse_maps();
        uintptr_t va_base = ar_find_large_span_start(regions, 1ULL << 40);
        assert(va_base != 0);
        uint64_t covered = ar_maps_covered_bytes(
            regions, va_base, va_base + uballoc::VA_RESERVATION_SIZE);
        assert(covered == uballoc::VA_RESERVATION_SIZE);

        void* p = uballoc::malloc(64);
        assert(p);
        std::memset(p, 0xB6, 64);
        assert(static_cast<uint8_t*>(p)[0] == 0xB6);
        uballoc::free(p);

        std::cout << "  OK: fork-inherited VA adopted, full coverage persists" << std::endl;
        _exit(0);
    }

    int st;
    waitpid(pid, &st, 0);
    check_child(st);

    ar_alloc().reset();
}

// -------------------------------------------------------------------------
// DT01-6 R8: global pointer contract — same VA dereferences in P1
// -------------------------------------------------------------------------
void dt01_6_global_pointer(const std::string& heap_id) {
    std::cout << "[DT01-6] P0 writes pattern, P1 dereferences the same VA..." << std::endl;
    ar_pre_case_cleanup(heap_id);
    int fds[2];
    assert(pipe(fds) == 0);

    ar_set_heap_id(heap_id);
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        ::close(fds[1]);
        uint64_t ptr_val = 0;
        ar_read_fd(fds[0], &ptr_val, sizeof(ptr_val));

        ar_child_init();  // init -> check_remote_segments -> attach P0's segments

        auto* remote = reinterpret_cast<volatile uint8_t*>(ptr_val);
        for (int i = 0; i < 64; ++i) {
            assert(remote[i] == 0xA5);
        }
        std::cout << "  OK: P1 read P0's data through the identical VA" << std::endl;
        _exit(0);
    }

    ::close(fds[0]);
    ar_parent_init();
    void* p = uballoc::malloc(64);
    assert(p);
    std::memset(p, 0xA5, 64);
    uint64_t ptr_val = reinterpret_cast<uintptr_t>(p);
    ar_write_fd(fds[1], &ptr_val, sizeof(ptr_val));

    int st;
    waitpid(pid, &st, 0);
    check_child(st);
    ::close(fds[1]);

    ar_alloc().reset();
}

}  // namespace

int main(int argc, char** argv) {
    // Exec-mode dispatch for DT01-5 (constructor(100)/(101) re-run on exec).
    if (argc >= 3) {
        std::string mode(argv[1]);
        if (mode == "--child") return child_mode(argv[2]);
    }

    g_env_at_startup = (::getenv("UBALLOC_HEAP_ID") != nullptr);
    std::string id = ar_heap_id("ar01");
    cleanup_all_distributed_shms(8, id);

    dt01_1_no_env();
    dt01_2_reserve_and_release(id);
    dt01_3_bracket_routing(id);
    dt01_4_no_reserve_oom(id);
    dt01_5_conflict_fallback();
    dt01_5b_fork_inherited(id);
    dt01_6_global_pointer(id);

    cleanup_all_distributed_shms(8, id);
    std::cout << "\n=== AR-01 DT passed ===" << std::endl;
    return 0;
}
