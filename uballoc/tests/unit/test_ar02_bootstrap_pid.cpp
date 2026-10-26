// SPDX-License-Identifier: Apache-2.0
 
/**
 * @file test_ar02_bootstrap_pid.cpp
 * @brief AR-02 DT: Bootstrap Block & PID Discovery
 *
 * Cases:
 *   DT02-1 R1,R2: P0 creates bootstrap, P1 joins — both see each other's PID
 *   DT02-2 R3:    kill -9 + restart — stale slot reclaimed via generation check
 *   DT02-3 R4:    is_single_node path: kill(pid, 0) stale-slot detection works
 *                 (POSIX-only; UBSE uses cross-node detection — skipped)
 *   DT02-4 R3:    generation counter increments correctly across restarts
 */
 
#include "uballoc.hpp"
#include "test_helpers.hpp"
 
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
 
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
 
namespace {
 
using ArBackend = uballoc::DistributedShmBackend<>;
 
// True if the test backend runs the same-node kill(pid, 0) detection path.
constexpr bool kIsSingleNode = ArBackend::ShmProvider::is_single_node;
 
// Read pid + generation of a bootstrap slot owned by rank `r`.
struct SlotInfo {
    int32_t pid = 0;
    uint32_t generation = 0;
    uint32_t ready = 0;
};
 
inline SlotInfo read_slot(const uballoc::BootstrapBlock* bb, int r) {
    SlotInfo s;
    if (!bb) return s;
    s.pid = bb->slots[r].pid.load(std::memory_order_acquire);
    s.generation = bb->slots[r].generation.load(std::memory_order_acquire);
    s.ready = bb->slots[r].ready.load(std::memory_order_acquire);
    return s;
}
 
// -----------------------------------------------------------------------------
// DT02-1 R1,R2: P0 creates bootstrap, P1 joins — both see each other's PID
// -----------------------------------------------------------------------------
void dt02_1_p0_p1_mutual_discovery(const std::string& heap_id) {
    std::cout << "[DT02-1] P0 creates bootstrap, P1 joins — mutual PID visibility..." << std::endl;
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
 
        ar_child_init();
        assert(uballoc::is_initialized());
        assert(uballoc::rank() == 1);
 
        // P1 sees its own pid in slot 1, and reads P0's pid from slot 0.
        auto* bb = ar_alloc().backend().bootstrap_block_;
        assert(bb != nullptr);
        SlotInfo s1 = read_slot(bb, 1);
        assert(s1.pid == static_cast<int32_t>(::getpid()));
        assert(s1.ready == 1u);
 
        SlotInfo s0 = read_slot(bb, 0);
        assert(s0.pid > 0);
        assert(s0.ready == 1u);
        assert(s0.pid != s1.pid);
 
        // Signal P0 that P1 has populated slot 1, then BLOCK until P0
        // finishes reading slot 1. This avoids the race where P1 exits
        // before P0 reads slot 1, leaving a stale/cleared slot.
        // fds[0] is still open in child (used earlier to read 'G').
        ar_write_byte(rp[1], 'C');
        assert(ar_read_byte(fds[0]) == 'A');  // P0 ack — safe to exit now
        _exit(0);
    }
 
    ::close(fds[0]);
    ::close(rp[1]);
    ar_parent_init();
    assert(uballoc::rank() == 0);
 
    // P0 sees its own pid in slot 0, and P1 will populate slot 1.
    auto* bb = ar_alloc().backend().bootstrap_block_;
    assert(bb != nullptr);
    SlotInfo s0 = read_slot(bb, 0);
    assert(s0.pid == static_cast<int32_t>(::getpid()));
    assert(s0.ready == 1u);
 
    ar_write_byte(fds[1], 'G');
    assert(ar_read_byte(rp[0]) == 'C');  // P1 populated slot 1
 
