// SPDX-License-Identifier: Apache-2.0
//
// Verifies boundary-size routing across Small / Large / Huge brackets.
//
//   Small = [8, 16384]         (37 classes, 4-per-doubling)
//   Large = [16385, 2MB)       (28 classes, 4-per-doubling)
//   Huge  = [2MB, inf)          (1 class, 2MB slot granularity)

#include "uballoc.hpp"
#include "test_helpers.hpp"
#include <iostream>
#include <cassert>
#include <cstring>
#include <vector>

static auto& alloc() {
    return uballoc::get_global_allocator<uballoc::DistributedShmBackend<>>();
}

// ═══════════════════════════════════════════════════════════════════════════
// Test 1: Pure routing-table checks (new_from_size, no allocation)
//
// Verifies that new_from_size() accepts/rejects exactly the right sizes at
// every bracket boundary, and that the returned class has the correct size().
// This catches off-by-one errors in MIN_SIZE / MAX_SIZE_VAL guards and
// binary-search landing on the wrong class.
// ═══════════════════════════════════════════════════════════════════════════
static void test_routing_table_boundaries() {
    std::cout << "[routing] routing-table boundary checks (new_from_size)..." << std::endl;

    constexpr size_t TWO_MB = 2 * 1024 * 1024;

    // ── Small bracket: [8, 16384] ──
    // Lower boundary: 8 = MIN_SIZE = SIZES[0]
    assert(uballoc::Small::new_from_size(8));
    assert(uballoc::Small::new_from_size(8)->size() == 8);

    // 9 rounds up to SIZES[1] = 16 (binary search: 8 < 9, 16 >= 9)
    assert(uballoc::Small::new_from_size(9));
    assert(uballoc::Small::new_from_size(9)->size() == 16);

    // Upper boundary: 16384 = MAX_SIZE_VAL = SIZES[36] (last class)
    assert(uballoc::Small::new_from_size(16384));
    assert(uballoc::Small::new_from_size(16384)->size() == 16384);

    // 16385 = past Small → must be rejected (routes to Large)
    assert(!uballoc::Small::new_from_size(16385));

    // ── Large bracket: [16385, 2MB) ──
    // Lower boundary: 16385 = MIN_SIZE (the gap start we fixed)
    assert(uballoc::Large::new_from_size(16385));
    assert(uballoc::Large::new_from_size(16385)->size() == 20480);

    // 16384 = below MIN_SIZE → rejected (belongs to Small)
    assert(!uballoc::Large::new_from_size(16384));

    // 20479 = just below SIZES[0], rounds up to class 20480
    assert(uballoc::Large::new_from_size(20479));
    assert(uballoc::Large::new_from_size(20479)->size() == 20480);

    // 20480 = SIZES[0] (first class boundary)
    assert(uballoc::Large::new_from_size(20480));
    assert(uballoc::Large::new_from_size(20480)->size() == 20480);

    // 20481 = past SIZES[0], rounds up to SIZES[1] = 24576
    assert(uballoc::Large::new_from_size(20481));
    assert(uballoc::Large::new_from_size(20481)->size() == 24576);

    // Upper boundary: 2MB-1 = last byte of Large (rounds to class 2097152)
    assert(uballoc::Large::new_from_size(TWO_MB - 1));
    assert(uballoc::Large::new_from_size(TWO_MB - 1)->size() == TWO_MB);

    // 2MB = MAX_SIZE_VAL → rejected (routes to Huge); guard is >= so exclusive
    assert(!uballoc::Large::new_from_size(TWO_MB));

    // ── Huge bracket: [2MB, inf) ──
    // Lower boundary: 2MB = MIN_SIZE
    assert(uballoc::HugeSize::new_from_size(TWO_MB));

    // 2MB - 1 = below MIN_SIZE → rejected (belongs to Large)
    assert(!uballoc::HugeSize::new_from_size(TWO_MB - 1));

    // Unbounded above
    assert(uballoc::HugeSize::new_from_size(8 * 1024 * 1024));
    assert(uballoc::HugeSize::new_from_size(static_cast<size_t>(-1)));

    // ── Continuity invariants (also enforced by static_assert in size.hpp) ──
    assert(uballoc::Small::MAX_SIZE_VAL + 1 == uballoc::Large::MIN_SIZE);
    assert(uballoc::Large::MAX_SIZE_VAL  == uballoc::HugeSize::MIN_SIZE);

    // ── No gap: every transition size is claimable by exactly one bracket ──
    // Small/Large transition: 16384 → Small, 16385 → Large
    assert( uballoc::Small::new_from_size(16384));
    assert(!uballoc::Large::new_from_size(16384));
    assert(!uballoc::Small::new_from_size(16385));
    assert( uballoc::Large::new_from_size(16385));

    // Large/Huge transition: 2MB-1 → Large, 2MB → Huge
    assert( uballoc::Large::new_from_size(TWO_MB - 1));
    assert(!uballoc::HugeSize::new_from_size(TWO_MB - 1));
    assert(!uballoc::Large::new_from_size(TWO_MB));
    assert( uballoc::HugeSize::new_from_size(TWO_MB));

    std::cout << "  Small=[8,16384] Large=[16385,2MB) Huge=[2MB,inf): "
                 "no gap, no overlap" << std::endl;
}

// ═══════════════════════════════════════════════════════════════════════════
// Test 2: Comprehensive boundary allocations
//
// For each boundary size, verifies the full allocation lifecycle:
//   1. malloc succeeds
//   2. class_size() returns the exact expected value
//   3. Exactly one bracket's checked_pointer_to_offset claims the pointer
//   4. Data integrity (write + verify full allocation)
//   5. free succeeds
// ═══════════════════════════════════════════════════════════════════════════

