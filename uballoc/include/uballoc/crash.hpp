// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <atomic>
#include <functional>
#include <cassert>

#include "allocator.hpp"
#include "thread.hpp"
#include "recover.hpp"

namespace uballoc {

/**
 * @file crash.hpp
 * @brief Crash recovery testing utilities
 * 
 * This module provides utilities for testing crash recovery scenarios.
 * It allows simulating crashes at various points during allocation
 * operations to verify recovery logic.
 */

struct CrashContext {
    ThreadId id;
    Allocator<void, void>* allocator;
    RecoverState* state;
    bool simulate_crash;
    size_t crash_point;
    
    CrashContext() : simulate_crash(false), crash_point(0) {}
    
    void set_crash_point(size_t point) {
        simulate_crash = true;
        crash_point = point;
    }
    
    void clear_crash() {
        simulate_crash = false;
        crash_point = 0;
    }
    
    bool should_crash(size_t current_point) {
        return simulate_crash && current_point == crash_point;
    }
};

template<typename F>
void run_with_recovery(Allocator<void, void>& allocator, ThreadId id, F&& operation) {
    RecoverState state;
    
    operation();
    
    if (state.load()) {
        recover_allocator(id, 
                          allocator.small,
                          allocator.large,
                          state,
                          allocator.context.get_distributed_help());
    }
}

namespace crash_test {

inline void test_unsized_to_sized_pre_log([[maybe_unused]] ThreadId id) {
    // Simulate crash before logging unsized_to_sized
    // Recovery should retry the operation
    
    // Note: This is a placeholder for crash testing
    // Full implementation would use fault injection
}

inline void test_unsized_to_sized_post_log([[maybe_unused]] ThreadId id) {
    // Simulate crash after logging but before completion
    // Recovery should complete or retry based on state
    
    // Note: This is a placeholder for crash testing
}

inline void test_global_to_unsized([[maybe_unused]] ThreadId id, [[maybe_unused]] Version version) {
    // Simulate crash during global to unsized transfer
    // Recovery should verify CAS completion via version
    
    // Note: This is a placeholder for crash testing
}

inline void test_bump_to_unsized([[maybe_unused]] ThreadId id, [[maybe_unused]] SlabIndex<Small> start, [[maybe_unused]] Version version) {
    // Simulate crash during bump allocation
    // Recovery should verify bump pointer state
    
    // Note: This is a placeholder for crash testing
}

inline void test_remote_free([[maybe_unused]] ThreadId id, [[maybe_unused]] SlabIndex<Small> index, [[maybe_unused]] Version version, [[maybe_unused]] bool last) {
    // Simulate crash during remote free
    // Recovery should verify remote descriptor CAS
    
    // Note: This is a placeholder for crash testing
}

inline void test_detach([[maybe_unused]] ThreadId id, [[maybe_unused]] SlabIndex<Small> index, [[maybe_unused]] Version version) {
    // Simulate crash during slab detach
    // Recovery should verify detach completion
    
    // Note: This is a placeholder for crash testing
}

inline void coverage_test() {
    // Verify all recovery paths have test coverage
    
    // Note: This requires integration with crash testing framework
    // Placeholder for coverage verification
}

template<typename B>
inline void validate_recovery_state([[maybe_unused]] ThreadId id, Heap<B>& heap, [[maybe_unused]] HeapState<B>& state) {
    // Validate that recovery produced correct allocator state
    
    auto& unsized = heap.owned->unsized;
    
    assert(unsized.is_valid(heap.slabs));
    
    for (size_t i = 0; i < B::COUNT; i++) {
        auto class_result = B::from_index(i);
        if (class_result) {
            B class_ = *class_result;
            assert(heap.owned->sized[class_].is_valid(heap.slabs));
        }
    }
}

}

namespace crash_assert {

inline void assert_recovered(void* ptr, [[maybe_unused]] size_t expected_size) {
    // Assert that recovered allocation is valid
    if (ptr) {
        // Check memory is accessible
        volatile uint8_t* data = static_cast<volatile uint8_t*>(ptr);
        uint8_t val = *data;
        (void)val;
    }
}

inline void assert_clean_state(Allocator<void, void>& allocator, [[maybe_unused]] ThreadId id) {
    assert(!allocator.owned->state.load());
}

}

}