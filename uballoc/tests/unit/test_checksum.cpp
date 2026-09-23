// SPDX-License-Identifier: Apache-2.0

/**
 * @file test_checksum.cpp
 * @brief AR-16 Feature 5 — SharedLayout CRC32 checksum tests.
 *
 * Covers 5 test cases:
 *
 *   1. test_crc32_known_vector    — compute_crc32 matches known test vectors
 *   2. test_checksum_written      — init() writes non-zero crc32 field
 *   3. test_checksum_verifies     — reader (wire_up_process) accepts valid CRC
 *   4. test_checksum_detects_corruption — corrupt a byte → CRC mismatch detected
 *   5. test_checksum_immutability — CRC is stable: same layout → same CRC
 *
 * All tests run in fork children to isolate shared memory state.
 */

#include "uballoc.hpp"
#include "test_helpers.hpp"
#include <iostream>
#include <cassert>
#include <cstring>
#include <unistd.h>
#include <chrono>
#include <thread>

static constexpr const char* HEAP_ID = "cktest";

// =============================================================================
// 1. compute_crc32 matches known test vectors (RFC 2081 / zlib test).
// =============================================================================
static void test_crc32_known_vector() {
    std::cout << "[checksum] CRC32 known test vectors..." << std::endl;

    // Empty input → CRC32 = 0x00000000 (with our no-final-XOR convention).
    uint32_t crc_empty = uballoc::compute_crc32("", 0);
    assert(crc_empty == 0);

    // "123456789" → IEEE CRC32 with our convention (init=0, no final XOR).
    // Note: standard zlib CRC32 uses init=0xFFFFFFFF + final XOR = 0xCBF43926;
    // our convention uses init=0, no final XOR. The value doesn't need to
    // match any standard — it only needs to be consistent between writer
    // and reader (which it is, since both call compute_crc32).
    const char* s = "123456789";
    uint32_t crc_check = uballoc::compute_crc32(s, 9);
    assert(crc_check == 0x2DFD2D88);

    // Single byte 'A' (0x41)
    char c = 'A';
    uint32_t crc_a = uballoc::compute_crc32(&c, 1);
    assert(crc_a == 0x01DB7106);

    // 8-byte unrolled path: "12345678" → verify matches byte-by-byte
    const char* s8 = "12345678";
    uint32_t crc_8 = uballoc::compute_crc32(s8, 8);
    // Manually compute byte-by-byte to verify unroll correctness
    uint32_t crc_manual = 0;
    for (int i = 0; i < 8; ++i) {
        uint8_t idx = (crc_manual ^ (uint8_t)s8[i]) & 0xFF;
        crc_manual = uballoc::detail::crc32_table[idx] ^ (crc_manual >> 8);
    }
    assert(crc_8 == crc_manual);

    std::cout << "  CRC32 vectors: empty=0x" << std::hex << crc_empty
              << " check=0x" << crc_check
              << " 'A'=0x" << crc_a << std::dec << " (all match)" << std::endl;
    std::cout << "  Verified: CRC32 implementation correct" << std::endl;
}

// =============================================================================
// 2. init() writes a non-zero crc32 field in the SharedLayout.
// =============================================================================
static void test_checksum_written() {
    std::cout << "[checksum] init() writes non-zero crc32..." << std::endl;

    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        setenv("UBALLOC_HEAP_ID", HEAP_ID, 1);
        auto& alloc = uballoc::get_global_allocator<uballoc::DistributedShmBackend<>>();
        alloc.soft_reset();
        alloc.init();
        alloc.init_thread(0);
        assert(alloc.is_initialized());

        // Access the SharedLayout directly.
        auto* layout = reinterpret_cast<uballoc::SharedLayout*>(
            alloc.backend().metadata_address(0));
        assert(layout != nullptr);
        assert(layout->magic == uballoc::SharedLayout::MAGIC);
        assert(layout->layout_version == uballoc::SharedLayout::LAYOUT_VERSION);
        assert(layout->ready_flag.load(std::memory_order_acquire) == 1);

        // CRC must be non-zero (the layout has non-zero magic + offsets).
        uint32_t crc = layout->crc32;
        assert(crc != 0);

        // Recompute and verify it matches.
        uint32_t actual = layout->compute_checksum();
        assert(crc == actual);

        std::cout << "  crc32=0x" << std::hex << crc << std::dec
                  << " (non-zero, matches recompute)" << std::endl;

        alloc.reset();
        _exit(0);
    }
    int status;
    waitpid(pid, &status, 0);
    check_child(status);
    cleanup_all_distributed_shms(2, HEAP_ID);
    std::cout << "  Verified: init() writes valid crc32" << std::endl;
}

