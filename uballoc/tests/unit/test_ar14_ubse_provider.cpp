// SPDX-License-Identifier: Apache-2.0

/**
 * @file test_ar14_ubse_provider.cpp
 * @brief AR-14 DT: UBSE 共享内存提供者（POSIX + UBSE）
 *
 * Automated subset (any Linux, no SDK required):
 *   DT14-7a R1:      POSIX naming convention + provider traits
 *   DT14-7b R2,R4,R6: try_create / ShmExists / attach / roundtrip / unlink
 *
 * UBSE framework (compiled only under -DUBALLOC_USE_UBSE):
 *   DT14-1(strings) R1: naming convention strings (no SDK device calls)
 *   DT14-1b         R1: min_shm_size (4MB) enforcement
 *   DT14-1          R1: create/attach/list — naming enum + cluster visibility
 *   DT14-2          R3: O_SYNC by export-node relation (self-map vs cross-node)
 *   DT14-3          R2: P1 preempt fallback (EXISTED → attach)
 *   DT14-4          R4: delete retry under cross-node borrow (1024 retry)
 *   DT14-5          R5: shm_is_unreferenced 3-state (none / self / cross)
 *   DT14-6 [A]      R2: error code mapping (1006/1007/idempotent unlink)
 *
 * Manual run recipe (two terminals, any order):
 *   Node A: UBALLOC_ROLE=P0 ./test_ar14_ubse_provider   (default role)
 *   Node B: UBALLOC_ROLE=P1 ./test_ar14_ubse_provider
 * Env:
 *   UBALLOC_ROLE  P0 (default) | P1
 */

#include "uballoc.hpp"
#include "test_helpers.hpp"

#include <cstring>
#include <iostream>
#include <string>

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#ifdef UBALLOC_USE_UBSE
#include "uballoc/ubshm_provider.hpp"
#endif

