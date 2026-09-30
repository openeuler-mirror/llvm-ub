// SPDX-License-Identifier: Apache-2.0
//
// Tests defrag_hints and usable_size APIs.

#include "uballoc.hpp"
#include "test_helpers.hpp"
#include <iostream>
#include <cassert>
#include <cstring>
#include <vector>

extern "C" {
#include "uballoc.h"
}

// Verify defrag_hints returns live allocations whose slab occupancy < threshold.
static void test_defrag_hints_basic() {
    std::cout << "[defrag] defrag_hints basic..." << std::endl;

    void* p = uballoc::malloc(64);
    assert(p);

    auto hints = uballoc::defrag_hints(0.5);
    std::cout << "  defrag_hints(0.5) returned " << hints.size() << " hints" << std::endl;
    assert(hints.size() >= 1);

    bool found = false;
    for (auto& h : hints) {
        if (h.ptr == p) {
            found = true;
            assert(h.size == 64);
            assert(h.occupancy < 0.5);
        }
    }
    assert(found);

    uballoc::free(p);
    std::cout << "  defrag_hints basic: OK" << std::endl;
}

// Verify threshold filtering: higher threshold returns more hints.
// Allocate 256 x 64B → one slab at 50% occupancy (256/512).
// threshold=0.2 → 0 hints (50% > 20%)
// threshold=0.8 → 256 hints (50% < 80%)
static void test_defrag_threshold() {
    std::cout << "[defrag] threshold filtering..." << std::endl;

    constexpr int N = 256;  // 256/512 = 50% occupancy
    std::vector<void*> ptrs;
    for (int i = 0; i < N; i++) {
        void* p = uballoc::malloc(64);
        assert(p);
        ptrs.push_back(p);
    }

    auto hints_low = uballoc::defrag_hints(0.2);   // 50% > 20% → excluded
    auto hints_high = uballoc::defrag_hints(0.8);  // 50% < 80% → included

    std::cout << "  threshold 0.2 → " << hints_low.size() << " hints" << std::endl;
    std::cout << "  threshold 0.8 → " << hints_high.size() << " hints" << std::endl;
    assert(hints_low.size() == 0);
    assert(hints_high.size() == N);

    for (void* p : ptrs) uballoc::free(p);
    std::cout << "  threshold filtering: OK" << std::endl;
}

// Verify max_hints limit: both C and C++ API.
static void test_max_hints() {
    std::cout << "[defrag] max_hints limit..." << std::endl;

    std::vector<void*> ptrs;
    for (int i = 0; i < 50; i++) {
        void* p = uballoc::malloc(64);
        assert(p);
        ptrs.push_back(p);
    }

    // C++ API with max_hints
    auto cpp_hints = uballoc::defrag_hints(0.5, 5);
    std::cout << "  C++ API max_hints=5 → " << cpp_hints.size() << " hints" << std::endl;
    assert(cpp_hints.size() <= 5);

    // C API with max_hints
    uballoc_defrag_hint_t c_hints[5];
    size_t n = uballoc_defrag_hints(0.5, c_hints, 5);
    std::cout << "  C API max_hints=5 → " << n << " hints" << std::endl;
    assert(n <= 5);

    for (void* p : ptrs) uballoc::free(p);
    std::cout << "  max_hints limit: OK" << std::endl;
}

// Verify usable_size.
static void test_usable_size() {
    std::cout << "[defrag] usable_size..." << std::endl;

    void* p = uballoc::malloc(63);
    assert(p);
    size_t us = uballoc::usable_size(p);
    std::cout << "  malloc(63) → usable_size=" << us << std::endl;
    assert(us == 64);
    assert(us >= 63);
    uballoc::free(p);

    assert(uballoc::usable_size(nullptr) == 0);

    int stack_var = 42;
    assert(uballoc::usable_size(&stack_var) == 0);

    std::cout << "  usable_size: OK" << std::endl;
}

int main() {
    cleanup_all_distributed_shms(2, "deftest");
    setenv("UBALLOC_HEAP_ID", "deftest", 1);

    uballoc::init();

    test_defrag_hints_basic();
    test_defrag_threshold();
    test_max_hints();
    test_usable_size();

    uballoc::reset();
    cleanup_all_distributed_shms(2, "deftest");
    unsetenv("UBALLOC_HEAP_ID");

    std::cout << "\n=== defrag_hints tests passed ===" << std::endl;
    return 0;
}