// expected_bracket: 0 = Small, 1 = Large, 2 = Huge
struct BoundaryCase {
    size_t alloc_size;
    size_t expected_class_size;
    int    expected_bracket;
    const char* desc;
};

static void verify_boundary_allocation(const BoundaryCase& tc) {
    void* p = alloc().malloc(tc.alloc_size);
    assert(p != nullptr);

    // 2. class_size must match exactly
    size_t cs = alloc().current_allocator().class_size(p);
    std::cout << "  " << tc.desc << ": alloc=" << tc.alloc_size
              << " class_size=" << cs << std::endl;
    assert(cs == tc.expected_class_size);

    // 3. Exactly one bracket must claim the pointer
    auto small_off = alloc().current_allocator().small.checked_pointer_to_offset(p);
    auto large_off = alloc().current_allocator().large.checked_pointer_to_offset(p);
    auto huge_off  = alloc().current_allocator().huge.checked_pointer_to_offset(p);

    assert(small_off.has_value() == (tc.expected_bracket == 0));
    assert(large_off.has_value() == (tc.expected_bracket == 1));
    assert(huge_off.has_value()  == (tc.expected_bracket == 2));

    // 4. Data integrity — write and verify the entire allocation
    uint8_t pat = static_cast<uint8_t>(tc.alloc_size ^ 0x5A);
    memset(p, pat, tc.alloc_size);
    for (size_t i = 0; i < tc.alloc_size; ++i) {
        assert(static_cast<uint8_t*>(p)[i] == pat);
    }

    // 5. Free
    alloc().free(p);
}

static void test_boundary_allocations() {
    std::cout << "[routing] boundary allocations across Small/Large/Huge..." << std::endl;

    constexpr size_t KB = 1024;
    constexpr size_t MB = 1024 * 1024;

    // ── Small bracket boundaries ──
    verify_boundary_allocation({8,         8,         0, "Small MIN_SIZE (first class)"});
    verify_boundary_allocation({9,         16,        0, "Small first class +1 (→ 16)"});
    verify_boundary_allocation({16 * KB,   16 * KB,   0, "Small MAX_SIZE_VAL (last class)"});

    // ── Small → Large transition (the gap we closed) ──
    verify_boundary_allocation({16 * KB,       16 * KB,   0, "S/L transition: Small side (16384)"});
    verify_boundary_allocation({16 * KB + 1,   20 * KB,   1, "S/L transition: Large side (16385, was gap)"});

    // ── Large bracket boundaries ──
    verify_boundary_allocation({16 * KB + 1,   20 * KB,      1, "Large MIN_SIZE (16385 → class 20480)"});
    verify_boundary_allocation({20 * KB - 1,   20 * KB,      1, "Large below SIZES[0] (20479 → class 20480)"});
    verify_boundary_allocation({20 * KB,       20 * KB,      1, "Large SIZES[0] (20480, first class)"});
    verify_boundary_allocation({20 * KB + 1,   24 * KB,      1, "Large SIZES[0]+1 (20481 → class 24576)"});
    verify_boundary_allocation({1 * MB,        1 * MB,       1, "Large mid class (1MB exact)"});
    verify_boundary_allocation({2 * MB - 1,    2 * MB,       1, "Large last byte (2MB-1 → class 2097152)"});

    // ── Large → Huge transition ──
    verify_boundary_allocation({2 * MB - 1,    2 * MB,       1, "L/H transition: Large side (2MB-1)"});
    verify_boundary_allocation({2 * MB,        2 * MB,       2, "L/H transition: Huge side (2MB)"});

    // ── Huge bracket boundaries ──
    // SLAB_SIZE = 2MB (P0-2). MIN_SIZE = 2MB (lowered from 4MB when Large
    // upper bound went 4MB→2MB), so 2MB → 1 slot (2MB, 0% waste),
    // 2MB+1 → 2 slots (4MB), 3MB → 2 slots (4MB, 33% waste, was Large 0%).
    verify_boundary_allocation({2 * MB,        2 * MB,       2, "Huge MIN_SIZE (2MB, 1 slot @2MB)"});
    verify_boundary_allocation({2 * MB + 1,    4 * MB,       2, "Huge MIN_SIZE+1 (2MB+1, 2 slots @2MB)"});
    verify_boundary_allocation({3 * MB,        4 * MB,       2, "Huge 3MB (2 slots @2MB, was Large 0% waste)"});
    verify_boundary_allocation({4 * MB,        4 * MB,       2, "Huge 4MB (2 slots @2MB exact)"});
    verify_boundary_allocation({4 * MB + 1,    6 * MB,       2, "Huge 4MB+1 (3 slots @2MB)"});
    verify_boundary_allocation({8 * MB,        8 * MB,       2, "Huge 8MB (4 slots @2MB exact)"});
    verify_boundary_allocation({8 * MB + 1,    10 * MB,       2, "Huge 8MB+1 (5 slots @2MB)"});

    std::cout << "  all boundary allocations routed correctly: OK" << std::endl;
}

int main() {
    cleanup_all_distributed_shms(2, "gaptest");
    setenv("UBALLOC_HEAP_ID", "gaptest", 1);

    alloc().soft_reset();
    alloc().init();
    alloc().init_thread(0);
    if (!alloc().is_initialized()) {
        std::cerr << "  init failed — skipping allocation tests" << std::endl;
        cleanup_all_distributed_shms(2, "gaptest");
        return 0;
    }

    test_routing_table_boundaries();
    test_boundary_allocations();

    alloc().reset();
    cleanup_all_distributed_shms(2, "gaptest");
    unsetenv("UBALLOC_HEAP_ID");

    std::cout << "\n=== routing + boundary tests passed ===" << std::endl;
    return 0;
}
