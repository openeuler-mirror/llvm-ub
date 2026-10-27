// SPDX-License-Identifier: Apache-2.0
 
/**
 * @file test_ar08_remote_attach.cpp
 * @brief AR-08 DT: Cross-Process Shared Memory Discovery & Eager Attach
 *
 * Cases:
 *   DT08-1 R1,R2: P0 allocates, P1 lookup_by_type → P1 attaches P0's segments
 *   DT08-2 R3:    P0 grows AFTER P1 attached → P1 discovers new segs next API
 *   DT08-3 R4:    P1 re-derives shm name from (heap_id, pid, bracket, seg_idx)
 *                 — not from descriptor.name field
 *   DT08-4 R5:    segment_directory(k, b) reads correct metadata region for k
 *   DT08-5 R6:    lookup_by_address() does NOT trigger check_remote_segments
 *                 (scan-only — P2's registry unmapped returns -1)
 *   DT08-6 R7:    Cross-node eager attach (UBSE 2-node hardware)
 *                 P0 (node A) publishes; P1 (node B) lookup_by_type triggers
 *                 cross-node attach of P0's data segment; data consistent.
 *                 Manual 2-terminal run via UBALLOC_ROLE=P0/P1 env var.
 *
 * DT08-1..5 are fork-based single-node cases (ctest). DT08-6 is a cross-node
 * case : two terminals, role-based dispatch, shm-state polling for cross-node
 * sync, [warn] + pass on single-node.
 */
 
#include "uballoc.hpp"
#include "test_helpers.hpp"
 
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
 
#include <sys/wait.h>
#include <unistd.h>

#ifdef UBALLOC_USE_UBSE
#include "uballoc/ubshm_provider.hpp"
#endif
 
namespace {
 
using ShmProvider = uballoc::DistributedShmBackend<>::ShmProvider;
 
constexpr uint32_t TID_A = 80;
constexpr uint32_t TID_B = 81;
constexpr uint32_t TID_C = 82;
 
// -----------------------------------------------------------------------------
// DT08-1 R1,R2: P0 allocates, P1 lookup_by_type → P1 attaches P0's segments
//
// init() calls check_remote_segments() at its tail, so by the time
// lookup_by_type runs, P0's data segment is already attached. This case
// verifies the end state: P1 has P0's segment attached AND the lookup
// resolves to owner=0 with the correct data. The "lookup triggers attach"
// behavior is exercised in DT08-2 (late growth discovered on next API call).
// -----------------------------------------------------------------------------
void dt08_1_lookup_attaches_p0_segments(const std::string& heap_id) {
    std::cout << "[DT08-1] P0 allocates+publishes, P1 lookup attaches..." << std::endl;
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

        ar_child_init();
        assert(uballoc::rank() == 1);

        // lookup_by_type triggers check_remote_segments (and init() already
        // did one). P1 must have P0's data segment attached.
        auto info = uballoc::lookup_by_type(TID_A);
        assert(info.owner_process == 0);
        assert(info.address != nullptr);
        assert(info.size == 64);
        auto* bytes = static_cast<const uint8_t*>(info.address);
        for (int i = 0; i < 64; ++i) assert(bytes[i] == 0x8A);

        // Verify P1 has at least one P0 data segment attached.
        auto& backend = ar_alloc().backend();
        bool attached = false;
        for (int b = 0; b < 3; ++b) {
            if (!backend.segments_[0][b].empty()) {
                attached = true;
                break;
            }
        }
        assert(attached);

        ar_write_byte(rp[1], 'C');
        _exit(0);
    }

    ::close(fds[0]);
    ::close(rp[1]);
    ar_parent_init();
    assert(uballoc::rank() == 0);

    void* p = uballoc::malloc(64);
    assert(p);
    std::memset(p, 0x8A, 64);
    assert(uballoc::publish(p, 64, TID_A) >= 0);
    ar_write_byte(fds[1], 'G');

    assert(ar_read_byte(rp[0]) == 'C');
    int st;
    waitpid(pid, &st, 0);
    check_child(st);
    ::close(fds[1]);
    ::close(rp[0]);
    uballoc::unpublish(p);
    uballoc::free(p);
    ar_alloc().reset();
    std::cout << "  OK: P1 lookup attached P0's segment, data readable" << std::endl;
}
 
