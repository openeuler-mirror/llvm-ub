// SPDX-License-Identifier: Apache-2.0

#include "uballoc.hpp"

static void test_issue5_owned_array_size_matches_thread_array() {
    std::cout << "[fix] Issue 5: Owned array allocation matches ThreadArray size" << std::endl;

    size_t layout_owned_entries = 0;
    size_t thread_array_size = uballoc::ThreadArray<uballoc::AllocatorOwned>::SIZE;

    auto sl = uballoc::compute_distributed_shared_region_layout(0, 2, 128, 256, 16);

    size_t owned_bytes_in_layout = sl.total_size - sl.owned_array_offset;
    layout_owned_entries = owned_bytes_in_layout / sizeof(uballoc::AllocatorOwned);

    std::cout << "  ThreadArray<AllocatorOwned>::SIZE: " << thread_array_size << " entries" << std::endl;
    std::cout << "  Owned region bytes: " << owned_bytes_in_layout << std::endl;
    std::cout << "  Owned region entries: " << layout_owned_entries << std::endl;

    assert(layout_owned_entries >= thread_array_size);
    assert(owned_bytes_in_layout >= sizeof(uballoc::ThreadArray<uballoc::AllocatorOwned>));

    std::cout << "  Verified: owned region allocation >= ThreadArray size (no OOB)" << std::endl;
}

int main() {
    test_issue5_owned_array_size_matches_thread_array();
    std::cout << "\n=== Issue 5 test passed ===" << std::endl;
    return 0;
}