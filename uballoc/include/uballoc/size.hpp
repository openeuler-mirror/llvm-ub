// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <cstddef>
#include <array>
#include <optional>
#include <cassert>
#include <type_traits>

#include "packed.hpp"
#include "bitset.hpp"
#include "cache.hpp"

namespace uballoc {

template<typename B>
struct BracketTraits {
    static constexpr const char* name = "";
    static constexpr size_t slab_size = 0;
    static constexpr size_t min_size = 0;
    static constexpr size_t max_size = 0;
    static constexpr size_t count = 0;
};

class Small {
public:
    static constexpr const char* NAME = "small";
    static constexpr size_t SLAB_SIZE = uballoc::SIZE_CACHE_LINE * 8 * 8 * 8;
    static constexpr size_t SLAB_CAPACITY = 32768;
    static constexpr size_t RESERVATION_SIZE = SLAB_CAPACITY * SLAB_SIZE;
    static constexpr size_t MIN_SIZE = 8;
    static constexpr size_t MAX_SIZE_VAL = 16384;
    static constexpr size_t COUNT = 37;
    
    using ArrayType = std::array<uint64_t, COUNT>;
    using BitSetType = BitSet<(SIZE_CACHE_LINE * 8 - BITSET_METADATA_SIZE - 8) / 8>;
    
    // 4-per-doubling size class table (jemalloc-style, ~25% max internal waste).
    //
    // PERFORMANCE WARNING: new_from_size() does a binary search over SIZES on
    // every malloc (~6 cmps for Small, ~5 for Large) — ~3-5ns added to the
    // hot path vs Phase 1's O(1) arithmetic (Small: (size+7)/8; Large:
    // MIN_SIZE<<i). Acceptable for now since slab metadata cache miss +
    // atomic CAS dominate the malloc path, but MUST be optimized if malloc
    // latency becomes critical. Recommended approaches (in priority order):
    //
    //   1. Small — direct-lookup table: replace binary search with a 2KB
    //      uint8_t[2048] table indexed by (size-1)/8. O(1), one cache-line
    //      touch. Tiny .rodata cost. Easiest win.
    //   2. Large — bit tricks: lg_floor(size) gives the doubling index, then
    //      2 high bits of (size-1) within the doubling give the offset.
    //      O(1), ~64 bytes of state. Harder to derive, see Phase 2 design
    //      notes for the formula.
    //
    // size() itself is O(1) (direct index) and is NOT a concern.
    static constexpr std::array<uint64_t, COUNT> SIZES = {
        8, 16, 32, 48, 64, 80, 96, 112, 128,
        160, 192, 224, 256, 320, 384, 448, 512,
        640, 768, 896, 1024, 1280, 1536, 1792, 2048,
        2560, 3072, 3584, 4096, 5120, 6144, 7168, 8192,
        10240, 12288, 14336, 16384
    };
    
private:
    u7 value_;
    
public:
    constexpr Small() : value_(0) {}
    constexpr explicit Small(u7 v) : value_(v) {}
    
    constexpr static std::optional<Small> new_from_size(size_t size) {
        if (size > MAX_SIZE_VAL) return std::nullopt;
        size_t lo = 0, hi = COUNT - 1;
        while (lo < hi) {
            size_t mid = (lo + hi) / 2;
            if (SIZES[mid] < size) lo = mid + 1;
            else hi = mid;
        }
        return Small(u7(lo));
    }
    
    constexpr static std::optional<Small> from_index(size_t idx) {
        if (idx >= COUNT) return std::nullopt;
        return Small(u7(idx));
    }
    
    constexpr bool is_zero() const { return false; }
    constexpr uint64_t size() const { return SIZES[value_]; }
    
    uint64_t count() const {
        static const std::array<uint16_t, COUNT> counts = init_counts();
        return counts[value_];
    }
    
    constexpr bool operator==(const Small& other) const { return value_ == other.value_; }
    constexpr bool operator!=(const Small& other) const { return value_ != other.value_; }
    
    constexpr static uint64_t pack(const Small& s) { return s.value_; }
    constexpr static Small unpack(uint64_t raw) { return Small(u7(raw)); }
    