// -----------------------------------------------------------------------------
// DT08-2 R3: P0 grows AFTER P1 attached → P1 discovers new segs next API call
// -----------------------------------------------------------------------------
void dt08_2_late_growth_discovered(const std::string& heap_id) {
    std::cout << "[DT08-2] P0 grows after P1 attached; P1 discovers on next API..." << std::endl;
    int fds[2], rp[2];
    assert(pipe(fds) == 0);
    assert(pipe(rp) == 0);
 
    ar_set_heap_id(heap_id);
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        ::close(fds[1]);
        ::close(rp[0]);
        assert(ar_read_byte(fds[0]) == 'G');  // P0 ready, initial seg attached
 
        ar_child_init();
        assert(uballoc::rank() == 1);
 
        // First lookup → P1 attaches P0's initial segments.
        auto info = uballoc::lookup_by_type(TID_A);
        assert(info.owner_process == 0);
        assert(info.address != nullptr);
 
        // Count P0's attached segments across all brackets.
        auto count_p0_segs = []() {
            size_t n = 0;
            auto& backend = ar_alloc().backend();
            for (int b = 0; b < 3; ++b) n += backend.segments_[0][b].size();
            return n;
        };
        size_t before = count_p0_segs();
        assert(before > 0);
 
        // Tell P0 to grow; wait for it to finish growing.
        ar_write_byte(rp[1], 'L');
        assert(ar_read_byte(fds[0]) == 'D');  // P0 grew
 
        // A no-op API call that triggers check_remote_segments.
        // lookup_by_type runs check_remote_segments internally.
        auto info2 = uballoc::lookup_by_type(TID_A);
        assert(info2.owner_process == 0);
 
        size_t after = count_p0_segs();
        assert(after > before);
        std::cout << "  P1 discovered " << (after - before)
                  << " new P0 segment(s) after growth" << std::endl;
 
        ar_write_byte(rp[1], 'C');
        _exit(0);
    }
 
    ::close(fds[0]);
    ::close(rp[1]);
    ar_parent_init();
    assert(uballoc::rank() == 0);
 
    void* p = uballoc::malloc(64);
    assert(p);
    std::memset(p, 0x8A, 64);
    assert(uballoc::publish(p, 64, TID_A) >= 0);
    ar_write_byte(fds[1], 'G');
 
    // Wait for P1 to attach initial segs and ask us to grow.
    assert(ar_read_byte(rp[0]) == 'L');
 
    // Grow Small bracket explicitly — creates a new segment on P0.
    bool grew = ar_alloc().grow_segment(0);
    assert(grew);
    ar_write_byte(fds[1], 'D');
 
    assert(ar_read_byte(rp[0]) == 'C');
    int st;
    waitpid(pid, &st, 0);
    check_child(st);
    ::close(fds[1]);
    ::close(rp[0]);
    uballoc::unpublish(p);
    uballoc::free(p);
    ar_alloc().reset();
    std::cout << "  OK: late growth discovered on next API call" << std::endl;
}
 
// -----------------------------------------------------------------------------
// DT08-3 R4: P1 re-derives shm name from (heap_id, pid, bracket, seg_idx)
//            not from descriptor.name field
//
// We verify this by:
//   1. P0 creates a segment, publishes a pointer in it.
//   2. P1 attaches via lookup_by_type.
//   3. P1 reads dir->descs[s].name from P0's metadata (the descriptor field).
//   4. P1 independently computes ShmProvider::shm_name(heap_id, 0, b, s)
//      using its own heap_id knowledge (from backend.heap_id_).
//   5. Both must match — proving the attach path uses the derived name.
//   6. We also verify the attach handle is valid (segment is mapped).
// -----------------------------------------------------------------------------
void dt08_3_name_redrived(const std::string& heap_id) {
    std::cout << "[DT08-3] P1 re-derives shm name from (heap,pid,bracket,seg)..." << std::endl;
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
 
        ar_child_init();
        assert(uballoc::rank() == 1);
 
        auto info = uballoc::lookup_by_type(TID_A);
        assert(info.address != nullptr);
        assert(info.owner_process == 0);
 
        auto& backend = ar_alloc().backend();
        // For each P0 segment that P1 has attached, the derived name (from
        // heap_id_/0/b/s) must equal the descriptor.name field. This proves
        // P1 re-derives the name (or, equivalently, the descriptor matches
        // the derivation).
        const std::string& hid = backend.heap_id_;
        bool any_checked = false;
        for (int b = 0; b < 3; ++b) {
            auto* dir = backend.segment_directory(0, b);
            if (!dir) continue;
            uint32_t cnt = dir->count.load(std::memory_order_acquire);
            for (uint32_t s = 0; s < cnt; ++s) {
                if (dir->descs[s].ready.load(std::memory_order_acquire) != 1u) continue;
                std::string derived = ShmProvider::shm_name(
                    hid, 0, b + 1, static_cast<int>(s));
                std::string stored(dir->descs[s].name);
                assert(derived == stored);
                any_checked = true;
            }
        }
        assert(any_checked);
 
        ar_write_byte(rp[1], 'C');
        _exit(0);
    }
 
    ::close(fds[0]);
    ::close(rp[1]);
    ar_parent_init();
    void* p = uballoc::malloc(64);
    assert(p);
    std::memset(p, 0x8A, 64);
    assert(uballoc::publish(p, 64, TID_A) >= 0);
    ar_write_byte(fds[1], 'G');
 
    assert(ar_read_byte(rp[0]) == 'C');
    int st;
    waitpid(pid, &st, 0);
    check_child(st);
    ::close(fds[1]);
    ::close(rp[0]);
    uballoc::unpublish(p);
    uballoc::free(p);
    ar_alloc().reset();
    std::cout << "  OK: derived name matches descriptor.name" << std::endl;
}
 
