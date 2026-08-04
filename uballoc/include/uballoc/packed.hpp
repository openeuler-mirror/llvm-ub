// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <cstddef>
#include <atomic>
#include <optional>
#include <type_traits>
#include <cassert>

namespace uballoc {

template<unsigned Bits>
struct PackedUInt {
    static_assert(Bits > 0 && Bits <= 64);
    
    using StorageType = std::conditional_t<Bits <= 8, uint8_t,
                        std::conditional_t<Bits <= 16, uint16_t,
                        std::conditional_t<Bits <= 32, uint32_t, uint64_t>>>;
    
    static constexpr uint64_t MASK = (Bits == 64) ? ~0ULL : ((1ULL << Bits) - 1);
    
    StorageType value_;
    
    constexpr PackedUInt() : value_(0) {}
    constexpr explicit PackedUInt(uint64_t v) : value_(static_cast<StorageType>(v & MASK)) {}
    
    constexpr uint64_t value() const { return value_ & MASK; }
    constexpr void set(uint64_t v) { value_ = static_cast<StorageType>(v & MASK); }
    
    constexpr operator uint64_t() const { return value(); }
    
    constexpr PackedUInt& operator=(uint64_t v) { set(v); return *this; }
    
    constexpr PackedUInt next() const { 
        return PackedUInt((value() + 1) & MASK);
    }
};

using u2  = PackedUInt<2>;
using u4  = PackedUInt<4>;
using u5  = PackedUInt<5>;
using u6  = PackedUInt<6>;
using u7  = PackedUInt<7>;
using u8  = PackedUInt<8>;
using u12 = PackedUInt<12>;
using u16 = PackedUInt<16>;
using u32 = PackedUInt<32>;
using u48 = PackedUInt<48>;
using u60 = PackedUInt<60>;
using u64 = PackedUInt<64>;

template<unsigned Bits>
struct NonZeroPackedUInt : PackedUInt<Bits> {
    using Base = PackedUInt<Bits>;
    
    constexpr NonZeroPackedUInt() : Base(1) {}
    constexpr explicit NonZeroPackedUInt(uint64_t v) : Base(v) {
        assert(v != 0);
    }
    
    constexpr static NonZeroPackedUInt min() { return NonZeroPackedUInt(1); }
    
    constexpr static std::optional<NonZeroPackedUInt> try_new(uint64_t v) {
        if (v == 0) return std::nullopt;
        return NonZeroPackedUInt(v);
    }
    
    constexpr NonZeroPackedUInt unchecked_add(uint64_t delta) const {
        return NonZeroPackedUInt(this->value() + delta);
    }
};

template<unsigned Bits>
using NonZero = NonZeroPackedUInt<Bits>;

// Helper to detect pack/unpack methods - C++17 SFINAE
template<typename T, typename = void>
struct has_pack_methods : std::false_type {};

template<typename T>
struct has_pack_methods<T, std::void_t<
    decltype(T::pack(std::declval<const T&>())),
    decltype(T::unpack(0ull))
>> : std::true_type {};

// Primary template with default second parameter
template<typename T, typename = void>
struct PackTrait {};

// Specialization for types with pack/unpack methods
template<typename T>
struct PackTrait<T, std::enable_if_t<has_pack_methods<T>::value>> {
    static constexpr unsigned total_bits = 64;
    static uint64_t pack(const T& value) { return T::pack(value); }
    static T unpack(uint64_t raw) { return T::unpack(raw); }
};

template<typename T>
inline uint64_t pack(const T& value) {
    return PackTrait<T>::pack(value);
}

template<typename T>
inline T unpack(uint64_t raw) {
    return PackTrait<T>::unpack(raw);
}

}