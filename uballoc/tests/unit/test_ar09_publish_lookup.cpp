// SPDX-License-Identifier: Apache-2.0

/**
 * @file test_ar09_publish_lookup.cpp
 * @brief AR-09 DT: Publish & Lookup by Type ID
 *
 * Cases:
 *   DT09-1 R1,R2: P0 publishes, P1 looks up and dereferences
 *   DT09-2 R6: concurrent publish of one type_id — exactly one CAS winner
 *   DT09-3 R5: unpublish releases the type_id for re-ownership
 *   DT09-4 R2: two processes, two type_ids, independent discovery
 *   DT09-5 R7: registry exhaustion at 256 entries; unpublish does NOT
 *                  recycle the slot (sticky exhaustion) — run LAST
 *   DT09-6 R3: blocking lookup returns owner==-1 when absent, then succeeds after publish
 *   DT09-7 R4: lookup_by_address scans only (no remote load); uses P2 (rank=2)
 *                  whose registry is unmapped after P1 init, then resolves
 *   DT09-8 R8: unpublish by a non-owner with explicit owner rank
 */

#include "uballoc.hpp"
#include "test_helpers.hpp"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>

#include <sys/wait.h>

namespace {

constexpr uint32_t TID_A = 200;
constexpr uint32_t TID_B = 201;
constexpr uint32_t TID_C = 202;
constexpr uint32_t TID_TO = 150;   // DT09-6 blocking target
constexpr uint32_t TID_TO_MISS = 151;

// -----------------------------------------------------------------------------
// DT09-1 R1,R2
// -----------------------------------------------------------------------------
void dt09_1_publish_lookup(const std::string& heap_id) {
    std::cout << "[DT09-1] P0 publishes, P1 finds and dereferences..." << std::endl;
    int fds[2];
    assert(pipe(fds) == 0);

    ar_set_heap_id(heap_id);
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        ::close(fds[1]);
        uint64_t v = 0;
        ar_read_fd(fds[0], &v, sizeof(v));

        ar_child_init();
        auto info = uballoc::lookup_by_type(TID_A);
        assert(info.owner_process == 0);
        assert(info.address == reinterpret_cast<void*>(v));
        assert(info.size == 64);
        auto* bytes = static_cast<const uint8_t*>(info.address);
        for (int i = 0; i < 64; ++i) assert(bytes[i] == 0x5A);
        _exit(0);
    }

    ::close(fds[0]);
    ar_parent_init();
    void* p = uballoc::malloc(64);
    assert(p);
    int slot = uballoc::publish(p, 64, TID_A);
    assert(slot >= 0);
    std::memset(p, 0x5A, 64);

    uint64_t v = reinterpret_cast<uintptr_t>(p);
    ar_write_fd(fds[1], &v, sizeof(v));

    int st;
    waitpid(pid, &st, 0);
    check_child(st);
    ::close(fds[1]);
    ar_alloc().reset();
}

// -----------------------------------------------------------------------------
// DT09-2 R6: concurrent publish — exactly one winner
// -----------------------------------------------------------------------------
void dt09_2_cas_exclusive(const std::string& heap_id) {
    std::cout << "[DT09-2] concurrent publish: exactly one CAS winner..." << std::endl;
    int p2c[2], c2p[2];
    assert(pipe(p2c) == 0 && pipe(c2p) == 0);

    ar_set_heap_id(heap_id);
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        ::close(p2c[1]);
        ::close(c2p[0]);
        assert(ar_read_byte(p2c[0]) == 'G');  // P0 initialized

        ar_child_init();
        ar_write_byte(c2p[1], 'G');  // ready
        assert(ar_read_byte(p2c[0]) == 'G');  // synchronized start

        void* p = uballoc::malloc(64);
        assert(p);
        int rc = uballoc::publish(p, 64, TID_A);
        int64_t payload[2] = {rc,
                              static_cast<int64_t>(reinterpret_cast<uintptr_t>(p))};
        ar_write_fd(c2p[1], payload, sizeof(payload));
        _exit(0);
    }

    ::close(p2c[0]);
    ::close(c2p[1]);
    ar_parent_init();
    ar_write_byte(p2c[1], 'G');
    assert(ar_read_byte(c2p[0]) == 'G');
    ar_write_byte(p2c[1], 'G');

    void* pp = uballoc::malloc(64);
    assert(pp);
    int rp = uballoc::publish(pp, 64, TID_A);

    int64_t payload[2] = {0, 0};
    ar_read_fd(c2p[0], payload, sizeof(payload));
    int rc = static_cast<int>(payload[0]);

    assert((rp >= 0) != (rc >= 0));  // exactly one winner
    int winner_rank = (rp >= 0) ? 0 : 1;

    // The loser must observe the winner as owner.
    auto info = uballoc::lookup_by_type(TID_A);
    assert(info.owner_process == winner_rank);
    if (winner_rank == 0) {
        assert(info.address == pp);
    } else {
        assert(info.address == reinterpret_cast<void*>(payload[1]));
    }

    int st;
    waitpid(pid, &st, 0);
    check_child(st);
    ::close(p2c[1]);
    ::close(c2p[0]);
    ar_alloc().reset();
}