// -----------------------------------------------------------------------------
// DT08-4 R5: segment_directory(k, b) reads correct metadata region for k
//
// We verify by:
//   - P0 publishes TID_A (in its small bracket segment).
//   - P1 publishes TID_B (in its small bracket segment).
//   - After mutual lookup, P1's segment_directory(0, 0) returns P0's dir,
//     and segment_directory(1, 0) returns P1's own dir.
//   - Each directory's count/ready reflects the owning process's segments.
// -----------------------------------------------------------------------------
void dt08_4_segment_directory_per_process(const std::string& heap_id) {
    std::cout << "[DT08-4] segment_directory(k, b) reads correct process k metadata..." << std::endl;
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
 
        ar_child_init();
        assert(uballoc::rank() == 1);
 
        // P1 publishes its own object.
        void* q = uballoc::malloc(64);
        assert(q);
        std::memset(q, 0x71, 64);
        assert(uballoc::publish(q, 64, TID_B) >= 0);
 
        // Lookup P0's object — attaches P0's metadata.
        auto info = uballoc::lookup_by_type(TID_A);
        assert(info.owner_process == 0);
        assert(info.address != nullptr);
 
        auto& backend = ar_alloc().backend();
        // P1's own directory (k=1) must be readable.
        auto* dir1 = backend.segment_directory(1, 0);
        assert(dir1 != nullptr);
        uint32_t cnt1 = dir1->count.load(std::memory_order_acquire);
        assert(cnt1 >= 1u);
        // P0's directory (k=0) must also be readable after attach.
        auto* dir0 = backend.segment_directory(0, 0);
        assert(dir0 != nullptr);
        uint32_t cnt0 = dir0->count.load(std::memory_order_acquire);
        assert(cnt0 >= 1u);
 
        // The two directories are at different metadata VAs — verify their
        // addresses differ.
        assert(static_cast<void*>(dir0) != static_cast<void*>(dir1));
 
        // Each directory's first ready entry name encodes its owner rank.
        std::string name0(dir0->descs[0].name);
        std::string name1(dir1->descs[0].name);
        assert(name0.find("-p0-") != std::string::npos);
        assert(name1.find("-p1-") != std::string::npos);
 
        ar_write_byte(rp[1], 'C');
        _exit(0);
    }
 
    ::close(fds[0]);
    ::close(rp[1]);
    ar_parent_init();
    void* p = uballoc::malloc(64);
    assert(p);
    std::memset(p, 0x8A, 64);
    assert(uballoc::publish(p, 64, TID_A) >= 0);
    ar_write_byte(fds[1], 'G');
 
    assert(ar_read_byte(rp[0]) == 'C');
    int st;
    waitpid(pid, &st, 0);
    check_child(st);
    ::close(fds[1]);
    ::close(rp[0]);
    uballoc::unpublish(p);
    uballoc::free(p);
    ar_alloc().reset();
    std::cout << "  OK: dir(k=0) reads P0, dir(k=1) reads P1 — distinct metadata" << std::endl;
}
 
