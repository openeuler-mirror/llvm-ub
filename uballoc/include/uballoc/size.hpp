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
    static constexpr size_t MIN_SIZE = 20480;
    static constexpr size_t COUNT = 32;
    static constexpr size_t MAX_SIZE_VAL = 4 * 1024 * 1024;
    
    using ArrayType = std::array<uint64_t, COUNT>;
    using BitSetType = BitSet<64>;
    
    // 4-per-doubling size class table. See Small::SIZES for the PERFORMANCE
    // WARNING about new_from_size()'s binary search and recommended O(1)
    // optimizations (bit-trick variant for Large).
    static constexpr std::array<uint64_t, COUNT> SIZES = {
        20480, 24576, 28672, 32768,
        40960, 49152, 57344, 65536,
        81920, 98304, 114688, 131072,
        163840, 196608, 229376, 262144,
        327680, 393216, 458752, 524288,
        655360, 786432, 917504, 1048576,
        1310720, 1572864, 1835008, 2097152,
        2621440, 3145728, 3670016, 4194304
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
    // FRAGMENTATION WARNING: 4MB slot size causes significant internal waste
    // for non-multiple-of-4MB Huge allocations. Example: 5MB alloc → 2 slots
    // = 8MB → 3MB waste (37.5%). Worst case at the boundary (4MB+1 byte) is
    // ~50% waste (2 slots = 8MB for 4MB+1 of data).
    //
    // This granularity is COARSER than necessary. UBSE's shm granularity is
    // 2MB above the 4MB minimum (see UBShmProvider::shm_size_granularity in
    // ubshm_provider.hpp: 4MB min, 2MB multiples above). POSIX has no such
    // constraint. So 4MB slots leave shm space on the table for sub-4MB
    // remainders.
    //
    // RECOMMENDED OPTIMIZATION (not yet implemented): reduce SLAB_SIZE to
    // 2MB, keep MIN_SIZE = 4MB. Effects:
    //   - Worst-case internal waste halves (~50% → ~33% at the 4MB+1 byte
    //     boundary; 5MB → 6MB not 8MB; 6MB → 6MB not 8MB; 9MB → 10MB not
    //     12MB).
    //   - Aligns slot size with UBSE shm granularity — no wasted shm space.
    //   - slot_count doubles in HugeShared bitmap (negligible metadata cost;
    //     Huge regions are 100s of MB).
    //   - class_size_from_offset returns less inflated values → realloc-shrink
    //     threshold triggers earlier (more accurate behavior; realloc(p, 6MB)
    //     on a 5MB alloc would now expand since class_size=6MB, vs current
    //     class_size=8MB which in-place shrinks).
    //   - Boundary discontinuity remains (4MB → 4MB+1 jumps to 6MB, not 4MB)
    //     but only half as bad as today.
    //   - Requires LAYOUT_VERSION bump (4 → 5) — incompatible with current
    //     Huge shms; tests must clean up before running.
    // See Phase 3 design notes for full analysis of alternatives considered
    // (Option B: lower MIN_SIZE to 2MB — drops Large's top 5 classes;
    //  Option C: variable slot size — breaks contiguous-allocation invariant;
    //  Option D: 1MB slot — sub-UBSE-granularity, wastes shm space).
    static constexpr size_t SLAB_SIZE = 4 * 1024 * 1024;
    static constexpr size_t MIN_SIZE = 4 * 1024 * 1024;
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

}