// -----------------------------------------------------------------------------
// DT09-3 R5: unpublish then re-own
// -----------------------------------------------------------------------------
void dt09_3_unpublish_reown(const std::string& heap_id) {
    std::cout << "[DT09-3] unpublish releases the type_id for re-ownership..." << std::endl;
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        ar_set_heap_id(heap_id);
        ar_child_init();

        void* p = uballoc::malloc(64);
        assert(p && uballoc::publish(p, 64, TID_A) >= 0);
        assert(uballoc::lookup_by_type(TID_A).owner_process == 0);

        assert(uballoc::unpublish(p));
        assert(uballoc::lookup_by_type(TID_A).owner_process == -1);

        // Same type_id can be claimed again.
        assert(uballoc::publish(p, 64, TID_A) >= 0);
        assert(uballoc::lookup_by_type(TID_A).owner_process == 0);

        uballoc::unpublish(p);
        uballoc::free(p);
        std::cout << "  OK: ownership released and reclaimable" << std::endl;
        _exit(0);
    }
    int st;
    waitpid(pid, &st, 0);
    check_child(st);
}

// -----------------------------------------------------------------------------
// DT09-4 R2: independent type_ids in both directions
// -----------------------------------------------------------------------------
void dt09_4_independent_type_ids(const std::string& heap_id) {
    std::cout << "[DT09-4] two processes, two type_ids, mutual discovery..." << std::endl;
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
        void* p = uballoc::malloc(64);
        assert(p && uballoc::publish(p, 64, TID_B) >= 0);
        std::memset(p, 0x3C, 64);

        auto info = uballoc::lookup_by_type_blocking(TID_A, 5000);
        assert(info.owner_process == 0 && info.size == 64);
        auto* bytes = static_cast<const uint8_t*>(info.address);
        for (int i = 0; i < 64; ++i) assert(bytes[i] == 0xC3);

        ar_write_byte(rp[1], 'D');
        _exit(0);
    }

    ::close(fds[0]);
    ::close(rp[1]);
    ar_parent_init();
    void* p = uballoc::malloc(64);
    assert(p && uballoc::publish(p, 64, TID_A) >= 0);
    std::memset(p, 0xC3, 64);
    ar_write_byte(fds[1], 'G');

    assert(ar_read_byte(rp[0]) == 'D');
    auto info = uballoc::lookup_by_type_blocking(TID_B, 5000);
    assert(info.owner_process == 1 && info.size == 64);
    auto* bytes = static_cast<const uint8_t*>(info.address);
    for (int i = 0; i < 64; ++i) assert(bytes[i] == 0x3C);

    int st;
    waitpid(pid, &st, 0);
    check_child(st);
    ::close(fds[1]);
    ar_alloc().reset();
}

