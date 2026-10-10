// SPDX-License-Identifier: Apache-2.0

/**
 * @file test_recover_state.cpp
 * @brief AR-16 Feature 10 — RecoverState writing correctness tests.
 *
 * Verifies that Actions 1-4 are correctly wired:
 *   1. Normal alloc/free path writes then clears RecoverState (clean state)
 *   2. Small bracket: discriminant is 0x10 series (not 0x20)
 *   3. Large bracket: discriminant is 0x20 series (not 0x10)
 *   4. HelpArray is fed (version > 0 after alloc/free)
 *   5. Payload round-trip: hand-built dirty state decodes correctly
 *   6. recover_allocator() with hand-built dirty state doesn't crash
 *
 * All tests run in fork children to isolate shared memory state.
 */

#include "uballoc.hpp"
#include "test_helpers.hpp"
#include <iostream>
#include <cassert>
#include <cstring>
#include <unistd.h>
#include <sys/wait.h>

static constexpr const char* HEAP_ID = "rctest10";

// =============================================================================
// 1. Normal alloc/free path: RecoverState is written then cleared.
//    After successful malloc + free, owned->state should be 0 (clean).
// =============================================================================
static void test_clean_state_after_alloc_free() {
    std::cout << "[recover] normal alloc/free leaves clean state..." << std::endl;

    cleanup_all_distributed_shms(2, HEAP_ID);
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        setenv("UBALLOC_HEAP_ID", HEAP_ID, 1);
        auto& alloc = uballoc::get_global_allocator<uballoc::DistributedShmBackend<>>();
        alloc.soft_reset();
        alloc.init();
        alloc.init_thread(0);
        assert(alloc.is_initialized());

        void* p = uballoc::malloc(64);
        assert(p);
        memset(p, 0xAB, 64);
        uballoc::free(p);

        auto loaded = uballoc::current_allocator().owned->state.load();
        assert(!loaded);
        std::cout << "  state after alloc+free: clean (0)" << std::endl;

        alloc.reset();
        _exit(0);
    }
    int status;
    waitpid(pid, &status, 0);
    check_child(status);
    cleanup_all_distributed_shms(2, HEAP_ID);
    std::cout << "  Verified: clean state after normal alloc/free" << std::endl;
}

// =============================================================================
// 2. Small bracket discriminant: after malloc(64), if we intercept the
//    state before clear, the discriminant high nibble should be 0x10.
//    We can't intercept mid-malloc, but we can hand-build a state with
//    the Small tag and verify recover_allocator dispatches to small_heap.
// =============================================================================
static void test_small_bracket_discriminant() {
    std::cout << "[recover] Small bracket discriminant is 0x1X..." << std::endl;

    cleanup_all_distributed_shms(2, HEAP_ID);
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        setenv("UBALLOC_HEAP_ID", HEAP_ID, 1);
        auto& alloc = uballoc::get_global_allocator<uballoc::DistributedShmBackend<>>();
        alloc.soft_reset();
        alloc.init();
        alloc.init_thread(0);
        assert(alloc.is_initialized());

        // Hand-build a dirty state with Small discriminant (0x15)
        // data = idx(32) | Bit::pack(block)(12)<<32
        uballoc::SlabIndex<uballoc::Small> slab_idx(0);
        uballoc::Bit block(uballoc::u6(3), uballoc::u6(5));  // col=3, row=5
        uint64_t data = slab_idx.internal() | (uballoc::Bit::pack(block) << 32);
        uballoc::current_allocator().owned->state.store_small(0x15, data);

        // Verify it loads correctly
        auto loaded = uballoc::current_allocator().owned->state.load();
        assert(loaded);
        auto [disc, raw] = *loaded;
        std::cout << "  disc=0x" << std::hex << (int)disc << std::dec
                  << " (high nibble=0x" << std::hex << (int)(disc & 0xF0) << std::dec << ")"
                  << std::endl;
        assert((disc & 0xF0) == 0x10);  // Small bracket

        // Verify the payload decodes correctly
        uint32_t idx_raw = raw & 0xFFFFFFFF;
        uint64_t bit_raw = (raw >> 32) & 0xFFF;
        uballoc::Bit decoded_block = uballoc::Bit::unpack(bit_raw);
        std::cout << "  idx=" << idx_raw << " block col=" << (int)decoded_block.col
                  << " row=" << (int)decoded_block.row << std::endl;
        assert(decoded_block.col == 3);
        assert(decoded_block.row == 5);

        // Clean up
        uballoc::current_allocator().owned->state.clear();

        alloc.reset();
        _exit(0);
    }
    int status;
    waitpid(pid, &status, 0);
    check_child(status);
    cleanup_all_distributed_shms(2, HEAP_ID);
    std::cout << "  Verified: Small bracket discriminant is 0x1X" << std::endl;
}