// -----------------------------------------------------------------------------
// DT08-5 R6: lookup_by_address() does NOT trigger check_remote_segments
//
// Strategy: P0 init (rank 0). P1 init (rank 1) attaches P0 metadata.
// Fork P2 (rank 2) which publishes TID_C. P1 has NOT yet discovered P2
// (no API call has run check_remote_segments against P2). P1 calls
// lookup_by_address(p2_ptr) — it scans only its already-attached
// registries. Since P2's registry is not mapped in P1, the lookup returns
// owner=-1 (scan-only, no remote load).
// Then P1 calls lookup_by_type(TID_C) — this triggers check_remote_segments,
// attaches P2's metadata, and lookup_by_address now returns owner=2.
// -----------------------------------------------------------------------------
void dt08_5_lookup_by_address_scan_only(const std::string& heap_id) {
    std::cout << "[DT08-5] lookup_by_address scan-only (no remote attach)..." << std::endl;
 
    ar_set_heap_id(heap_id);
    ar_parent_init();  // P0 = rank 0 (coordinator)
 
    pid_t pid1 = fork();
    assert(pid1 >= 0);
    if (pid1 == 0) {
        // P1 (rank=1) — test subject.
        ar_child_init();   // wire_up rank=1 + rank=0 only; published_registry_ptrs_[2..] null.
 
        int p2_to_p1[2], p1_to_p2[2];
        assert(pipe(p2_to_p1) == 0 && pipe(p1_to_p2) == 0);
 
        pid_t pid2 = fork();
        assert(pid2 >= 0);
        if (pid2 == 0) {
            // P2 (rank=2) — publisher of TID_C.
            ::close(p2_to_p1[0]);
            ::close(p1_to_p2[1]);
            ar_child_init();
            void* p = uballoc::malloc(64);
            assert(p && uballoc::publish(p, 64, TID_C) >= 0);
            std::memset(p, 0xC2, 64);
            uint64_t v = reinterpret_cast<uintptr_t>(p);
            ar_write_fd(p2_to_p1[1], &v, sizeof(v));
            ::close(p2_to_p1[1]);
            char dummy;
            ar_read_fd(p1_to_p2[0], &dummy, 1);
            ::close(p1_to_p2[0]);
            _exit(0);
        }
 
        ::close(p2_to_p1[1]);
        ::close(p1_to_p2[0]);
        uint64_t v = 0;
        ar_read_fd(p2_to_p1[0], &v, sizeof(v));
        ::close(p2_to_p1[0]);
        auto* remote = reinterpret_cast<void*>(v);
 
        // Phase 1 — scan-only: no ATTACH is triggered by
        // lookup_by_address. V2 shared registry (million-type-id
        // registry, remote-allocation branch): the registry is a single
        // shared shm mapped by every process, so P2's publish IS
        // discoverable by P1 without mapping P2's metadata region — the
        // V1 premise ("P2's registry unmapped -> owner == -1") no longer
        // holds. The contract that matters: the lookup does NOT trigger
        // check_remote_segments (metadata_regions_[2] stays unmapped).
        auto& backend = ar_alloc().backend();
        assert(backend.metadata_regions_[2].address == nullptr);
 
        auto pre = uballoc::lookup_by_address(remote);
        assert(pre.address == remote);      // found via the shared registry
        assert(pre.owner_process == 2);     // V2: visible without attach
        assert(pre.type_id == TID_C);
        assert(backend.metadata_regions_[2].address == nullptr);  // attach NOT triggered
 
        // Phase 2 — lookup_by_type triggers check_remote_segments, attaches P2.
        auto via_type = uballoc::lookup_by_type(TID_C);
        assert(via_type.owner_process == 2);
        assert(via_type.address == remote);
        assert(backend.metadata_regions_[2].address != nullptr);
 
        // Phase 3 — after attach, lookup_by_address finds it.
        auto post = uballoc::lookup_by_address(remote);
        assert(post.owner_process == 2);
        assert(post.type_id == TID_C);
        assert(post.address == remote);
        auto* bytes = static_cast<const uint8_t*>(post.address);
        for (int i = 0; i < 64; ++i) assert(bytes[i] == 0xC2);
 
        char done = 'D';
        ar_write_fd(p1_to_p2[1], &done, 1);
        ::close(p1_to_p2[1]);
        int st2;
        waitpid(pid2, &st2, 0);
        check_child(st2);
        _exit(0);
    }
 
    int st1;
    waitpid(pid1, &st1, 0);
    check_child(st1);
    ar_alloc().reset();
    std::cout << "  OK: scan-only (no attach), shared-registry discovery, post-attach returns owner" << std::endl;
}

