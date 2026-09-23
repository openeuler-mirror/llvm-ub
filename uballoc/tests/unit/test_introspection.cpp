// SPDX-License-Identifier: Apache-2.0
//
// Tests return_stats introspection and fragmentation_ratio.

#include "uballoc.hpp"
#include "test_helpers.hpp"
#include <iostream>
#include <cassert>
#include <cstring>
#include <vector>

extern "C" {
#include "uballoc.h"
}

static void test_basic_stats() {
    std::cout << "[introspection] basic return_stats..." << std::endl;

    auto s0 = uballoc::return_stats();
    std::cout << "  initial: live=" << s0.segments_live
              << " frag=" << s0.fragmentation_ratio << std::endl;

    void* p1 = uballoc::malloc(64);
    void* p2 = uballoc::malloc(128);
    void* p3 = uballoc::malloc(256);
    assert(p1 && p2 && p3);

    auto s1 = uballoc::return_stats();
    std::cout << "  after-alloc: live=" << s1.segments_live
              << " allocated=" << s1.allocated_to_app_bytes << std::endl;
    assert(s1.allocated_to_app_bytes >= s0.allocated_to_app_bytes + 64 + 128 + 256);
    assert(s1.segments_live >= 1);

    uballoc::free(p1);
    uballoc::free(p2);
    uballoc::free(p3);

    auto s2 = uballoc::return_stats();
    assert(s2.freed_from_app_bytes >= s1.freed_from_app_bytes + 64 + 128 + 256);
    std::cout << "  basic stats: OK" << std::endl;
}

// Verify fragmentation_ratio with a known, precomputable value.
//
// malloc(8) allocates from Small class 8B (the minimum class).
// Each slab is 32KB, holding 32KB/8 = 4096 blocks.
// One slab fills → segment created with 4MB ladder (seg0).
//
// After allocating N objects:
//   allocated_to_app = N * 8 (the class size, not the request size)
//   held = segment total_size (the 4MB segment)
//   frag_ratio = held / in_use = segment_size / (N * 8)
//
// With N = 1000: in_use = 8000B, held ≈ 4194304B
//   frag_ratio ≈ 4194304 / 8000 = 524.288
static void test_fragmentation_ratio() {
    std::cout << "[introspection] fragmentation ratio..." << std::endl;

    constexpr int N = 1000;
    constexpr size_t CLASS_SIZE = 8;  // Small::SIZES[0]

    std::vector<void*> ptrs;
    for (int i = 0; i < N; i++) {
        void* p = uballoc::malloc(8);
        assert(p);
        ptrs.push_back(p);
    }

    auto s = uballoc::return_stats();
    uint64_t held = s.requested_from_os_bytes - s.returned_to_os_bytes;
    uint64_t in_use = s.allocated_to_app_bytes - s.freed_from_app_bytes;
    double expected = (double)held / in_use;

    std::cout << "  held=" << held << " in_use=" << in_use
              << " frag_ratio=" << s.fragmentation_ratio
              << " expected=" << expected << std::endl;

    assert(in_use == (uint64_t)N * CLASS_SIZE);
    assert(s.fragmentation_ratio == expected);
    assert(s.fragmentation_ratio > 1.0);

    for (void* p : ptrs) uballoc::free(p);
    std::cout << "  fragmentation ratio: OK" << std::endl;
}

int main() {
    cleanup_all_distributed_shms(2, "introtest");
    setenv("UBALLOC_HEAP_ID", "introtest", 1);

    uballoc::init();

    test_basic_stats();
    test_fragmentation_ratio();

    uballoc::reset();
    cleanup_all_distributed_shms(2, "introtest");
    unsetenv("UBALLOC_HEAP_ID");

    std::cout << "\n=== introspection tests passed ===" << std::endl;
    return 0;
}