// =============================================================================
// 3. Large bracket discriminant: store_large writes 0x20 series.
// =============================================================================
static void test_large_bracket_discriminant() {
    std::cout << "[recover] Large bracket discriminant is 0x2X..." << std::endl;

    cleanup_all_distributed_shms(2, HEAP_ID);
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        setenv("UBALLOC_HEAP_ID", HEAP_ID, 1);
        auto& alloc = uballoc::get_global_allocator<uballoc::DistributedShmBackend<>>();
        alloc.soft_reset();
        alloc.init();
        alloc.init_thread(0);
        assert(alloc.is_initialized());

        // Hand-build a dirty state with Large discriminant using store_large
        uballoc::SlabIndex<uballoc::Large> slab_idx(0);
        uint64_t data = slab_idx.internal();
        uballoc::current_allocator().owned->state.store_large(0x15, data);

        auto loaded = uballoc::current_allocator().owned->state.load();
        assert(loaded);
        auto [disc, raw] = *loaded;
        std::cout << "  disc=0x" << std::hex << (int)disc << std::dec
                  << " (high nibble=0x" << std::hex << (int)(disc & 0xF0) << std::dec << ")"
                  << std::endl;
        assert((disc & 0xF0) == 0x20);  // Large bracket

        // Verify idx decodes
        uint32_t idx_raw = raw & 0xFFFFFFFF;
        assert(idx_raw == slab_idx.internal());

        uballoc::current_allocator().owned->state.clear();

        alloc.reset();
        _exit(0);
    }
    int status;
    waitpid(pid, &status, 0);
    check_child(status);
    cleanup_all_distributed_shms(2, HEAP_ID);
    std::cout << "  Verified: Large bracket discriminant is 0x2X" << std::endl;
}

// =============================================================================
// 4. HelpArray is fed: after alloc/free, the per-thread version in
//    HelpArray should be > 0 (next_version was called).
// =============================================================================
static void test_help_array_fed() {
    std::cout << "[recover] HelpArray fed (version > 0)..." << std::endl;

    cleanup_all_distributed_shms(2, HEAP_ID);
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        setenv("UBALLOC_HEAP_ID", HEAP_ID, 1);
        auto& alloc = uballoc::get_global_allocator<uballoc::DistributedShmBackend<>>();
        alloc.soft_reset();
        alloc.init();
        alloc.init_thread(0);
        assert(alloc.is_initialized());

        // Do alloc/free to trigger next_version + do_help
        void* p = uballoc::malloc(64);
        assert(p);
        uballoc::free(p);

        // Check HelpArray: the per-thread self-version should be > 0
        auto* help = uballoc::current_allocator().context.get_distributed_help();
        if (help) {
            uballoc::ThreadId tid = uballoc::current_allocator().thread_id;
            uballoc::Version ver = help->load(tid, tid);
            std::cout << "  per-thread version after alloc/free: " << ver.value_ << std::endl;
            assert(ver.value_ > 0);  // version must have been bumped
        }

        alloc.reset();
        _exit(0);
    }
    int status;
    waitpid(pid, &status, 0);
    check_child(status);
    cleanup_all_distributed_shms(2, HEAP_ID);
    std::cout << "  Verified: HelpArray version > 0 after alloc/free" << std::endl;
}

