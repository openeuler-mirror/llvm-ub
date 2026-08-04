// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <cstddef>
#include <array>
#include <atomic>
#include <optional>
#include <cassert>
#include <cstring>

#include "packed.hpp"
#include "cache.hpp"

namespace uballoc {

constexpr size_t BITSET_METADATA_SIZE = sizeof(uint64_t) * 2;

struct Bit {
    u6 col;
    u6 row;
    
    constexpr Bit() : col(0), row(0) {}
    constexpr Bit(u6 c, u6 r) : col(c), row(r) {}
    
    constexpr static Bit from_loose(uint16_t bit) {
        return Bit(u6(bit & 0x3F), u6(bit >> 6));
    }
    
    constexpr operator uint64_t() const {
        return static_cast<uint64_t>(col) | (static_cast<uint64_t>(row) << 6);
    }
    
    constexpr bool operator==(const Bit& other) const {
        return col == other.col && row == other.row;
    }
    constexpr bool operator!=(const Bit& other) const {
        return !(*this == other);
    }
    
    constexpr static uint64_t pack(const Bit& b) {
        return static_cast<uint64_t>(b);
    }
    constexpr static Bit unpack(uint64_t raw) {
        return Bit(u6(raw & 0x3F), u6((raw >> 6) & 0x3F));
    }
};

struct BitTrait {
    static constexpr unsigned total_bits = 12;
};

template<size_t Size>
struct BitSet {
    static_assert(Size <= 64);
    
    static constexpr size_t SIZE_DATA = sizeof(uint64_t) * Size;
    static constexpr size_t SIZE = SIZE_DATA + BITSET_METADATA_SIZE;
    
    uint64_t count;
    uint64_t sparse;
    std::array<uint64_t, Size> dense;
    
    constexpr BitSet() : count(0), sparse(0), dense{} {}
    
    void fill(uint64_t n) {
        uint64_t rows = n / 64;
        uint64_t cols = n % 64;
        
        for (size_t i = 0; i < rows; ++i) {
            dense[i] = ~0ULL;
        }
        
        sparse = (rows < 64) ? ((1ULL << rows) - 1) : ~0ULL;
        
        if (cols > 0) {
            dense[rows] = (1ULL << cols) - 1;
            sparse |= (1ULL << rows);
        }
        
        for (size_t j = rows + (cols > 0 ? 1 : 0); j < Size; ++j) {
            dense[j] = 0;
        }
        
        count = n;
    }
    
    Bit peek_unchecked() const {
        uint8_t row = __builtin_ctzll(sparse);
        uint8_t col = __builtin_ctzll(dense[row]);
        return Bit(u6(col), u6(row));
    }
    
    void set(Bit bit) {
        size_t row = bit.row;
        size_t col = bit.col;
        
        assert((dense[row] & (1ULL << col)) == 0);
        dense[row] |= (1ULL << col);
        flush(&dense[row], Invalidate::No);
        
        count++;
        sparse |= (1ULL << row);
    }
    
    void unset(Bit bit) {
        size_t row = bit.row;
        size_t col = bit.col;
        
        assert((dense[row] & (1ULL << col)) != 0);
        dense[row] &= ~(1ULL << col);
        flush(&dense[row], Invalidate::No);
        
        count--;
        if (dense[row] == 0) {
            sparse &= ~(1ULL << row);
        }
    }
    
    uint64_t len() const { return count; }
    bool is_empty() const { return count == 0; }
    
    void validate() const {
#ifdef UBALLOC_VALIDATE
        uint64_t actual = 0;
        for (const auto& d : dense) {
            actual += __builtin_popcountll(d);
        }
        assert(actual == count);
        
        for (size_t bit = 0; bit < Size; ++bit) {
            bool sparse_has = (sparse & (1ULL << bit)) != 0;
            bool dense_has = dense[bit] > 0;
            assert(sparse_has == dense_has);
        }
#endif
    }
};

template<size_t Size>
struct BitSetTrait {
    static constexpr unsigned total_bits = 64;  // Not directly packed, used differently
};

using BitSet64 = BitSet<64>;

}