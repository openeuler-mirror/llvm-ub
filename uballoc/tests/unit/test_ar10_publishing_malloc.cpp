// SPDX-License-Identifier: Apache-2.0

/**
 * @file test_ar10_publishing_malloc.cpp
 * @brief AR-10 DT: malloc (Publishing malloc)
 *
 * Cases (all single-process; cross-process behavior is covered by AR-09):
 *   DT10-1 R1: malloc(size, tid) — allocated + published, triplet consistent
 *   DT10-2 R2: equivalence with malloc+publish; type_id mutex on failure
 *   DT10-3 R3: publish failure -> nullptr, internal allocation rolled back
 *   DT10-4 R4: free() leaves the entry dangling unless explicitly unpublished
 *
 */

#include "uballoc.hpp"
#include "test_helpers.hpp"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <iostream>

#include <sys/wait.h>

namespace {

constexpr size_t M = 1024 * 1024;

// -----------------------------------------------------------------------------
// DT10-1 R1
// -----------------------------------------------------------------------------
void dt10_1_combo_alloc_publish(const std::string& heap_id) {
    std::cout << "[DT10-1] malloc(64, 200) allocates and publishes..." << std::endl;
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        ar_set_heap_id(heap_id);
        ar_child_init();

        void* p = uballoc::malloc(64, 200);
        assert(p != nullptr);

        auto info = uballoc::lookup_by_type(200);
        assert(info.owner_process == 0);
        assert(info.address == p);
        assert(info.size == 64);

        std::memset(p, 0x66, 64);
        auto* via_info = static_cast<const uint8_t*>(info.address);
        for (int i = 0; i < 64; ++i) assert(via_info[i] == 0x66);

        assert(uballoc::unpublish(p));
        uballoc::free(p);
        std::cout << "  OK: one call == malloc + publish" << std::endl;
        _exit(0);
    }
    int st;
    waitpid(pid, &st, 0);
    check_child(st);
}

// -----------------------------------------------------------------------------
// DT10-2 R2: two-step vs one-step equivalence + type_id mutex
// -----------------------------------------------------------------------------
void dt10_2_equivalence(const std::string& heap_id) {
    std::cout << "[DT10-2] malloc+publish vs malloc(size,tid); mutex enforced..." << std::endl;
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        ar_set_heap_id(heap_id);
        ar_child_init();

        // Path A: two steps.
        void* pa = uballoc::malloc(64);
        assert(pa);
        assert(uballoc::publish(pa, 64, 210) >= 0);

        // Path B: one step.
        void* pb = uballoc::malloc(64, 211);
        assert(pb != nullptr);

        auto ia = uballoc::lookup_by_type(210);
        assert(ia.owner_process == 0 && ia.address == pa && ia.size == 64);
        auto ib = uballoc::lookup_by_type(211);
        assert(ib.owner_process == 0 && ib.address == pb && ib.size == 64);

        // The type_id is exclusive: second owner fails both ways.
        void* p2 = uballoc::malloc(64);
        assert(p2);
        assert(uballoc::publish(p2, 64, 210) == -1);
        assert(uballoc::malloc(64, 210) == nullptr);
        uballoc::free(p2);

        uballoc::unpublish(pa);
        uballoc::unpublish(pb);
        uballoc::free(pa);
        uballoc::free(pb);
        std::cout << "  OK: paths equivalent, type_id exclusive" << std::endl;
        _exit(0);
    }
    int st;
    waitpid(pid, &st, 0);
    check_child(st);
}

// -----------------------------------------------------------------------------
// DT10-3 R3: failure rolls back the internal allocation
// -----------------------------------------------------------------------------
void dt10_3_failure_rollback(const std::string& heap_id) {
    std::cout << "[DT10-3] occupied type_id -> combo malloc returns nullptr repeatedly..." << std::endl;
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        ar_set_heap_id(heap_id);
        ar_child_init();

        void* occ = uballoc::malloc(32);
        assert(occ && uballoc::publish(occ, 32, 220) >= 0);

        for (int i = 0; i < 10; ++i) {
            assert(uballoc::malloc(64, 220) == nullptr);
        }

        // No orphaned memory: the heap must still serve plain allocations.
        for (int i = 0; i < 10; ++i) {
            void* t = uballoc::malloc(64);
            assert(t != nullptr);
            uballoc::free(t);
        }

        assert(uballoc::unpublish(occ));
        uballoc::free(occ);
        std::cout << "  OK: nullptr on conflict, heap healthy" << std::endl;
        _exit(0);
    }
    int st;
    waitpid(pid, &st, 0);
    check_child(st);
}

// -----------------------------------------------------------------------------
// DT10-4 R4: free() leaves the entry dangling unless explicitly unpublished
// -----------------------------------------------------------------------------
void dt10_4_dangling_publish(const std::string& heap_id) {
    std::cout << "[DT10-4] free() does NOT clear the publish entry (fixed as-is)..." << std::endl;
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        ar_set_heap_id(heap_id);
        ar_child_init();

        void* p = uballoc::malloc(64, 230);
        assert(p != nullptr);
        uballoc::free(p);

        // The lookup still resolves to the freed block. The correct lifecycle
        // is publish -> unpublish -> free. Do NOT dereference info.address.
        auto info = uballoc::lookup_by_type(230);
        assert(info.owner_process == 0);
        assert(info.address == p);
        assert(info.size == 64);

        // Cleanup restores consistency.
        assert(uballoc::unpublish(p));
        assert(uballoc::lookup_by_type(230).owner_process == -1);
        std::cout << "  OK: deviation confirmed and documented" << std::endl;
        _exit(0);
    }
    int st;
    waitpid(pid, &st, 0);
    check_child(st);
}

}  // namespace

int main() {
    std::string id = ar_heap_id("ar10");
    cleanup_all_distributed_shms(8, id);

    ar_pre_case_cleanup(id);

    dt10_1_combo_alloc_publish(id);
    ar_pre_case_cleanup(id);
    dt10_2_equivalence(id);
    ar_pre_case_cleanup(id);
    dt10_3_failure_rollback(id);
    ar_pre_case_cleanup(id);
    dt10_4_dangling_publish(id);
    ar_pre_case_cleanup(id);

    cleanup_all_distributed_shms(8, id);
    std::cout << "\n=== AR-10 DT passed ===" << std::endl;
    return 0;
}