    constexpr uint8_t index() const { return value_; }
    
private:
    static std::array<uint16_t, COUNT> init_counts() {
        std::array<uint16_t, COUNT> arr{};
        const size_t bitset_cap = BitSetType::SIZE_DATA * 8;
        for (size_t i = 0; i < COUNT; ++i) {
            size_t fit = SLAB_SIZE / SIZES[i];
            arr[i] = static_cast<uint16_t>(fit < bitset_cap ? fit : bitset_cap);
        }
        return arr;
    }
};

struct SmallTrait {
    static constexpr unsigned total_bits = 7;
};

class Large {
public:
    static constexpr const char* NAME = "large";
    static constexpr size_t SLAB_SIZE = 4 * 1024 * 1024;
    static constexpr size_t SLAB_CAPACITY = 512;
    static constexpr size_t RESERVATION_SIZE = SLAB_CAPACITY * SLAB_SIZE;
    static constexpr size_t MIN_SIZE = 16385;
    static constexpr size_t COUNT = 28;
    static constexpr size_t MAX_SIZE_VAL = 2 * 1024 * 1024;
    
    using ArrayType = std::array<uint64_t, COUNT>;
    using BitSetType = BitSet<64>;
    
    // 4-per-doubling size class table. See Small::SIZES for the PERFORMANCE
    // WARNING about new_from_size()'s binary search and recommended O(1)
    // optimizations (bit-trick variant for Large).
    //
    // Upper bound lowered from 4MB to 2MB: classes 2MB..4MB (2621440,
    // 3145728, 3670016, 4194304) were dropped, routing [2MB, 4MB) requests
    // to Huge (2MB slots). HugeSize::MIN_SIZE was lowered from 4MB to 2MB
    // to keep the Large/Huge boundary contiguous (no routing gap). COUNT went
    // 32→28 (still fits u5/32). Cost: [2MB,4MB) requests lose exact Large
    // classes (e.g. 3MB was 0% waste → now 2 Huge slots = 4MB, 33% waste).
    static constexpr std::array<uint64_t, COUNT> SIZES = {
        20480, 24576, 28672, 32768,
        40960, 49152, 57344, 65536,
        81920, 98304, 114688, 131072,
        163840, 196608, 229376, 262144,
        327680, 393216, 458752, 524288,
        655360, 786432, 917504, 1048576,
        1310720, 1572864, 1835008, 2097152
    };
    
private:
    u5 value_;
    
public:
    constexpr Large() : value_(0) {}
    constexpr explicit Large(u5 v) : value_(v) {}
    
    constexpr static std::optional<Large> new_from_size(size_t size) {
        if (size >= MAX_SIZE_VAL || size < MIN_SIZE) return std::nullopt;
        size_t lo = 0, hi = COUNT - 1;
        while (lo < hi) {
            size_t mid = (lo + hi) / 2;
            if (SIZES[mid] < size) lo = mid + 1;
            else hi = mid;
        }
        return Large(u5(lo));
    }
    
    constexpr static std::optional<Large> from_index(size_t idx) {
        if (idx >= COUNT) return std::nullopt;
        return Large(u5(idx));
    }
    
constexpr bool is_zero() const { return false; }
    
    constexpr uint64_t size() const { 
        return SIZES[value_];
    }
    
    constexpr uint64_t count() const {
        return SLAB_SIZE / size();
    }
    
    constexpr bool operator==(const Large& other) const { return value_ == other.value_; }
    constexpr bool operator!=(const Large& other) const { return value_ != other.value_; }
    
    constexpr static uint64_t pack(const Large& l) { return l.value_; }
    constexpr static Large unpack(uint64_t raw) { return Large(u5(raw)); }
    
