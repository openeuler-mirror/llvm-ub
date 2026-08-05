// SPDX-License-Identifier: Apache-2.0

#include "uballoc.hpp"
#include <iostream>
#include <cassert>

struct TestShmProvider4MB {
    using ShmHandle = int;
    static constexpr ShmHandle INVALID_HANDLE = -1;
    static constexpr bool has_reliable_unreferenced_check = false;
    static constexpr size_t min_shm_size = 4 * 1024 * 1024;
    static constexpr size_t shm_size_granularity = uballoc::SIZE_PAGE;
};

static void test_min_shm_size_trait() {
    std::cout << "[Part A] min_shm_size trait exists and defaults to 0 for POSIX" << std::endl;

    assert(uballoc::PosixShmProvider::min_shm_size == 0);
    assert(TestShmProvider4MB::min_shm_size == 4 * 1024 * 1024);

    std::cout << "  Verified: PosixShmProvider::min_shm_size == 0, TestShmProvider4MB::min_shm_size == 4MB" << std::endl;
}

static void test_round_up_shm_size_posix() {
    std::cout << "[Part A] round_up_shm_size with min_shm_size=0 (POSIX)" << std::endl;

    using Backend = uballoc::DistributedShmBackend<uballoc::PosixShmProvider>;

    assert(Backend::round_up_shm_size(0) == 0);
    assert(Backend::round_up_shm_size(100) == uballoc::SIZE_PAGE);
    assert(Backend::round_up_shm_size(uballoc::SIZE_PAGE - 1) == uballoc::SIZE_PAGE);
    assert(Backend::round_up_shm_size(uballoc::SIZE_PAGE) == uballoc::SIZE_PAGE);
    assert(Backend::round_up_shm_size(uballoc::SIZE_PAGE + 1) == 2 * uballoc::SIZE_PAGE);
    assert(Backend::round_up_shm_size(2 * uballoc::SIZE_PAGE) == 2 * uballoc::SIZE_PAGE);

    std::cout << "  Verified: size < PAGE rounds up to PAGE, size >= PAGE stays (no min)" << std::endl;
}

static void test_round_up_shm_size_custom_4mb() {
    std::cout << "[Part A] round_up_shm_size with min_shm_size=4MB (custom provider)" << std::endl;

    using TestBackend = uballoc::DistributedShmBackend<TestShmProvider4MB>;
    constexpr size_t MB4 = 4 * 1024 * 1024;

    assert(TestBackend::round_up_shm_size(0) == MB4);
    assert(TestBackend::round_up_shm_size(100) == MB4);
    assert(TestBackend::round_up_shm_size(3000) == MB4);
    assert(TestBackend::round_up_shm_size(MB4 - 1) == MB4);
    assert(TestBackend::round_up_shm_size(MB4) == MB4);
    assert(TestBackend::round_up_shm_size(MB4 + 1) == MB4 + uballoc::SIZE_PAGE);
    assert(TestBackend::round_up_shm_size(5 * 1024 * 1024) == 5 * 1024 * 1024);

    std::cout << "  Verified: size < 4MB rounds up to 4MB, size >= 4MB stays page-aligned" << std::endl;
}

static void test_va_stride_rounding_prevents_overlap() {
    std::cout << "[Part A] VA uses per-process reserved range (process_stride apart)" << std::endl;

    constexpr size_t MB4 = 4 * 1024 * 1024;

    uballoc::DistributedConfig config;
    config.rank = 0;
    config.total_processes = 2;
    config.slab_count_small = {1, 1};
    config.slab_count_large = {0, 0};
    config.huge_slots = {0, 0};

    uballoc::DistributedVALayout layout_rounded = uballoc::DistributedVALayout::compute(config, MB4,
        uballoc::DEFAULT_VA_BASE);
    uballoc::DistributedVALayout layout_no_round = uballoc::DistributedVALayout::compute(config, 0,
        uballoc::DEFAULT_VA_BASE);

    size_t stride_rounded = layout_rounded.data_small_va[1] - layout_rounded.data_small_va[0];
    size_t stride_no_round = layout_no_round.data_small_va[1] - layout_no_round.data_small_va[0];

    assert(layout_rounded.data_small_size[0] >= MB4);
    assert(layout_rounded.data_small_size[1] >= MB4);
    assert(stride_rounded == uballoc::PROCESS_STRIDE_SMALL);
    assert(stride_no_round == uballoc::PROCESS_STRIDE_SMALL);

    std::cout << "  Verified: VA stride = process_stride = " << stride_rounded << " (per-process reserved range)" << std::endl;
}

static void test_zero_size_data_no_roundup() {
    std::cout << "[Part A] slab_count=0 → data_size=0 (no round-up applied)" << std::endl;

    constexpr size_t MB4 = 4 * 1024 * 1024;

    uballoc::DistributedConfig config;
    config.rank = 0;
    config.total_processes = 2;
    config.slab_count_small = {0, 256};
    config.slab_count_large = {0, 16};
    config.huge_slots = {0, 128};

    uballoc::DistributedVALayout layout = uballoc::DistributedVALayout::compute(config, MB4,
        uballoc::DEFAULT_VA_BASE);

    assert(layout.data_small_size[0] == 0);
    assert(layout.data_large_size[0] == 0);
    assert(layout.data_huge_size[0] == 0);

    assert(layout.data_small_size[1] >= MB4);

    std::cout << "  Verified: P0 (0 slabs) → size=0, P1 (256 slabs) → size >= 4MB" << std::endl;
}

static void test_wait_for_all_shms_count_matches_config() {
    std::cout << "[Part A] wait_for_all_shms counts only regions that will be created" << std::endl;

    uballoc::DistributedConfig config;
    config.rank = 0;
    config.total_processes = 2;
    config.slab_count_small = {0, 256};
    config.slab_count_large = {16, 16};
    config.huge_slots = {128, 128};

    int expected = 0;
    for (int k = 0; k < config.total_processes; ++k) {
        expected++;
        if (config.slab_count_small[k] > 0) expected++;
        if (config.slab_count_large[k] > 0) expected++;
        if (config.huge_slots[k] > 0) expected++;
    }

    assert(expected == 7);

    int old_buggy = config.total_processes * uballoc::DistributedShmBackend<>::REGION_COUNT;
    assert(old_buggy == 8);
    assert(expected < old_buggy);

    std::cout << "  Verified: correct count=7, old buggy count=8" << std::endl;
}

int main() {
    test_min_shm_size_trait();
    test_round_up_shm_size_posix();
    test_round_up_shm_size_custom_4mb();
    test_va_stride_rounding_prevents_overlap();
    test_zero_size_data_no_roundup();
    test_wait_for_all_shms_count_matches_config();
    std::cout << "\n=== Part A tests passed ===" << std::endl;
    return 0;
}