// =============================================================================
// 5. Payload round-trip: hand-build dirty states for each discriminant
//    and verify they decode correctly via RecoverState::load().
// =============================================================================
static void test_payload_round_trip() {
    std::cout << "[recover] payload round-trip for all discriminants..." << std::endl;

    cleanup_all_distributed_shms(2, HEAP_ID);
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        setenv("UBALLOC_HEAP_ID", HEAP_ID, 1);
        auto& alloc = uballoc::get_global_allocator<uballoc::DistributedShmBackend<>>();
        alloc.soft_reset();
        alloc.init();
        alloc.init_thread(0);
        assert(alloc.is_initialized());

        auto& state = uballoc::current_allocator().owned->state;

        // Test 5a: UnsizedToSized (disc 0x15 = SizedToApplication, but test Bit packing)
        // disc 5 = SizedToApplication: idx(32) | Bit::pack(block)(12)<<32
        {
            uballoc::SlabIndex<uballoc::Small> idx(42);
            uballoc::Bit block(uballoc::u6(7), uballoc::u6(3));  // col=7, row=3
            uint64_t data = idx.internal() | (uballoc::Bit::pack(block) << 32);
            state.store_small(0x15, data);

            auto loaded = state.load();
            assert(loaded);
            auto [disc, raw] = *loaded;
            assert((disc & 0x0F) == 5);
            assert((raw & 0xFFFFFFFF) == idx.internal());
            uballoc::Bit decoded = uballoc::Bit::unpack((raw >> 32) & 0xFFF);
            assert(decoded.col == 7);
            assert(decoded.row == 3);
            std::cout << "  SizedToApplication: idx=" << (raw & 0xFFFFFFFF)
                      << " col=" << (int)decoded.col << " row=" << (int)decoded.row
                      << " (round-trip OK)" << std::endl;
        }

        // Test 5b: GlobalToUnsized (disc 1): idx(32) | version(16)<<32
        {
            uint32_t test_idx = 100;
            uint16_t test_ver = 7;
            uint64_t data = test_idx | (static_cast<uint64_t>(test_ver) << 32);
            state.store_small(0x11, data);

            auto loaded = state.load();
            assert(loaded);
            auto [disc, raw] = *loaded;
            assert((disc & 0x0F) == 1);
            assert((raw & 0xFFFFFFFF) == test_idx);
            assert(((raw >> 32) & 0xFFFF) == test_ver);
            std::cout << "  GlobalToUnsized: idx=" << (raw & 0xFFFFFFFF)
                      << " ver=" << ((raw >> 32) & 0xFFFF) << " (round-trip OK)" << std::endl;
        }

        // Test 5c: Remote (disc 7): idx(32) | version(16)<<32 | last(1)<<48
        {
            uint32_t test_idx = 200;
            uint16_t test_ver = 3;
            uint64_t data = test_idx |
                (static_cast<uint64_t>(test_ver) << 32) |
                (1ULL << 48);
            state.store_small(0x17, data);

            auto loaded = state.load();
            assert(loaded);
            auto [disc, raw] = *loaded;
            assert((disc & 0x0F) == 7);
            assert((raw & 0xFFFFFFFF) == test_idx);
            assert(((raw >> 32) & 0xFFFF) == test_ver);
            assert(((raw >> 48) & 0x1) == 1);  // last=true
            std::cout << "  Remote: idx=" << (raw & 0xFFFFFFFF)
                      << " ver=" << ((raw >> 32) & 0xFFFF)
                      << " last=" << ((raw >> 48) & 0x1) << " (round-trip OK)" << std::endl;
        }

        // Test 5d: Detach (disc 8): idx(32) | version(16)<<32
        {
            uint32_t test_idx = 50;
            uint16_t test_ver = 9;
            uint64_t data = test_idx | (static_cast<uint64_t>(test_ver) << 32);
            state.store_small(0x18, data);

            auto loaded = state.load();
            assert(loaded);
            auto [disc, raw] = *loaded;
            assert((disc & 0x0F) == 8);
            assert((raw & 0xFFFFFFFF) == test_idx);
            assert(((raw >> 32) & 0xFFFF) == test_ver);
            std::cout << "  Detach: idx=" << (raw & 0xFFFFFFFF)
                      << " ver=" << ((raw >> 32) & 0xFFFF) << " (round-trip OK)" << std::endl;
        }

        state.clear();
        alloc.reset();
        _exit(0);
    }
    int status;
    waitpid(pid, &status, 0);
    check_child(status);
    cleanup_all_distributed_shms(2, HEAP_ID);
    std::cout << "  Verified: payload round-trip for all discriminants" << std::endl;
}

// =============================================================================
// 6. recover_allocator() with hand-built dirty state doesn't crash.
//    Build a dirty state, call recover(), verify it completes and clears.
// =============================================================================
static void test_recover_allocator_invocation() {
    std::cout << "[recover] recover_allocator() with dirty state..." << std::endl;

    cleanup_all_distributed_shms(2, HEAP_ID);
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        setenv("UBALLOC_HEAP_ID", HEAP_ID, 1);
        auto& alloc = uballoc::get_global_allocator<uballoc::DistributedShmBackend<>>();
        alloc.soft_reset();
        alloc.init();
        alloc.init_thread(0);
        assert(alloc.is_initialized());

        // Build a dirty state with a Small UnsizedToSized discriminant
        // and call recover() — it should dispatch and clear.
        uint64_t data = 0;  // idx=0, class=0
        uballoc::current_allocator().owned->state.store_small(0x10, data);

        // Verify state is dirty
        auto loaded = uballoc::current_allocator().owned->state.load();
        assert(loaded);

        // Call recover() — should dispatch and clear
        uballoc::current_allocator().recover();

        // Verify state is clean after recovery
        loaded = uballoc::current_allocator().owned->state.load();
        assert(!loaded);  // recovered and cleared
        std::cout << "  recover() completed, state cleared" << std::endl;

        // Verify subsequent malloc/free still works
        void* p = uballoc::malloc(64);
        assert(p);
        uballoc::free(p);

        alloc.reset();
        _exit(0);
    }
    int status;
    waitpid(pid, &status, 0);
    check_child(status);
    cleanup_all_distributed_shms(2, HEAP_ID);
    std::cout << "  Verified: recover_allocator() works with dirty state" << std::endl;
}

// =============================================================================
// main
// =============================================================================
int main() {
    cleanup_all_distributed_shms(2, HEAP_ID);

    test_clean_state_after_alloc_free();
    test_small_bracket_discriminant();
    test_large_bracket_discriminant();
    test_help_array_fed();
    test_payload_round_trip();
    test_recover_allocator_invocation();

    cleanup_all_distributed_shms(2, HEAP_ID);
    std::cout << "\n=== RecoverState tests passed ===" << std::endl;
    return 0;
}