    // P1 is still alive (blocked on ACK). Read slot 1 NOW — it's a live
    // process's slot, not a dead-process residual.
    // attach_new_processes() runs on the next API call; trigger one.
    uballoc::malloc(64);
    SlotInfo s1 = read_slot(bb, 1);
    assert(s1.pid > 0);
    assert(s1.pid != s0.pid);
    assert(s1.ready == 1u);
 
    // ACK P1 so it can exit, then reap it.
    ar_write_byte(fds[1], 'A');
    int st;
    waitpid(pid, &st, 0);
    check_child(st);
    ::close(fds[1]);
    ::close(rp[0]);
    uballoc::free(uballoc::malloc(64));  // touch allocator before reset
    ar_alloc().reset();
    std::cout << "  OK: P0 pid in slot 0, P1 pid in slot 1, both ready" << std::endl;
}
 
// -----------------------------------------------------------------------------
// DT02-2 R3: kill -9 + restart — stale slot reclaimed via generation check
// -----------------------------------------------------------------------------
void dt02_2_kill9_reclaim(const std::string& heap_id) {
    std::cout << "[DT02-2] kill -9 + restart — stale slot reclaimed..." << std::endl;
    int fds[2], rp[2];
    assert(pipe(fds) == 0);
    assert(pipe(rp) == 0);

    ar_set_heap_id(heap_id);
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        ::close(fds[0]);
        ::close(rp[1]);
        // Wait for P0 to init first so the child attaches as rank 1.
        assert(ar_read_byte(rp[0]) == 'G');

        ar_child_init();
        assert(uballoc::rank() == 1);

        // Record our generation, then signal parent that we're ready to be killed.
        auto* bb = ar_alloc().backend().bootstrap_block_;
        uint32_t my_gen = bb->slots[1].generation.load(std::memory_order_acquire);
        ar_write_fd(fds[1], &my_gen, sizeof(my_gen));
        ar_write_byte(fds[1], 'R');

        // Wait for parent's "die now" signal. Hold the slot until then.
        char c = ar_read_byte(rp[0]);
        (void)c;
        _exit(0);  // parent SIGKILLs us before this runs
    }

    ::close(fds[1]);
    ::close(rp[0]);
    ar_parent_init();
    assert(uballoc::rank() == 0);
    ar_write_byte(rp[1], 'G');  // signal child to init

    // Wait for P1 to publish its generation.
    uint32_t p1_gen_before = 0;
    ar_read_fd(fds[0], &p1_gen_before, sizeof(p1_gen_before));
    assert(ar_read_byte(fds[0]) == 'R');

    auto* bb = ar_alloc().backend().bootstrap_block_;
    SlotInfo s1_before = read_slot(bb, 1);
    assert(s1_before.pid > 0);
    assert(s1_before.generation == p1_gen_before);
    assert(s1_before.ready == 1u);

    // SIGKILL P1 — no chance to clear its slot.
    int k = ::kill(pid, SIGKILL);
    assert(k == 0);
    int st;
    waitpid(pid, &st, 0);
    assert(WIFSIGNALED(st) && WTERMSIG(st) == SIGKILL);

    // The slot is stale: pid set, ready set, but pid is dead.
    // Fork a replacement process; claim_rank() should reclaim slot 1.
    // Two pipes: p2c (parent→child 'G'), c2p (child→parent 'D').
    int p2c[2], c2p[2];
    assert(pipe(p2c) == 0);
    assert(pipe(c2p) == 0);
    pid_t pid2 = fork();
    assert(pid2 >= 0);
    if (pid2 == 0) {
        ::close(fds[0]);
        ::close(rp[1]);
        ::close(p2c[1]);   // child doesn't write to p2c
        ::close(c2p[0]);    // child doesn't read from c2p
        // Wait for P0 to be ready.
        assert(ar_read_byte(p2c[0]) == 'G');

        ar_child_init();
        assert(uballoc::is_initialized());
        // Replacement claims the same rank (1) — stale slot was reclaimed.
        assert(uballoc::rank() == 1);

        auto* bb2 = ar_alloc().backend().bootstrap_block_;
        SlotInfo s1_after = read_slot(bb2, 1);
        assert(s1_after.pid == static_cast<int32_t>(::getpid()));
        assert(s1_after.generation > p1_gen_before);

        ar_write_byte(c2p[1], 'D');
        ::close(p2c[0]);
        ::close(c2p[1]);
        _exit(0);
    }

    ::close(fds[0]);
    ::close(rp[1]);
    ::close(p2c[0]);   // parent doesn't read from p2c
    ::close(c2p[1]);   // parent doesn't write to c2p
    ar_write_byte(p2c[1], 'G');  // signal replacement to init
    assert(ar_read_byte(c2p[0]) == 'D');

    int st2;
    waitpid(pid2, &st2, 0);
    check_child(st2);
    ::close(p2c[1]);
    ::close(c2p[0]);
    ar_alloc().reset();
    std::cout << "  OK: stale slot reclaimed, generation incremented" << std::endl;
}
 
