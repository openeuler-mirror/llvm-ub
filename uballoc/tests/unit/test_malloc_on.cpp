// SPDX-License-Identifier: Apache-2.0

/**
 * @file test_malloc_on.cpp
 * @brief Node-targeted allocation (malloc_on) — v1 tests.
 * See node_targeted_allocation.md.
 *
 * Automated subset (any Linux, POSIX semantics — the only node is 0):
 *   MON-1  node identity: node_index_local()==0, node_index_of live==0,
 *          foreign==UINT32_MAX
 *   MON-2  malloc_on(local, Fail): Small + Large round-trips, data
 *          integrity, distinct addresses, usable_size
 *   MON-3  malloc_on(local) reuses grown capacity (second call does not
 *          grow: same segment count)
 *   MON-4  fallbacks on unroutable node (POSIX: node 1): Fail→nullptr,
 *          Local/Any→valid local pointer
 *   MON-5  invalid node (>= MAX_PROCESSES): same fallback matrix
 *   MON-6  Huge size: fallback applied per flag (no node targeting in v1)
 *   MON-7  coexistence: interleaved malloc/malloc_on(0),
 *          publish/lookup of a malloc_on pointer, free all
 *   MON-8  C API: uballoc_malloc_on + enum values + node helpers
 *
 * UBSE 2-node manual recipe (-DUBALLOC_USE_UBSE, dual process):
 *   Node A: UBALLOC_ROLE=P0 ./test_malloc_on
 *   Node B: UBALLOC_ROLE=P1 ./test_malloc_on
 *   P0 waits for node 1 to be live, malloc_on(1, Fail) → growth lands on
 *   node 1 (node_index_of==1), publishes; P1 looks up, verifies data and
 *   its own node_index_of, acks via a marker shm.
 */

#include "uballoc.hpp"
#include "uballoc.h"
#include "test_helpers.hpp"

#include <cassert>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include <unistd.h>

#ifdef UBALLOC_USE_UBSE
#include "uballoc/ubshm_provider.hpp"
#include <chrono>
#endif