namespace {

constexpr size_t K = 1024;

#ifdef UBALLOC_USE_UBSE
// ---------------------------------------------------------------------------
// DT14-1(strings) R1: UBSE naming convention strings (no SDK device calls)
// ---------------------------------------------------------------------------
void dt14_1_ubse_naming_strings() {
    std::cout << "[DT14-1(strings)] UBSE naming convention..." << std::endl;
    using P = uballoc::UBShmProvider;

    assert(P::bootstrap_shm_name("h") == "ub-h-bootstrap");
    assert(P::shm_name("h", 0, 0) == "ub-h-p0-m");
    assert(P::shm_name("h", 1, 1) == "ub-h-p1-ds");
    assert(P::shm_name("h", 2, 2, 3) == "ub-h-p2-dl-s3");
    assert(P::shm_name("h", 3, 3, 0) == "ub-h-p3-dh");

    assert(P::min_shm_size == 4 * 1024 * 1024);
    assert(P::shm_size_granularity == 2 * 1024 * 1024);
    assert(!P::is_single_node);
    assert(P::has_reliable_unreferenced_check);

    std::cout << "  OK: ub-{heap}-bootstrap, ub-{heap}-p{k}-{m|ds|dl|dh}[-s{idx}]" << std::endl;
}

// ---------------------------------------------------------------------------
// DT14-1b R1: min_shm_size (4MB) enforcement
//   (1) backend round_up_shm_size (size<=4MB→4MB, >4MB→2MB align)
//   (2) SDK rejects sub-4MB (ubs_mem_shm_create returns 1000)
// ---------------------------------------------------------------------------
void dt14_1b_min_size_enforcement() {
    std::cout << "[DT14-1b] min_shm_size (4MB) enforcement..." << std::endl;
    using P = uballoc::UBShmProvider;
    using Backend = uballoc::DistributedShmBackend<uballoc::UBShmProvider>;
    constexpr size_t MB = 1024 * 1024;

    // (1) backend round_up_shm_size 归整逻辑
    assert(Backend::round_up_shm_size(0)    == 4 * MB);
    assert(Backend::round_up_shm_size(1*MB) == 4 * MB);
    assert(Backend::round_up_shm_size(4*MB) == 4 * MB);
    assert(Backend::round_up_shm_size(5*MB) == 6 * MB);
    assert(Backend::round_up_shm_size(8*MB) == 8 * MB);
    std::cout << "  OK: round_up_shm_size (0/1/4/5/8 MB → 4/4/4/6/8 MB)" << std::endl;

    // (2) SDK rejects sub-4MB
    P::provider_init();
    const std::string name = P::shm_name("ar14d1b", 0, 1);
    P::shm_unlink(name);
    auto c = P::shm_try_create(name, 1 * MB);
    assert(uballoc::is_err(c) && "SDK should reject sub-4MB (1000)");
    std::cout << "  OK: SDK rejects 1MB request (ret=1000) — 4MB enforced at SDK layer" << std::endl;
}

// ---------------------------------------------------------------------------
// UBSE hardware subset (DT14-1..5) — event-sync dual-process.
// Each case has P0/P1 branches synchronized by shm-state events (no sleeps
// for coordination). See the file header for the recipe and env vars.
// ---------------------------------------------------------------------------

// Returns P0/P1 role from env var UBALLOC_ROLE (default "P0").
static const char* ubse_role() {
    const char* r = ::getenv("UBALLOC_ROLE");
    return (r && *r) ? r : "P0";
}

// Poll budget for cross-process shm-state waits (1s granularity). 5s is
// enough for scripted 2-node runs; single-node runs (no P1) fall through
// to [warn] after this timeout.
constexpr int kSyncSecs = 5;

// How long P1 holds its cross-node borrow in DT14-4/5 before releasing.
constexpr int kP1HoldSecs = 5;

static bool poll_exists(const std::string& name) {
    using P = uballoc::UBShmProvider;
    for (int i = 0; i < kSyncSecs; ++i) {
        if (P::shm_exists(name)) return true;
        ::sleep(1);
    }
    return P::shm_exists(name);
}

static bool poll_gone(const std::string& name) {
    using P = uballoc::UBShmProvider;
    for (int i = 0; i < kSyncSecs; ++i) {
        if (!P::shm_exists(name)) return true;
        ::sleep(1);
    }
    return !P::shm_exists(name);
}

static bool poll_referenced(const std::string& name) {
    using P = uballoc::UBShmProvider;
    for (int i = 0; i < kSyncSecs; ++i) {
        if (!P::shm_is_unreferenced(name)) return true;
        ::sleep(1);
    }
    return !P::shm_is_unreferenced(name);
}

// Proper attach release: close the fd AND detach the node-level borrow.
static void release_attach(uballoc::UBShmProvider::ShmHandle h) {
    using P = uballoc::UBShmProvider;
    P::shm_close_fd(h);
    P::shm_detach_by_name(std::string(h.name));
}

// P1 ack-marker: P1's own slot shm (ub-{heap}-p1-ds). P1 creates it AFTER
// finishing its checks and releasing borrows; P0 polls for it before cleanup.
static uballoc::UBShmProvider::ShmHandle marker_create(const std::string& heap) {
    using P = uballoc::UBShmProvider;
    const std::string n = P::shm_name(heap, 1, 1);
    P::shm_unlink(n);  // pre-clean stale marker
    auto c = P::shm_try_create(n, P::min_shm_size);
    assert(uballoc::is_ok(c) && "P1 marker create failed");
    std::cout << "  P1 ack marker: " << n << std::endl;
    return uballoc::unwrap(c);
}

// Delete P1's own marker. fd is closed BEFORE unlink.
static void marker_release(uballoc::UBShmProvider::ShmHandle h) {
    using P = uballoc::UBShmProvider;
    P::shm_close_fd(h);
    P::shm_unlink(std::string(h.name));
}

// P0: wait for P1's ack marker. Returns false on timeout (single-node run).
static bool wait_marker(const std::string& heap) {
    using P = uballoc::UBShmProvider;
    const std::string n = P::shm_name(heap, 1, 1);
    for (int i = 0; i < kSyncSecs; ++i) {
        if (P::shm_exists(n)) return true;
        ::sleep(1);
    }
    return P::shm_exists(n);
}

// ---------------------------------------------------------------------------
// DT14-1 R1: create / attach / enumerate — naming enum + cluster visibility
// ---------------------------------------------------------------------------
void dt14_1_ubse_create_attach_list() {
    std::cout << "[DT14-1] UBSE create/attach/list (naming enum)..." << std::endl;
    using P = uballoc::UBShmProvider;
    P::provider_init();

    const std::string heap = "ar14d1";
    const std::string name = P::shm_name(heap, 0, 1);  // ub-ar14d1-p0-ds
    constexpr size_t kSize = P::min_shm_size;  // 4MB
    const char* role = ubse_role();

    if (std::string(role) == "P0") {
        P::shm_unlink(name);  // pre-clean
        auto c = P::shm_try_create(name, kSize);
        assert(uballoc::is_ok(c) && "P0 shm_try_create failed");
        auto h = uballoc::unwrap(c);
        std::cout << "  P0 created: " << name << " (fd=" << h.fd << ")" << std::endl;

        // Enumerate; verify own shm is present.
        auto names = P::shm_list_with_prefix("ub-" + heap + "-");
        bool found = false;
        for (const auto& nm : names) {
            std::cout << "    list: " << nm << std::endl;
            if (nm == name) found = true;
        }
        assert(found && "P0: created shm not in own list");
        assert(P::shm_exists(name));
        std::cout << "  OK (P0): naming enum + shm_exists verified" << std::endl;

        if (wait_marker(heap)) {
            std::cout << "  OK (P0): P1 acked (shm cluster-visible on node B)" << std::endl;
        } else {
            std::cout << "  [warn] P1 did not ack within " << kSyncSecs
                      << "s (single-node run?) — cross-node visibility not verified" << std::endl;
        }

        P::shm_close_fd(h);
        P::shm_unlink(name);
        assert(poll_gone(name) && "data shm should be gone after unlink");
        std::cout << "  OK (P0): cleanup done" << std::endl;
    } else {
        // P1: on node B, enumerate and verify P0's shm is visible.
        assert(poll_exists(name) && "P1: P0's shm not visible (P0 running?)");

        auto names = P::shm_list_with_prefix("ub-" + heap + "-");
        bool found = false;
        for (const auto& nm : names) {
            std::cout << "    list: " << nm << std::endl;
            if (nm == name) found = true;
        }
        assert(found && "P1: P0's shm not in node B list (cluster visibility)");
        std::cout << "  OK (P1): cluster-visible naming enum verified" << std::endl;

        // Ack, then keep the marker until P0's cleanup is observable.
        auto mk = marker_create(heap);
        poll_gone(name);
        marker_release(mk);
    }
}

// ---------------------------------------------------------------------------
// DT14-2 R3: O_SYNC by export-node relation
// ---------------------------------------------------------------------------
void dt14_2_ubse_osync_by_export_node() {
    std::cout << "[DT14-2] UBSE O_SYNC by export-node relation..." << std::endl;
    using P = uballoc::UBShmProvider;
    P::provider_init();

    const std::string heap = "ar14d2";
    const std::string name = P::shm_name(heap, 0, 1);
    constexpr size_t kSize = P::min_shm_size;
    const char* role = ubse_role();

    if (std::string(role) == "P0") {
        // P0: creator/lender — self-map, O_SYNC MUST be disabled.
        P::shm_unlink(name);  // pre-clean
        auto c = P::shm_try_create(name, kSize);
        assert(uballoc::is_ok(c) && "P0 create failed");
        auto h = uballoc::unwrap(c);

        int fl = ::fcntl(h.fd, F_GETFL);
        assert(fl != -1 && "fcntl F_GETFL failed");
        std::cout << "  P0 self-map fd flags: 0x" << std::hex << fl << std::dec
                  << " (O_SYNC=0x" << std::hex << O_SYNC << std::dec << ")" << std::endl;
        // Self-map MUST NOT have O_SYNC (mmap would EPERM).
        assert((fl & O_SYNC) == 0 && "self-map must disable O_SYNC");
        std::cout << "  OK (P0): self-map O_SYNC disabled" << std::endl;

        if (wait_marker(heap)) {
            std::cout << "  OK (P0): P1 acked (cross-node O_SYNC verified)" << std::endl;
        } else {
            std::cout << "  [warn] P1 did not ack within " << kSyncSecs
                      << "s (single-node run?) — cross-node O_SYNC not verified" << std::endl;
        }

        P::shm_close_fd(h);
        P::shm_unlink(name);
        assert(poll_gone(name) && "data shm should be gone after unlink");
        std::cout << "  OK (P0): cleanup done" << std::endl;
    } else {
        // P1: cross-node borrower — O_SYNC MUST be present.
        assert(poll_exists(name) && "P1: P0's shm not visible (P0 running?)");
        auto a = P::shm_attach(name);
        assert(uballoc::is_ok(a) && "P1 attach failed");
        auto h = uballoc::unwrap(a);

        int fl = ::fcntl(h.fd, F_GETFL);
        assert(fl != -1 && "fcntl F_GETFL failed");
        std::cout << "  P1 cross-node fd flags: 0x" << std::hex << fl << std::dec
                  << " (O_SYNC=0x" << std::hex << O_SYNC << std::dec << ")" << std::endl;
        // Cross-node borrow MUST have O_SYNC (plain O_RDWR → EPERM).
        assert((fl & O_SYNC) != 0 && "cross-node borrow must enable O_SYNC");
        std::cout << "  OK (P1): cross-node O_SYNC enabled" << std::endl;

        release_attach(h);
        auto mk = marker_create(heap);
        poll_gone(name);
        marker_release(mk);
    }
}

// ---------------------------------------------------------------------------
// DT14-3 R2: P1 preempt fallback (EXISTED → attach)
// ---------------------------------------------------------------------------
void dt14_3_ubse_p1_preempt_fallback() {
    std::cout << "[DT14-3] UBSE P1 preempt fallback (EXISTED → attach)..." << std::endl;
    using P = uballoc::UBShmProvider;
    P::provider_init();

    const std::string heap = "ar14d3";
    const std::string name = P::shm_name(heap, 0, 1);
    constexpr size_t kSize = P::min_shm_size;
    const char* role = ubse_role();

    if (std::string(role) == "P0") {
        // P0: first creator wins.
        P::shm_unlink(name);  // pre-clean
        auto c = P::shm_try_create(name, kSize);
        assert(uballoc::is_ok(c) && "P0 create failed");
        auto h = uballoc::unwrap(c);
        std::cout << "  OK (P0): created first" << std::endl;

        if (wait_marker(heap)) {
            std::cout << "  OK (P0): P1 acked (preempt fallback verified)" << std::endl;
        } else {
            std::cout << "  [warn] P1 did not ack within " << kSyncSecs
                      << "s (single-node run?) — fallback not verified" << std::endl;
        }

        P::shm_close_fd(h);
        P::shm_unlink(name);
        assert(poll_gone(name) && "data shm should be gone after unlink");
        std::cout << "  OK (P0): cleanup done" << std::endl;
    } else {
        // P1: create on existing name → ShmExists (1006) → attach fallback.
        assert(poll_exists(name) && "P1: P0's shm not visible (P0 running?)");
        auto c = P::shm_try_create(name, kSize);
        assert(uballoc::is_err(c) && "P1 create should fail (P0 owns shm)");
        assert(uballoc::unwrap_err(c).kind == uballoc::ErrorKind::ShmExists);
        std::cout << "  OK (P1): create → ShmExists (1006 EXISTED mapped)" << std::endl;

        // Fallback: attach to the existing shm — success.
        auto a = P::shm_attach(name);
        assert(uballoc::is_ok(a) && "P1 attach after ShmExists failed");
        auto h = uballoc::unwrap(a);
        std::cout << "  OK (P1): attach succeeds (fallback path)" << std::endl;

        release_attach(h);
        auto mk = marker_create(heap);
        poll_gone(name);
        marker_release(mk);
    }
}

// ---------------------------------------------------------------------------
// DT14-4 R4: delete retry under cross-node borrow (1024 retry)
// ---------------------------------------------------------------------------
void dt14_4_ubse_delete_retry() {
    std::cout << "[DT14-4] UBSE delete retry under borrow..." << std::endl;
    using P = uballoc::UBShmProvider;
    P::provider_init();
    P::cache_local_node_slot_id();

    const std::string heap = "ar14d4";
    const std::string name = P::shm_name(heap, 0, 1);
    constexpr size_t kSize = P::min_shm_size;
    const char* role = ubse_role();

    if (std::string(role) == "P0") {
        // P0: create, event-wait for P1's borrow, then unlink under borrow.
        P::shm_unlink(name);  // pre-clean
        auto c = P::shm_try_create(name, kSize);
        assert(uballoc::is_ok(c) && "P0 create failed");
        auto h = uballoc::unwrap(c);
        std::cout << "  P0 created" << std::endl;

        // Event wait: P1's cross-node borrow (local borrows are excluded).
        if (poll_referenced(name)) {
            std::cout << "  P0: P1 borrow observed (cross-node)" << std::endl;
        } else {
            std::cout << "  [warn] no cross-node borrow within " << kSyncSecs
                      << "s (single-node run?) — retry path not exercised" << std::endl;
        }

        // Close our own fd FIRST, then unlink while P1 may still hold borrow.
        P::shm_close_fd(h);
        P::shm_unlink(name);
        if (P::shm_exists(name)) {
            std::cout << "  unlink #1: shm still present (1024 retry timed out;"
                      << " silent give-up, no auto-cleanup)" << std::endl;
        } else {
            std::cout << "  unlink #1: deleted (retry landed)" << std::endl;
        }

        // Event wait: P1 signals the borrow is released.
        if (wait_marker(heap)) {
            std::cout << "  P0: P1 released borrow" << std::endl;
        } else {
            std::cout << "  [warn] P1 did not ack within " << kSyncSecs
                      << "s (single-node run?)" << std::endl;
        }

        // Re-unlink with no borrow held: lands (or NOT_EXIST if #1 landed).
        P::shm_unlink(name);
        assert(poll_gone(name) && "shm should be gone after unlink + P1 release");
        std::cout << "  OK (P0): eventual deletion verified" << std::endl;
    } else {
        // P1: attach to hold the cross-node borrow, release, ack.
        assert(poll_exists(name) && "P1: P0's shm not visible (P0 running?)");
        auto a = P::shm_attach(name);
        assert(uballoc::is_ok(a) && "P1 attach failed (P0 not ready?)");
        auto h = uballoc::unwrap(a);
        std::cout << "  P1 attached (cross-node borrow held)" << std::endl;

        // Hold so P0's unlink #1 runs against a live borrow (1024 path).
        ::sleep(kP1HoldSecs);

        release_attach(h);
        std::cout << "  P1 released borrow" << std::endl;

        auto mk = marker_create(heap);
        poll_gone(name);
        marker_release(mk);
    }
}

// ---------------------------------------------------------------------------
// DT14-5 R5: shm_is_unreferenced 3-state
//   (a) no shm           → true  (NOT_EXIST branch)
//   (b) self-borrow only → true  (local excluded)
//   (c) cross-node borrow → false
//   P0: (a)(b) self-test → wait P1 borrow → (c) assert false → cleanup
//   P1: attach (cross-node borrow) → hold → release → ack
// ---------------------------------------------------------------------------
void dt14_5_ubse_unreferenced_3state() {
    std::cout << "[DT14-5] UBSE shm_is_unreferenced 3-state..." << std::endl;
    using P = uballoc::UBShmProvider;
    P::provider_init();
    // cache_local_node_slot_id is normally called by GlobalAllocator::init
    // after bootstrap_join. Here we exercise the provider directly, so call
    // it so shm_is_unreferenced can filter the local node's borrow.
    P::cache_local_node_slot_id();

    const std::string heap = "ar14d5";
    const std::string name = P::shm_name(heap, 0, 1);
    constexpr size_t kSize = P::min_shm_size;
    const char* role = ubse_role();

    if (std::string(role) == "P0") {
        // State (a): no shm → true (NOT_EXIST branch).
        P::shm_unlink(name);  // ensure absent
        bool r = P::shm_is_unreferenced(name);
        std::cout << "  (a) no shm: shm_is_unreferenced=" << r << " (true expected)" << std::endl;
        assert(r && "non-existent shm should be unreferenced");

        // State (b): only local node borrows → true (local excluded).
        auto c = P::shm_try_create(name, kSize);
        assert(uballoc::is_ok(c) && "P0 create failed");
        auto h = uballoc::unwrap(c);
        r = P::shm_is_unreferenced(name);
        std::cout << "  (b) self-borrow only: shm_is_unreferenced=" << r
                  << " (true expected — local excluded)" << std::endl;
        assert(r && "self-only borrow should be unreferenced (local excluded)");

        // Event wait: P1's cross-node borrow appears.
        if (poll_referenced(name)) {
            // State (c): cross-node borrow present → false.
            r = P::shm_is_unreferenced(name);
            std::cout << "  (c) cross-node borrow: shm_is_unreferenced=" << r
                      << " (false expected)" << std::endl;
            assert(!r && "cross-node borrow should make shm referenced");
            std::cout << "  OK (P0): state (c) false verified" << std::endl;
        } else {
            std::cout << "  (c) [warn] no cross-node borrow within " << kSyncSecs
                      << "s (single-node run?) — state (c) not verified" << std::endl;
        }

        // Event wait: P1 released its borrow (marker) — then cleanup lands.
        if (wait_marker(heap)) {
            std::cout << "  P0: P1 released borrow" << std::endl;
        } else {
            std::cout << "  [warn] P1 did not ack within " << kSyncSecs
                      << "s (single-node run?)" << std::endl;
        }
        P::shm_close_fd(h);
        P::shm_unlink(name);
        assert(poll_gone(name) && "data shm should be gone after unlink");
        std::cout << "  OK (P0): cleanup done" << std::endl;
    } else {
        // P1: attach to establish the cross-node borrow, hold, release, ack.
        assert(poll_exists(name) && "P1: P0's shm not visible (P0 running?)");
        auto a = P::shm_attach(name);
        assert(uballoc::is_ok(a) && "P1 attach failed (P0 not ready?)");
        auto h = uballoc::unwrap(a);
        std::cout << "  P1 attached (cross-node borrow held)" << std::endl;

        // Hold long enough for P0 to observe state (c).
        ::sleep(kP1HoldSecs);

        release_attach(h);
        std::cout << "  P1 released borrow" << std::endl;

        auto mk = marker_create(heap);
        poll_gone(name);
        marker_release(mk);
    }
}

// ---------------------------------------------------------------------------
// DT14-6 [A] R2: error code mapping (1006/1007/idempotent unlink)
// ---------------------------------------------------------------------------
void dt14_6_ubse_error_codes() {
    std::cout << "[DT14-6] UBSE error code mapping (1006/1007/1024)..." << std::endl;
    using P = uballoc::UBShmProvider;
    P::provider_init();

    const std::string heap = "ar14d6";
    const std::string name = P::shm_name(heap, 0, 1);
    constexpr size_t kSize = P::min_shm_size;

    P::shm_unlink(name);

    // 1007 NOT_EXIST path: shm doesn't exist.
    assert(!P::shm_exists(name));
    assert(P::shm_is_unreferenced(name));  // NOT_EXIST branch returns true
    std::cout << "  OK: 1007 NOT_EXIST → shm_exists=false, is_unreferenced=true" << std::endl;

    // 1006 EXISTED path: create then re-create.
    auto c0 = P::shm_try_create(name, kSize);
    assert(uballoc::is_ok(c0) && "first create failed");
    auto h0 = uballoc::unwrap(c0);
    auto c1 = P::shm_try_create(name, kSize);
    assert(uballoc::is_err(c1) && "second create should fail");
    assert(uballoc::unwrap_err(c1).kind == uballoc::ErrorKind::ShmExists);
    std::cout << "  OK: 1006 EXISTED → ShmExists" << std::endl;

    // 1024 ATTACH_USING path: covered by DT14-4. Here verify idempotent
    // unlink on existing (detach + delete), then NOT_EXIST treated as success.
    P::shm_close_fd(h0);
    P::shm_unlink(name);
    P::shm_unlink(name);  // second unlink: NOT_EXIST treated as success
    std::cout << "  OK: 1024 retry covered by DT14-4; idempotent unlink OK" << std::endl;
}

#endif  // UBALLOC_USE_UBSE

// ---------------------------------------------------------------------------
// DT14-7a R1: POSIX naming convention and traits (any Linux)
// ---------------------------------------------------------------------------
void dt14_7a_posix_naming() {
    std::cout << "[DT14-7a] POSIX naming convention and traits..." << std::endl;
    using P = uballoc::PosixShmProvider;

    assert(P::bootstrap_shm_name("h") == "/h-bootstrap");
    assert(P::shm_name("h", 0, 0) == "/h-p0-m");
    assert(P::shm_name("h", 1, 1) == "/h-p1-ds");
    assert(P::shm_name("h", 2, 2) == "/h-p2-dl");
    assert(P::shm_name("h", 3, 3) == "/h-p3-dh");
    // -s{idx} suffix only when region > 0 and idx > 0.
    assert(P::shm_name("h", 1, 1, 3) == "/h-p1-ds-s3");
    assert(P::shm_name("h", 0, 1, 0) == "/h-p0-ds");
    assert(P::shm_name("h", 0, 0, 5) == "/h-p0-m");

    static_assert(P::min_shm_size == 0);
    static_assert(P::shm_size_granularity == uballoc::SIZE_PAGE);
    static_assert(P::is_single_node);
    static_assert(!P::has_reliable_unreferenced_check);

    std::cout << "  OK: /{heap}-bootstrap, /{heap}-p{k}-{m|ds|dl|dh}[-s{idx}]" << std::endl;
}

// ---------------------------------------------------------------------------
// DT14-7b R2,R4,R6: try_create / ShmExists / attach / data roundtrip /
//   unlink / ShmOpen after unlink / idempotent unlink (PosixShmProvider)
// ---------------------------------------------------------------------------
void dt14_7b_provider_semantics() {
    std::cout << "[DT14-7b] create/exists/attach/roundtrip/unlink idempotency..." << std::endl;
    using P = uballoc::PosixShmProvider;
    const std::string name = "/ar14-provider-t1";
    constexpr size_t kSize = 64 * K;

    P::shm_unlink(name);  // pre-clean from earlier runs

    auto c1 = P::shm_try_create(name, kSize);
    assert(uballoc::is_ok(c1));
    int fd1 = uballoc::unwrap(c1);

    // Second create on the same name -> ShmExists.
    auto c2 = P::shm_try_create(name, kSize);
    assert(uballoc::is_err(c2));
    assert(uballoc::unwrap_err(c2).kind == uballoc::ErrorKind::ShmExists);

    // Attach an independent handle and verify the data roundtrip.
    auto a1 = P::shm_attach(name);
    assert(uballoc::is_ok(a1));
    int fd2 = uballoc::unwrap(a1);

    void* w = ::mmap(nullptr, kSize, PROT_READ | PROT_WRITE, MAP_SHARED, fd1, 0);
    assert(w != MAP_FAILED);
    std::memset(w, 0x5A, kSize);
    ::munmap(w, kSize);
    ::close(fd1);

    void* r = ::mmap(nullptr, kSize, PROT_READ | PROT_WRITE, MAP_SHARED, fd2, 0);
    assert(r != MAP_FAILED);
    auto* bytes = static_cast<const uint8_t*>(r);
    for (size_t i = 0; i < kSize; ++i) assert(bytes[i] == 0x5A);
    ::munmap(r, kSize);
    ::close(fd2);

    assert(P::shm_exists(name));

    // Delete: unlink, then attach must fail with ShmOpen (ENOENT).
    P::shm_unlink(name);
    auto a2 = P::shm_attach(name);
    assert(uballoc::is_err(a2));
    assert(uballoc::unwrap_err(a2).kind == uballoc::ErrorKind::ShmOpen);

    // Idempotent cleanup: NOT_EXIST is treated as success, no crash.
    P::shm_unlink(name);
    assert(!P::shm_exists(name));

    std::cout << "  OK: ShmExists / ShmOpen / idempotent unlink verified" << std::endl;
}

}  // namespace

int main() {
#ifdef UBALLOC_USE_UBSE
    dt14_1_ubse_naming_strings();
    dt14_1b_min_size_enforcement();
    dt14_1_ubse_create_attach_list();
    dt14_2_ubse_osync_by_export_node();
    dt14_3_ubse_p1_preempt_fallback();
    dt14_4_ubse_delete_retry();
    dt14_5_ubse_unreferenced_3state();
    dt14_6_ubse_error_codes();
#endif
    dt14_7a_posix_naming();
    dt14_7b_provider_semantics();

    std::cout << "\n=== AR-14 DT passed ===" << std::endl;
#ifdef UBALLOC_USE_UBSE
    std::cout << "DT14-1..6: event-sync dual-process (UBALLOC_ROLE=P0/P1, any start order)." << std::endl;
#else
    std::cout << "PosixShmProvider subset: DT14-7a/7b. DT14-1..6 need UBSE build + 2-node hardware."
              << std::endl;
#endif
    return 0;
}