// -----------------------------------------------------------------------------
// DT09-5 R7: registry capacity 256, sticky exhaustion (run LAST)
// -----------------------------------------------------------------------------
void dt09_5_registry_exhaustion(const std::string& heap_id) {
    std::cout << "[DT09-5] 256 publishes succeed; 257th fails; sticky after unpublish..." << std::endl;
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        ar_set_heap_id(heap_id);
        ar_child_init();

        // 256 distinct type_ids (0..255, all < 256).
        std::vector<void*> blocks;
        blocks.reserve(256);
        for (uint32_t tid = 0; tid < 256; ++tid) {
            void* p = uballoc::malloc(32);
            assert(p);
            assert(uballoc::publish(p, 32, tid) >= 0);
            blocks.push_back(p);
        }

        // 257th publish: registry exhausted (next_slot == 256).
        {
            void* q = uballoc::malloc(32);
            assert(q);
            assert(uballoc::publish(q, 32, 256) == -1);
            uballoc::free(q);
        }

        // Sticky exhaustion: unpublish frees the CAS ownership of type_id 0
        // but recycles no registry slot, so a re-publish still fails.
        assert(uballoc::unpublish(blocks[0]));
        {
            void* q = uballoc::malloc(32);
            assert(q);
            assert(uballoc::publish(q, 32, 0) == -1);
            uballoc::free(q);
        }

        for (size_t i = 1; i < blocks.size(); ++i) uballoc::free(blocks[i]);
        std::cout << "  OK: exhaustion and stickiness confirmed" << std::endl;
        _exit(0);
    }
    int st;
    waitpid(pid, &st, 0);
    check_child(st);
}

// -----------------------------------------------------------------------------
// DT09-6 R3: blocking lookup returns owner==-1 when absent, then succeeds after publish
// -----------------------------------------------------------------------------
void dt09_6_blocking_timeout(const std::string& heap_id) {
    std::cout << "[DT09-6] blocking(151,400) returns -1; blocking(150) succeeds after publish..." << std::endl;
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
        auto info = uballoc::lookup_by_type_blocking(TID_TO_MISS, 400);
        assert(info.owner_process == -1);

        ar_write_byte(rp[1], 'R');  // ask P0 to publish
        auto hit = uballoc::lookup_by_type_blocking(TID_TO, 5000);
        assert(hit.owner_process == 0 && hit.size == 64);
        auto* bytes = static_cast<const uint8_t*>(hit.address);
        for (int i = 0; i < 64; ++i) assert(bytes[i] == 0x77);

        ar_write_byte(rp[1], 'D');
        _exit(0);
    }

    ::close(fds[0]);
    ::close(rp[1]);
    ar_parent_init();
    ar_write_byte(fds[1], 'G');

    assert(ar_read_byte(rp[0]) == 'R');
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    void* p = uballoc::malloc(64);
    assert(p && uballoc::publish(p, 64, TID_TO) >= 0);
    std::memset(p, 0x77, 64);

    assert(ar_read_byte(rp[0]) == 'D');
    assert(uballoc::unpublish(p));
    uballoc::free(p);

    int st;
    waitpid(pid, &st, 0);
    check_child(st);
    ::close(fds[1]);
    ar_alloc().reset();
}