    constexpr uint8_t index() const { return value_; }
    
private:
};

struct LargeTrait {
    static constexpr unsigned total_bits = 5;
};

class HugeSize {
public:
    static constexpr const char* NAME = "huge";
    // Slot granularity = 2MB
    //
    // Halving to 2MB reduces Huge rounding waste:
    //   - 4MB+1 byte: 2 slots (4MB) → 3 slots (6MB), worst-case waste ~50%
    //     (was ~50% at 8MB; still 50% ratio but 6MB < 8MB absolute).
    //   - 5MB: 2 slots (8MB, 60% waste) → 3 slots (6MB, 20% waste).
    //   - 6MB: 2 slots (8MB, 33% waste) → 3 slots (6MB, 0% waste).
    //   - 9MB: 3 slots (12MB, 33% waste) → 5 slots (10MB, 11% waste).
    //
    // UBSE safety: UBShmProvider::min_shm_size (4MB) is enforced at the shm
    // layer (see ubshm_provider.hpp:25), constraining segments (128MB ≫ 4MB),
    // not slots. POSIX has no minimum. So 2MB slots leave no shm space on
    // the table for sub-4MB remainders.
    //
    // MIN_SIZE lowered from 4MB to 2MB (Large upper-bound lowering): Large's
    // MAX_SIZE_VAL was lowered from 4MB to 2MB, so Huge must cover [2MB, inf)
    // to keep the Large/Huge boundary contiguous (no routing gap). A 2MB
    // request now routes to Huge (1 slot = 2MB, 0% waste) instead of Large's
    // 2MB class (2097152, also 0% waste) — footprint is identical, but Huge
    // is used for the [2MB, 4MB) range that Large no longer covers. The 4
    // Large classes dropped (2621440, 3145728, 3670016, 4194304) are replaced
    // by 2MB-slot Huge rounding: 3MB → 2 slots (4MB, 33% waste, was 0%).
    //
    // MAX_SIZE_VAL = SLAB_SIZE = 2MB = MIN_SIZE now. HugeSize::new_from_size
    // routes by `size >= MIN_SIZE`, never by MAX_SIZE_VAL (HugeSize has
    // COUNT=1, no class enumeration). MAX_SIZE_VAL is retained only for API
    // symmetry with Small/Large; it is unused in routing. The static_assert
    // invariants at the bottom of this file check Small↔Large and Large↔Huge
    // MIN_SIZE boundaries, not SLAB_SIZE.
    //
    // Cost: HUGE_SLOTS_PER_SEGMENT doubles (32→64) → MAX_HUGE_SLOTS doubles
    // → HugeShared dynamic slots array in per-process metadata doubles
    // (~2→4MB/process). Requires LAYOUT_VERSION bump (6→7).
    static constexpr size_t SLAB_SIZE = uballoc::HUGE_SLAB_SIZE;
    static constexpr size_t MIN_SIZE = 2 * 1024 * 1024;
    static constexpr size_t MAX_SIZE_VAL = SLAB_SIZE;
    static constexpr size_t COUNT = 1;
    
    using ArrayType = std::array<uint64_t, 1>;
    using BitSetType = BitSet<1>;
    
    constexpr HugeSize() {}
    
    constexpr static std::optional<HugeSize> new_from_size(size_t size) {
        if (size >= MIN_SIZE) return HugeSize();
        return std::nullopt;
    }
    
    constexpr static std::optional<HugeSize> from_index(size_t idx) {
        if (idx == 0) return HugeSize();
        return std::nullopt;
    }
    
    constexpr bool is_zero() const { return false; }
    constexpr uint64_t size() const { return SLAB_SIZE; }
    constexpr uint64_t count() const { return 1; }
    
    constexpr bool operator==(const HugeSize&) const { return true; }
    constexpr bool operator!=(const HugeSize&) const { return false; }
    
    constexpr static uint64_t pack(const HugeSize&) { return 0; }
    constexpr static HugeSize unpack(uint64_t) { return HugeSize(); }
};

struct HugeTrait {
    static constexpr unsigned total_bits = 0;
};

using SmallSize = Small;
using LargeSize = Large;

// Routing-continuity invariants: the three brackets must tile [8, inf) with
// no gaps and no overlap. Small covers [8, 16384], Large covers [16385, 2MB),
// Huge covers [2MB, inf). These guards catch the (16KB, 20KB) gap regression
// and any future boundary drift at compile time.
static_assert(Small::MAX_SIZE_VAL + 1 == Large::MIN_SIZE,
              "Small/Large boundary must be contiguous (no routing gap)");
static_assert(Large::MAX_SIZE_VAL == HugeSize::MIN_SIZE,
              "Large/Huge boundary must be contiguous (no routing gap)");

}