namespace {

constexpr uint32_t TID_A = 240;
constexpr uint32_t FOREIGN = UINT32_MAX;

// ---------------------------------------------------------------------------
// MON-1: node identity
// ---------------------------------------------------------------------------
void mon1_node_identity(const std::string& heap) {
    std::cout << "[MON-1] node identity..." << std::endl;
    ar_set_heap_id(heap);
    ar_parent_init();

    uint32_t local = uballoc::node_index_local();
    std::cout << "  node_index_local()=" << local << std::endl;
    assert(local == 0);  // POSIX: single node

    void* p = uballoc::malloc(64);
    assert(p);
    assert(uballoc::node_index_of(p) == 0);
    std::cout << "  node_index_of(live ptr)=" << uballoc::node_index_of(p) << std::endl;

    int stack_var = 0;
    assert(uballoc::node_index_of(&stack_var) == FOREIGN);
    assert(uballoc::node_index_of(nullptr) == FOREIGN);
    std::cout << "  foreign pointers -> UINT32_MAX" << std::endl;

    uballoc::free(p);
    uballoc::reset();
    std::cout << "  OK" << std::endl;
}

// ---------------------------------------------------------------------------
// MON-2: malloc_on(local, Fail) round-trips (Small + Large)
// ---------------------------------------------------------------------------
void mon2_local_roundtrip(const std::string& heap) {
    std::cout << "[MON-2] malloc_on(local, Fail) round-trips..." << std::endl;
    ar_set_heap_id(heap);
    ar_parent_init();

    for (size_t sz : {size_t(64), size_t(128), size_t(20 * 1024), size_t(64 * 1024)}) {
        void* p = uballoc::malloc_on(sz, 0, uballoc::NodeFallback::Fail);
        assert(p && "malloc_on(local, Fail) must succeed on POSIX");
        std::memset(p, 0xA5, sz);
        for (size_t i = 0; i < sz; ++i) {
            assert(static_cast<uint8_t*>(p)[i] == 0xA5);
        }
        assert(uballoc::node_index_of(p) == 0);
        assert(uballoc::usable_size(p) >= sz);
        uballoc::free(p);
    }

    // Repeated calls return distinct live addresses.
    std::vector<void*> ptrs;
    for (int i = 0; i < 32; ++i) {
        void* p = uballoc::malloc_on(64, 0, uballoc::NodeFallback::Fail);
        assert(p);
        std::memset(p, i, 64);
        ptrs.push_back(p);
    }
    for (int i = 0; i < 32; ++i) {
        for (int j = 0; j < 64; ++j) {
            assert(static_cast<uint8_t*>(ptrs[i])[j] == uint8_t(i));
        }
    }
    for (void* p : ptrs) uballoc::free(p);

    uballoc::reset();
    std::cout << "  OK (Small + Large, integrity, distinct addresses)" << std::endl;
}

// ---------------------------------------------------------------------------
// MON-3: second malloc_on(local) reuses grown capacity (no new segment)
// ---------------------------------------------------------------------------
void mon3_reuse_grown_capacity(const std::string& heap) {
    std::cout << "[MON-3] reuse of grown capacity..." << std::endl;
    ar_set_heap_id(heap);
    ar_parent_init();

    auto& alloc = uballoc::get_global_allocator<uballoc::DistributedShmBackend<>>();

    void* p1 = uballoc::malloc_on(64, 0, uballoc::NodeFallback::Fail);
    assert(p1);
    size_t segs_after_first = alloc.backend().segments_[0][0].size();

    for (int i = 0; i < 100; ++i) {
        void* p = uballoc::malloc_on(64, 0, uballoc::NodeFallback::Fail);
        assert(p);
        assert(uballoc::node_index_of(p) == 0);
        uballoc::free(p);
    }
    size_t segs_after_loop = alloc.backend().segments_[0][0].size();
    std::cout << "  small segments: " << segs_after_first << " -> " << segs_after_loop
              << std::endl;
    assert(segs_after_loop <= segs_after_first + 1 &&
           "100x64B malloc_on must not grow a segment per call");

    uballoc::free(p1);
    uballoc::reset();
    std::cout << "  OK" << std::endl;
}

// ---------------------------------------------------------------------------
// MON-4/MON-5: fallback matrix on unroutable / invalid nodes
// ---------------------------------------------------------------------------
void mon_fallback(const std::string& heap, uint32_t node, const char* label) {
    std::cout << "[MON] fallback matrix, node=" << node << " (" << label << ")..." << std::endl;
    ar_set_heap_id(heap);
    ar_parent_init();

    void* f = uballoc::malloc_on(64, node, uballoc::NodeFallback::Fail);
    assert(f == nullptr && "unroutable node + Fail must return nullptr");

    void* l = uballoc::malloc_on(64, node, uballoc::NodeFallback::Local);
    assert(l != nullptr && "unroutable node + Local must fall back to plain malloc");
    std::memset(l, 0x5A, 64);
    assert(uballoc::node_index_of(l) == 0);
    uballoc::free(l);

    void* a = uballoc::malloc_on(64, node, uballoc::NodeFallback::AnyNode);
    assert(a != nullptr && "unroutable node + Any must fall back to plain malloc");
    uballoc::free(a);

    uballoc::reset();
    std::cout << "  OK (Fail=nullptr, Local/Any=valid local ptr)" << std::endl;
}

// ---------------------------------------------------------------------------
// MON-6: Huge size applies the fallback (no node targeting in v1)
// ---------------------------------------------------------------------------
void mon6_huge_fallback(const std::string& heap) {
    std::cout << "[MON-6] Huge size fallback..." << std::endl;
    ar_set_heap_id(heap);
    ar_parent_init();

    constexpr size_t kHuge = 8 * 1024 * 1024;  // > Large max (2MB) -> Huge

    void* f = uballoc::malloc_on(kHuge, 0, uballoc::NodeFallback::Fail);
    assert(f == nullptr && "Huge + Fail must return nullptr in v1");

    void* l = uballoc::malloc_on(kHuge, 0, uballoc::NodeFallback::Local);
    assert(l != nullptr && "Huge + Local must fall back to plain malloc");
    std::memset(l, 0xC3, 4096);
    uballoc::free(l);

    uballoc::reset();
    std::cout << "  OK" << std::endl;
}

// ---------------------------------------------------------------------------
// MON-7: coexistence with plain malloc / publish / lookup
// ---------------------------------------------------------------------------
void mon7_coexistence(const std::string& heap) {
    std::cout << "[MON-7] coexistence..." << std::endl;
    ar_set_heap_id(heap);
    ar_parent_init();

    for (int round = 0; round < 10; ++round) {
        void* plain = uballoc::malloc(64);
        void* on = uballoc::malloc_on(64, 0, uballoc::NodeFallback::Fail);
        assert(plain && on);
        std::memset(plain, 0x11, 64);
        std::memset(on, 0x22, 64);

        assert(uballoc::publish(on, 64, TID_A) >= 0);
        auto info = uballoc::lookup_by_type(TID_A);
        assert(info.address == on && info.size == 64);
        for (int i = 0; i < 64; ++i) {
            assert(static_cast<uint8_t*>(info.address)[i] == 0x22);
        }

        uballoc::unpublish(on);
        uballoc::free(on);
        uballoc::free(plain);
    }
    std::cout << "  OK (10 rounds interleaved malloc/malloc_on/publish)" << std::endl;
    uballoc::reset();
}

// ---------------------------------------------------------------------------
// MON-8: C API
// ---------------------------------------------------------------------------
extern "C" void mon8_c_api(const std::string& heap) {
    std::cout << "[MON-8] C API..." << std::endl;
    ar_set_heap_id(heap);
    ar_parent_init();

    // Enum values match the C++ NodeFallback.
    static_assert(static_cast<uint32_t>(uballoc::NodeFallback::Fail) ==
                  UBALLOC_NODE_FALLBACK_FAIL, "");
    static_assert(static_cast<uint32_t>(uballoc::NodeFallback::Local) ==
                  UBALLOC_NODE_FALLBACK_LOCAL, "");
    static_assert(static_cast<uint32_t>(uballoc::NodeFallback::AnyNode) ==
                  UBALLOC_NODE_FALLBACK_ANY, "");

    void* p = uballoc_malloc_on(64, 0, UBALLOC_NODE_FALLBACK_FAIL);
    assert(p);
    std::memset(p, 0x99, 64);
    assert(uballoc_node_index_local() == 0);
    assert(uballoc_node_index_of(p) == 0);
    assert(uballoc_malloc_on(64, 1, UBALLOC_NODE_FALLBACK_FAIL) == nullptr);
    void* q = uballoc_malloc_on(64, 1, UBALLOC_NODE_FALLBACK_LOCAL);
    assert(q);
    uballoc_free(q);
    uballoc_free(p);

    uballoc::reset();
    std::cout << "  OK" << std::endl;
}

#ifdef UBALLOC_USE_UBSE
// ---------------------------------------------------------------------------
// UBSE 2-node manual recipe (node_targeted_allocation.md §9).
//   Node A: UBALLOC_ROLE=P0 ./test_malloc_on   (default role)
//   Node B: UBALLOC_ROLE=P1 ./test_malloc_on
// Any start order; P0 polls for node 1 liveness, P1 polls for the publish.
// ---------------------------------------------------------------------------
// Heap names are env-overridable so repeated runs on a shared cluster can
// use fresh names instead of colliding with leftovers of a crashed run
// (stuck shims cannot be deleted and make the bootstrap join go wrong).
static const char* ubse_heap_name() {
    const char* env = ::getenv("UBALLOC_UBSE_HEAP");
    return (env && *env) ? env : "mon-ubse";
}
static const char* ubse_ack_name() {
    const char* env = ::getenv("UBALLOC_UBSE_ACK");
    return (env && *env) ? env : "mon-ack";
}

constexpr uint32_t kRemoteNode = 1;
constexpr int kPollSecs = 120;

static bool wait_ack_marker() {
    using P = uballoc::UBShmProvider;
    const std::string n = P::shm_name(ubse_ack_name(), 1, 1);  // distinct heap — no collision
    for (int i = 0; i < kPollSecs; ++i) {
        if (P::shm_exists(n)) return true;
        ::sleep(1);
    }
    return P::shm_exists(n);
}

void ubse_p0_remote_growth() {
    std::cout << "[UBSE-P0] node-targeted growth on node " << kRemoteNode << "..." << std::endl;
    // Clean this heap's leftovers before joining (P0 only — P1 must not
    // delete the segments P0 is about to create). With the listing-based
    // cleanup this costs one prefix query plus the real names, instead
    // of the old blind rank x class x slab grid.
    cleanup_all_distributed_shms(2, ubse_heap_name());
    ar_set_heap_id(ubse_heap_name());
    ar_parent_init();
    assert(uballoc::node_index_local() == 0);

    // Wedge-diagnosis sequencing. node_slot_id_of() reads the shared
    // bootstrap slots — a liveness probe with NO daemon RPC and NO create.
    // The previous protocol polled via malloc_on(), which issues a real
    // remote-provisioned create the instant the peer's rank appears — i.e.
    // while the peer process is still mid-init (its rank is claimed before
    // the registry attach). All three dual-node runs so far wedged the peer
    // node at exactly that point (registry imports never reached 2). Wait
    // for liveness, then give the peer a grace window to finish its init
    // (metadata segment + registry attach) before the first create.
    for (int i = 0; i < kPollSecs; ++i) {
        if (ar_alloc().backend().node_slot_id_of(kRemoteNode) != 0) break;
        ::sleep(1);
    }
    std::cout << "  node 1 live; 20s grace for P1 init..." << std::endl;
    ::sleep(20);

    // Single remote-provisioned growth attempt after the grace window.
    void* p = uballoc::malloc_on(64, kRemoteNode, uballoc::NodeFallback::Fail);
    assert(p && "malloc_on(node 1, Fail) must succeed once P1 is live");

    uint32_t backing = uballoc::node_index_of(p);
    std::cout << "  ptr=" << p << " backing node=" << backing << std::endl;
    assert(backing == kRemoteNode && "growth must land on the target node");

    std::memset(p, 0xA5, 64);
    // A second allocation must reuse the node-1 backed capacity.
    void* p2 = uballoc::malloc_on(64, kRemoteNode, uballoc::NodeFallback::Fail);
    assert(p2 && uballoc::node_index_of(p2) == kRemoteNode);
    std::memset(p2, 0x5A, 64);

    assert(uballoc::publish(p, 64, TID_A) >= 0);
    std::cout << "  published; waiting for P1 ack..." << std::endl;
    assert(wait_ack_marker() && "P1 did not ack in time");

    uballoc::unpublish(p);
    uballoc::free(p2);
    uballoc::free(p);
    uballoc::reset();
    std::cout << "  OK: growth on remote node + reuse + cross-node visibility" << std::endl;
}

void ubse_p1_worker() {
    std::cout << "[UBSE-P1] worker (node 1)..." << std::endl;
    ar_set_heap_id(ubse_heap_name());
    ar_parent_init();
    assert(uballoc::node_index_local() == kRemoteNode);

    // Poll for P0's publish.
    uballoc::PublishedInfo info{};
    bool found = false;
    for (int i = 0; i < kPollSecs && !found; ++i) {
        info = uballoc::lookup_by_type(TID_A);
        if (info.address) {
            found = true;
        } else {
            ::sleep(1);
        }
    }
    assert(found && "P1: publish not visible in time");

    uint32_t backing = uballoc::node_index_of(info.address);
    std::cout << "  looked up " << info.address << " backing node=" << backing << std::endl;
    assert(backing == kRemoteNode);

    // Lender-node access: this is the cacheable self-map path.
    for (int i = 0; i < 64; ++i) {
        assert(static_cast<uint8_t*>(info.address)[i] == 0xA5);
    }

    // Ack via a marker shm on a distinct heap id (avoids name collision
    // with this rank's real data segments).
    {
        using P = uballoc::UBShmProvider;
        const std::string n = P::shm_name(ubse_ack_name(), 1, 1);
        P::shm_unlink(n);
        auto c = P::shm_try_create(n, P::min_shm_size);
        assert(uballoc::is_ok(c));
        P::shm_close_fd(uballoc::unwrap(c));
    }

    uballoc::reset();
    std::cout << "  OK: data verified on the lender node" << std::endl;
}
#endif  // UBALLOC_USE_UBSE

}  // namespace