// -----------------------------------------------------------------------------
// DT09-7 R4: lookup_by_address scans without triggering remote load
// -----------------------------------------------------------------------------
void dt09_7_lookup_by_address(const std::string& heap_id) {
    std::cout << "[DT09-7] lookup_by_address: scan-only (P2 registry unmapped)..." << std::endl;

    ar_set_heap_id(heap_id);
    ar_parent_init();   // P0 = rank 0 (coordinator only, no publish)

    pid_t pid1 = fork();
    assert(pid1 >= 0);
    if (pid1 == 0) {
        // P1 (rank=1) — test subject.
        ar_child_init();   // init wire_up's rank=1 (self) + rank=0 (P0) only;
                           // published_registry_ptrs_[2..7] remain nullptr.

        int p2_to_p1[2], p1_to_p2[2];
        assert(pipe(p2_to_p1) == 0 && pipe(p1_to_p2) == 0);

        pid_t pid2 = fork();
        assert(pid2 >= 0);
        if (pid2 == 0) {
            // P2 (rank=2) — publisher of TID_C.
            ::close(p2_to_p1[0]);
            ::close(p1_to_p2[1]);

            ar_child_init();   // rank=2
            void* p = uballoc::malloc(64);
            assert(p && uballoc::publish(p, 64, TID_C) >= 0);
            std::memset(p, 0x77, 64);

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

        // Phase 1 — scan-only: published_registry_ptrs_[2] is nullptr
        auto pre = uballoc::lookup_by_address(remote);
        assert(pre.owner_process == -1);

        // Phase 2 — lookup_by_type triggers check_remote_segments.
        auto via_type = uballoc::lookup_by_type(TID_C);
        assert(via_type.owner_process == 2);
        assert(via_type.address == remote);

        // Phase 3 — after the metadata is loaded, lookup_by_address finds it too.
        auto post = uballoc::lookup_by_address(remote);
        assert(post.owner_process == 2);
        assert(post.type_id == TID_C);
        assert(post.address == remote);

        auto* bytes = static_cast<const uint8_t*>(post.address);
        for (int i = 0; i < 64; ++i) assert(bytes[i] == 0x77);

        char done = 'D';
        ar_write_fd(p1_to_p2[1], &done, 1);
        ::close(p1_to_p2[1]);

        int st2;
        waitpid(pid2, &st2, 0);
        check_child(st2);

        std::cout << "  OK: scan-only verified (P2 registry unmapped -> -1, post-attach -> found)" << std::endl;
        _exit(0);
    }

    int st1;
    waitpid(pid1, &st1, 0);
    check_child(st1);
    ar_alloc().reset();
}

// -----------------------------------------------------------------------------
// DT09-8 R8: unpublish by a non-owner (explicit owner rank)
// -----------------------------------------------------------------------------
void dt09_8_non_owner_unpublish(const std::string& heap_id) {
    std::cout << "[DT09-8] P1 unpublishes P0's entry with explicit owner rank..." << std::endl;
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
        uint64_t v = 0;
        ar_read_fd(fds[0], &v, sizeof(v));
        auto* remote = reinterpret_cast<void*>(v);

        assert(uballoc::unpublish(remote, 0));
        assert(uballoc::lookup_by_type(TID_A).owner_process == -1);

        ar_write_byte(rp[1], 'D');
        _exit(0);
    }

    ::close(fds[0]);
    ::close(rp[1]);
    ar_parent_init();
    void* p = uballoc::malloc(64);
    assert(p && uballoc::publish(p, 64, TID_A) >= 0);
    ar_write_byte(fds[1], 'G');
    uint64_t v = reinterpret_cast<uintptr_t>(p);
    ar_write_fd(fds[1], &v, sizeof(v));

    assert(ar_read_byte(rp[0]) == 'D');
    assert(uballoc::lookup_by_type(TID_A).owner_process == -1);

    int st;
    waitpid(pid, &st, 0);
    check_child(st);
    ::close(fds[1]);
    uballoc::free(p);
    ar_alloc().reset();
}

}  // namespace

int main() {
    std::string id = ar_heap_id("ar09");
    cleanup_all_distributed_shms(8, id);

    ar_pre_case_cleanup(id);

    dt09_1_publish_lookup(id);
    ar_pre_case_cleanup(id);
    dt09_2_cas_exclusive(id);
    ar_pre_case_cleanup(id);
    dt09_3_unpublish_reown(id);
    ar_pre_case_cleanup(id);
    dt09_4_independent_type_ids(id);
    ar_pre_case_cleanup(id);
    dt09_6_blocking_timeout(id);
    ar_pre_case_cleanup(id);
    dt09_7_lookup_by_address(id);
    ar_pre_case_cleanup(id);
    dt09_8_non_owner_unpublish(id);
    ar_pre_case_cleanup(id);
    dt09_5_registry_exhaustion(id);  // LAST: sticky exhaustion pollutes publishing

    cleanup_all_distributed_shms(8, id);
    std::cout << "\n=== AR-09 DT passed ===" << std::endl;
    return 0;
}