// -----------------------------------------------------------------------------
// DT02-3 R4: is_single_node path: kill(pid, 0) stale-slot detection
// -----------------------------------------------------------------------------
void dt02_3_kill0_detection(const std::string& heap_id) {
    std::cout << "[DT02-3] is_single_node: kill(pid, 0) stale-slot detection..." << std::endl;
    if constexpr (!kIsSingleNode) {
        std::cout << "  [skip] UBSE (cross-node) — kill(pid,0) path not used" << std::endl;
        return;
    }

    int fds[2], rp[2];
    assert(pipe(fds) == 0);
    assert(pipe(rp) == 0);

    ar_set_heap_id(heap_id);
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        ::close(fds[0]);
        ::close(rp[1]);
        // Wait for P0 to init first.
        assert(ar_read_byte(rp[0]) == 'G');
        ar_child_init();
        assert(uballoc::rank() == 1);
        ar_write_byte(fds[1], 'R');
        // Block until parent kills us.
        for (;;) ::sleep(60);
        _exit(0);
    }

    ::close(fds[1]);
    ::close(rp[0]);
    ar_parent_init();
    ar_write_byte(rp[1], 'G');  // signal child to init
    assert(ar_read_byte(fds[0]) == 'R');

    auto* bb = ar_alloc().backend().bootstrap_block_;
    int32_t p1_pid = bb->slots[1].pid.load(std::memory_order_acquire);
    assert(p1_pid > 0);

    // Live peer: kill(pid, 0) returns 0 (process exists).
    int live = ::kill(p1_pid, 0);
    assert(live == 0);

    // Kill P1 — slot becomes stale.
    int k = ::kill(pid, SIGKILL);
    assert(k == 0);
    int st;
    waitpid(pid, &st, 0);
    assert(WIFSIGNALED(st));

    // Stale peer: kill(pid, 0) returns -1, errno == ESRCH.
    int dead = ::kill(p1_pid, 0);
    assert(dead == -1);
    assert(errno == ESRCH);

    // The next claim_rank will use this path to reclaim slot 1.
    // Two pipes: p2c (parent→child 'G'), c2p (child→parent 'D').
    int p2c[2], c2p[2];
    assert(pipe(p2c) == 0);
    assert(pipe(c2p) == 0);
    pid_t pid2 = fork();
    assert(pid2 >= 0);
    if (pid2 == 0) {
        ::close(fds[0]);
        ::close(rp[1]);
        ::close(p2c[1]);
        ::close(c2p[0]);
        assert(ar_read_byte(p2c[0]) == 'G');
        ar_child_init();
        assert(uballoc::rank() == 1);  // reclaimed via kill(pid,0) detection
        ar_write_byte(c2p[1], 'D');
        ::close(p2c[0]);
        ::close(c2p[1]);
        _exit(0);
    }
    ::close(fds[0]);
    ::close(rp[1]);
    ::close(p2c[0]);
    ::close(c2p[1]);
    ar_write_byte(p2c[1], 'G');  // signal replacement to init
    assert(ar_read_byte(c2p[0]) == 'D');
    int st2;
    waitpid(pid2, &st2, 0);
    check_child(st2);

    ::close(p2c[1]);
    ::close(c2p[0]);
    ar_alloc().reset();
    std::cout << "  OK: kill(pid,0) live→0, dead→-1/ESRCH, reclaim succeeds" << std::endl;
}
 