int main() {
#ifdef UBALLOC_USE_UBSE
    const char* role = ::getenv("UBALLOC_ROLE");
    const bool is_p1 = role && std::string(role) == "P1";
    if (is_p1) {
        ubse_p1_worker();
        std::cout << "\n=== malloc_on UBSE P1 passed ===" << std::endl;
        return 0;
    }
#endif

    const std::string id = ar_heap_id("mon");
    cleanup_all_distributed_shms(2, id);

    ar_pre_case_cleanup(id + "_1");
    mon1_node_identity(id + "_1");
    ar_pre_case_cleanup(id + "_2");
    mon2_local_roundtrip(id + "_2");
    ar_pre_case_cleanup(id + "_3");
    mon3_reuse_grown_capacity(id + "_3");
    ar_pre_case_cleanup(id + "_4");
    mon_fallback(id + "_4", 1, "unroutable on POSIX");
    ar_pre_case_cleanup(id + "_5");
    mon_fallback(id + "_5", 100, "invalid node index");
    ar_pre_case_cleanup(id + "_6");
    mon6_huge_fallback(id + "_6");
    ar_pre_case_cleanup(id + "_7");
    mon7_coexistence(id + "_7");
    ar_pre_case_cleanup(id + "_8");
    mon8_c_api(id + "_8");

#ifdef UBALLOC_USE_UBSE
    ubse_p0_remote_growth();
#endif

    cleanup_all_distributed_shms(2, id);
    std::cout << "\n=== malloc_on tests passed ===" << std::endl;
    return 0;
}