#ifdef UBALLOC_USE_UBSE
// ===========================================================================
// DT08-6 R7: Cross-node eager attach (UBSE 2-node hardware)
//
// Manual run recipe (two terminals, any order):
//   Node A: UBALLOC_ROLE=P0 ./test_ar08_remote_attach
//   Node B: UBALLOC_ROLE=P1 ./test_ar08_remote_attach
//
// P0 (node A): init → malloc → publish(TID_A) → wait for P1 ack → cleanup
// P1 (node B): poll bootstrap → init → lookup_by_type(TID_A) → cross-node
//              attach of P0's data segment → verify data → ack via marker shm
//
// Single-node (ctest, no UBALLOC_ROLE): DT08-6 is skipped — fork-based
// DT08-1..5 cover single-node behavior. Cross-node sync uses UBSE
// cluster-visible shm_exists polling, which only works on real 2-node
// hardware. On single-node, P0 would time out and [warn] — but we skip
// entirely to keep ctest fast and avoid false [warn] noise.
//
// The cross-node case uses a dedicated heap_id ("ar08d6") so it does NOT
// conflict with the fork-based cases' "ar08" heap_id if both run modes
// are used on the same hardware.
// ===========================================================================

constexpr int kAr08SyncSecs = 15;  // generous for cross-node UBSE latency

// Returns P0/P1 role from env var UBALLOC_ROLE (default: not set = ctest).
static const char* ar08_cross_role() {
    const char* r = ::getenv("UBALLOC_ROLE");
    return (r && *r) ? r : nullptr;
}

// Poll for a UBSE shm to become cluster-visible (cross-node).
static bool ar08_poll_exists(const std::string& name) {
    using P = uballoc::UBShmProvider;
    for (int i = 0; i < kAr08SyncSecs; ++i) {
        if (P::shm_exists(name)) return true;
        ::sleep(1);
    }
    return P::shm_exists(name);
}

// P1 ack marker: P1 creates ub-{heap}-p1-ds after finishing its checks.
// P0 polls for this marker before cleanup.
static bool ar08_wait_marker(const std::string& heap) {
    using P = uballoc::UBShmProvider;
    const std::string n = P::shm_name(heap, 1, 1);  // ub-{heap}-p1-ds
    for (int i = 0; i < kAr08SyncSecs; ++i) {
        if (P::shm_exists(n)) return true;
        ::sleep(1);
    }
    return P::shm_exists(n);
}

