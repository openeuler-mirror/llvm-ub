// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <cstddef>
#include <atomic>

namespace uballoc {

// ---------------------------------------------------------------------------
// Million-type-id published registry (see million_type_id_registry.md).
//
// A single shared direct-indexed array: entries[type_id]. Each entry is
// 16 bytes (two uint64_t atomics). The registry lives in a dedicated shm
// mapped at a fixed VA offset after the Huge data area. P0 creates it;
// all processes discover it via the BootstrapBlock.
// ---------------------------------------------------------------------------

// One published entry — 16 bytes, naturally aligned.
//
// meta word bit layout:
//   [63]    active (1 = published, 0 = free or mid-publish)
//   [62:60] owner rank (0-7)
//   [59:0]  address offset from va_base (45 bits needed, 60 available)
//
// meta == 0  →  inactive (slot free)
//
// The address is stored as an offset from va_base (not a full pointer)
// so that offset 0 is never a valid data address — it falls in the
// metadata area, making meta == 0 an unambiguous "free" sentinel.
struct PublishedEntryV2 {
    std::atomic<uint64_t> meta;
    std::atomic<uint64_t> size;
};

static_assert(sizeof(PublishedEntryV2) == 16, "PublishedEntryV2 must be 16 bytes");
static_assert(alignof(PublishedEntryV2) == 8, "PublishedEntryV2 must be 8-byte aligned");

// Meta word field extraction/packing.
constexpr uint64_t PUBLISHED_ACTIVE_BIT  = 1ULL << 63;
constexpr int      PUBLISHED_OWNER_SHIFT = 60;
constexpr uint64_t PUBLISHED_OWNER_MASK  = 0x7ULL << 60;
constexpr uint64_t PUBLISHED_ADDR_MASK   = (1ULL << 60) - 1;

// Default registry capacity (entries). Override via UBALLOC_MAX_TYPE_ID.
constexpr uint32_t DEFAULT_MAX_TYPE_ID = 1'000'000;

// VA space reserved for the registry (must cover the largest capacity
// the user might configure). 4 GB accommodates 256M entries.
constexpr size_t REGISTRY_VA_SIZE = 4ULL << 30;  // 4 GB

// Return type of lookup_by_type / lookup_by_address (unchanged from V1).
struct PublishedInfo {
    void* address = nullptr;
    size_t size = 0;
    uint32_t type_id = 0;
    int owner_process = -1;
};

} // namespace uballoc