// -----------------------------------------------------------------------------
// DT02-4 R3: generation counter increments across restarts
// -----------------------------------------------------------------------------
void dt02_4_generation_increments(const std::string& heap_id) {
    std::cout << "[DT02-4] generation counter increments across restarts..." << std::endl;
    ar_set_heap_id(heap_id);
    ar_parent_init();

    auto* bb = ar_alloc().backend().bootstrap_block_;
    // Claim rank 1 twice (kill -9 between). Each iteration's gen must be >
    // the previous. Slot 0 is ours; track slot 1's generation across iterations.
    uint32_t prev_gen = 0;
    for (int iter = 0; iter < 2; ++iter) {
        int fds[2], rp[2];
        assert(pipe(fds) == 0);
        assert(pipe(rp) == 0);
        pid_t pid = fork();
        assert(pid >= 0);
        if (pid == 0) {
            ::close(fds[0]);
            ::close(rp[1]);
            // Wait for P0 to be ready.
            assert(ar_read_byte(rp[0]) == 'G');
            ar_child_init();
            assert(uballoc::rank() == 1);
            auto* bb2 = ar_alloc().backend().bootstrap_block_;
            uint32_t g = bb2->slots[1].generation.load(std::memory_order_acquire);
            ar_write_fd(fds[1], &g, sizeof(g));
            ar_write_byte(fds[1], 'R');
            for (;;) ::sleep(60);  // hold the slot
            _exit(0);
        }
        ::close(fds[1]);
        ::close(rp[0]);
        ar_write_byte(rp[1], 'G');  // signal child to init
        uint32_t g = 0;
        ar_read_fd(fds[0], &g, sizeof(g));
        assert(ar_read_byte(fds[0]) == 'R');
        assert(g > prev_gen);
        prev_gen = g;

        // SIGKILL P1; wait; then re-claim on the next iteration.
        assert(::kill(pid, SIGKILL) == 0);
        int st;
        waitpid(pid, &st, 0);
        assert(WIFSIGNALED(st));
        ::close(fds[0]);
        ::close(rp[1]);
    }
    (void)bb;
    ar_alloc().reset();
    std::cout << "  OK: generation incremented each restart (" << prev_gen << " total)" << std::endl;
}
 
}  // namespace
 
int main() {
    std::string id = ar_heap_id("ar02");
    cleanup_all_distributed_shms(8, id);

    // Use unique heap_id per case to avoid stale UBSE borrows from
    // _exit'd children (which skip destructors and leave device fds
    // briefly active on UBSE hardware).
    ar_pre_case_cleanup(id + "-1");
    dt02_1_p0_p1_mutual_discovery(id + "-1");
    ar_pre_case_cleanup(id + "-1");
    dt02_2_kill9_reclaim(id + "-2");
    ar_pre_case_cleanup(id + "-2");
    dt02_3_kill0_detection(id + "-3");
    ar_pre_case_cleanup(id + "-3");
    dt02_4_generation_increments(id + "-4");
    ar_pre_case_cleanup(id + "-4");

    // Final cleanup of all sub-heap IDs.
    for (const char* suffix : {"-1", "-2", "-3", "-4"}) {
        cleanup_all_distributed_shms(8, id + suffix);
    }
    std::cout << "\n=== AR-02 DT passed ===" << std::endl;
    return 0;
}