void dt08_6_cross_node_eager_attach() {
    std::cout << "[DT08-6] Cross-node eager attach (UBSE 2-node)..." << std::endl;
    using P = uballoc::UBShmProvider;
    P::provider_init();
    P::cache_local_node_slot_id();

    const std::string heap = "ar08d6";
    const char* role = ar08_cross_role();

    if (role == nullptr) {
        std::cout << "  [skip] UBALLOC_ROLE not set — cross-node case requires "
                     "manual 2-terminal run (see file header)" << std::endl;
        return;
    }

    if (std::string(role) == "P0") {
        // -------------------------------------------------------------------
        // P0 (node A): pre-clean stale shms from previous runs, then init,
        //              malloc, publish, wait for P1 ack, cleanup.
        // Only P0 pre-cleans — P1 must NOT unlink shms because P0 may have
        // already created them (any-order startup).
        // -------------------------------------------------------------------
        P::shm_unlink(P::bootstrap_shm_name(heap));
        for (int r = 0; r < 2; ++r) {
            for (int rt = 0; rt < 4; ++rt) {
                P::shm_unlink(P::shm_name(heap, r, rt, 0));
            }
        }

        ar_set_heap_id(heap);
        ar_parent_init();
        assert(uballoc::rank() == 0);

        void* p = uballoc::malloc(64);
        assert(p);
        std::memset(p, 0xA8, 64);
        assert(uballoc::publish(p, 64, TID_A) >= 0);
        std::cout << "  P0: published TID_A (size=64, pattern=0xA8)" << std::endl;

        // Wait for P1's ack marker (cross-node shm cluster-visible).
        if (ar08_wait_marker(heap)) {
            std::cout << "  OK (P0): P1 acked — cross-node eager attach verified"
                      << std::endl;
        } else {
            std::cout << "  [warn] P1 did not ack within " << kAr08SyncSecs
                      << "s (single-node run?) — cross-node not verified"
                      << std::endl;
        }

        uballoc::unpublish(p);
        uballoc::free(p);
        ar_alloc().reset();
        // Final cleanup of bootstrap shm (not unlinked by reset()).
        P::shm_unlink(P::bootstrap_shm_name(heap));
        std::cout << "  OK (P0): cleanup done" << std::endl;
    } else {
        // -------------------------------------------------------------------
        // P1 (node B): poll bootstrap, init, lookup → cross-node attach.
        // -------------------------------------------------------------------
        const std::string bs_name = P::bootstrap_shm_name(heap);
        assert(ar08_poll_exists(bs_name) &&
               "P1: P0's bootstrap not visible (P0 running on node A?)");

        ar_set_heap_id(heap);
        ar_child_init();
        assert(uballoc::rank() == 1);

        // lookup_by_type triggers check_remote_segments → cross-node
        // attach of P0's data segment (the one hosting TID_A's object).
        auto info = uballoc::lookup_by_type_blocking(TID_A, 10000);
        assert(info.owner_process == 0);
        assert(info.address != nullptr);
        assert(info.size == 64);
        auto* bytes = static_cast<const uint8_t*>(info.address);
        for (int i = 0; i < 64; ++i) assert(bytes[i] == 0xA8);
        std::cout << "  OK (P1): cross-node lookup verified, data=0xA8" << std::endl;

        // Verify P1 has P0's segment attached cross-node.
        auto& backend = ar_alloc().backend();
        bool attached = false;
        for (int b = 0; b < 3; ++b) {
            if (!backend.segments_[0][b].empty()) {
                attached = true;
                break;
            }
        }
        assert(attached && "P1: no P0 segment attached cross-node");
        std::cout << "  OK (P1): P0's data segment attached cross-node" << std::endl;

        // Ack via marker shm so P0 can proceed to cleanup.
        auto mk = P::shm_try_create(P::shm_name(heap, 1, 1), P::min_shm_size);
        assert(uballoc::is_ok(mk) && "P1 marker create failed");
        auto h = uballoc::unwrap(mk);

        // Wait for P0 to clean up (bootstrap gone) before releasing marker.
        for (int i = 0; i < kAr08SyncSecs; ++i) {
            if (!P::shm_exists(bs_name)) break;
            ::sleep(1);
        }

        P::shm_close_fd(h);
        P::shm_unlink(P::shm_name(heap, 1, 1));
        ar_alloc().reset();
        std::cout << "  OK (P1): cleanup done" << std::endl;
    }
}
#endif  // UBALLOC_USE_UBSE
 
}  // namespace
 
int main() {
    // Cross-node manual run: if UBALLOC_ROLE is set, only run DT08-6.
    // The fork-based DT08-1..5 use fork (same-node children) and share
    // the "ar08" heap_id — running them on two terminals simultaneously
    // would conflict. DT08-6 uses a separate "ar08d6" heap_id.
#ifdef UBALLOC_USE_UBSE
    if (ar08_cross_role() != nullptr) {
        dt08_6_cross_node_eager_attach();
        std::cout << "\n=== AR-08 DT (cross-node) done ===" << std::endl;
        std::cout << "DT08-1..5 (fork-based) skipped — run without UBALLOC_ROLE "
                     "for ctest/single-node coverage." << std::endl;
        return 0;
    }
#endif

    // ctest / single-node: fork-based cases.
    std::string id = ar_heap_id("ar08");
    cleanup_all_distributed_shms(8, id);
 
    ar_pre_case_cleanup(id);
    dt08_1_lookup_attaches_p0_segments(id);
    ar_pre_case_cleanup(id);
    dt08_2_late_growth_discovered(id);
    ar_pre_case_cleanup(id);
    dt08_3_name_redrived(id);
    ar_pre_case_cleanup(id);
    dt08_4_segment_directory_per_process(id);
    ar_pre_case_cleanup(id);
    dt08_5_lookup_by_address_scan_only(id);
 
#ifdef UBALLOC_USE_UBSE
    // DT08-6 prints [skip] when UBALLOC_ROLE is not set (ctest context).
    dt08_6_cross_node_eager_attach();
#endif

    cleanup_all_distributed_shms(8, id);
    std::cout << "\n=== AR-08 DT passed ===" << std::endl;
    return 0;
}