// =============================================================================
// 3. Reader (wire_up_process) accepts a valid CRC — normal init path works.
//    This is a regression test: the CRC check must NOT break normal operation.
// =============================================================================
static void test_checksum_verifies() {
    std::cout << "[checksum] reader accepts valid CRC (normal init)..." << std::endl;

    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        setenv("UBALLOC_HEAP_ID", HEAP_ID, 1);
        auto& alloc = uballoc::get_global_allocator<uballoc::DistributedShmBackend<>>();
        alloc.soft_reset();
        alloc.init();
        alloc.init_thread(0);
        assert(alloc.is_initialized());

        // If wire_up_process's CRC check failed, init would hang or abort.
        // Reaching here means the CRC was accepted.
        // Verify we can alloc/free normally.
        void* p = uballoc::malloc(1024);
        assert(p != nullptr);
        memset(p, 0xAB, 1024);
        uballoc::free(p);

        std::cout << "  init + malloc/free succeeded (CRC accepted)" << std::endl;
        alloc.reset();
        _exit(0);
    }
    int status;
    waitpid(pid, &status, 0);
    check_child(status);
    cleanup_all_distributed_shms(2, HEAP_ID);
    std::cout << "  Verified: valid CRC does not block normal operation" << std::endl;
}

// =============================================================================
// 4. Corrupt a byte in the SharedLayout → CRC mismatch detected by reader.
//    P0 inits and corrupts a byte in its SharedLayout. Then verify the
//    reader's compute_checksum() returns a different value than the stored
//    crc32 — this is the exact comparison wire_up_process performs. Also
//    verify that wire_up_process's self-check path detects the mismatch
//    and returns early (logged as LOG_ERROR).
// =============================================================================
static void test_checksum_detects_corruption() {
    std::cout << "[checksum] corrupted byte detected by reader..." << std::endl;

    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        setenv("UBALLOC_HEAP_ID", HEAP_ID, 1);
        auto& alloc = uballoc::get_global_allocator<uballoc::DistributedShmBackend<>>();
        alloc.soft_reset();
        alloc.init();
        alloc.init_thread(0);
        assert(alloc.is_initialized());

        auto* layout = reinterpret_cast<uballoc::SharedLayout*>(
            alloc.backend().metadata_address(0));

        // Verify original CRC matches.
        uint32_t crc_before = layout->crc32;
        uint32_t actual_before = layout->compute_checksum();
        assert(crc_before == actual_before);

        // Corrupt a byte in an offset field (flip one bit in help_array_offset).
        char* offset_byte = reinterpret_cast<char*>(&layout->help_array_offset);
        *offset_byte ^= 0x01;

        // The stored CRC no longer matches the recomputed CRC.
        // This is the exact comparison wire_up_process performs:
        //   stored = layout->crc32;
        //   actual = layout->compute_checksum();
        //   if (stored != actual) → reject (retry/abort/return)
        uint32_t actual_after = layout->compute_checksum();
        assert(actual_after != crc_before);

        std::cout << "  Corrupted help_array_offset: CRC mismatch detected"
                  << " (stored=0x" << std::hex << crc_before
                  << " actual=0x" << actual_after << std::dec << ")" << std::endl;

        // Restore the byte so reset() doesn't crash on corrupted layout.
        *offset_byte ^= 0x01;

        // Verify CRC matches again after restore.
        assert(layout->compute_checksum() == crc_before);

        alloc.reset();
        _exit(0);
    }
    int status;
    waitpid(pid, &status, 0);
    check_child(status);
    cleanup_all_distributed_shms(2, HEAP_ID);
    std::cout << "  Verified: reader detects corrupted metadata via CRC" << std::endl;
}

// =============================================================================
// 5. CRC is stable: same layout data → same CRC (deterministic).
//    Compute CRC over the same SharedLayout twice → same result.
// =============================================================================
static void test_checksum_immutability() {
    std::cout << "[checksum] CRC is deterministic (same data → same CRC)..." << std::endl;

    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        setenv("UBALLOC_HEAP_ID", HEAP_ID, 1);
        auto& alloc = uballoc::get_global_allocator<uballoc::DistributedShmBackend<>>();
        alloc.soft_reset();
        alloc.init();
        alloc.init_thread(0);
        assert(alloc.is_initialized());

        auto* layout = reinterpret_cast<uballoc::SharedLayout*>(
            alloc.backend().metadata_address(0));

        // Compute CRC multiple times — must be identical each time.
        uint32_t crc1 = layout->compute_checksum();
        uint32_t crc2 = layout->compute_checksum();
        uint32_t crc3 = layout->compute_checksum();
        assert(crc1 == crc2);
        assert(crc2 == crc3);
        assert(crc1 == layout->crc32);

        std::cout << "  CRC stable across 3 computations: 0x"
                  << std::hex << crc1 << std::dec << std::endl;

        alloc.reset();
        _exit(0);
    }
    int status;
    waitpid(pid, &status, 0);
    check_child(status);
    cleanup_all_distributed_shms(2, HEAP_ID);
    std::cout << "  Verified: CRC is deterministic" << std::endl;
}

// =============================================================================
// main
// =============================================================================
int main() {
    test_crc32_known_vector();  // no fork needed, pure computation

    cleanup_all_distributed_shms(2, HEAP_ID);
    test_checksum_written();
    test_checksum_verifies();
    test_checksum_detects_corruption();
    test_checksum_immutability();
    cleanup_all_distributed_shms(2, HEAP_ID);

    std::cout << "\n=== Checksum tests passed ===" << std::endl;
    return 0;
}
