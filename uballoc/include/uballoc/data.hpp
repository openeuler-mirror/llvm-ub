// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <cstddef>
#include <optional>
#include <cassert>
#include <type_traits>

#include "packed.hpp"
#include "size.hpp"
#include "bitset.hpp"
#include "thread.hpp"

namespace uballoc {

struct Page {
    uint8_t data[4096];
};

template<typename B>
struct Offset {
    static constexpr unsigned total_bits = 64;
    
    uint64_t value_;
    
    constexpr Offset() : value_(1) {}  // NonZero, default to 1
    constexpr explicit Offset(uint64_t v) : value_(v) {
        assert(v != 0);
    }
    
    constexpr static std::optional<Offset<B>> try_new(uint64_t v) {
        if (v == 0) return std::nullopt;
        return Offset<B>(v);
    }
    
    constexpr uint64_t get() const { return value_; }
    constexpr operator uint64_t() const { return value_; }
    
    constexpr bool operator==(const Offset& other) const { return value_ == other.value_; }
    constexpr bool operator!=(const Offset& other) const { return value_ != other.value_; }
    constexpr bool operator<(const Offset& other) const { return value_ < other.value_; }
    
    constexpr Offset operator+(uint64_t delta) const {
        return Offset(value_ + delta);
    }
    
    constexpr static uint64_t pack(const Offset<B>& o) { return o.value_; }
    constexpr static Offset<B> unpack(uint64_t raw) { return Offset<B>(raw); }
};

template<typename B>
struct SlabIndex {
    static constexpr unsigned total_bits = 32;
    
    uint32_t value_;  // Stored as value + 1 (like NonZero)
    
    constexpr SlabIndex() : value_(1) {}
    constexpr explicit SlabIndex(uint32_t v) : value_(v + 1) {}
    
    constexpr static SlabIndex min() { return SlabIndex(0); }
    
    constexpr uint32_t get() const { return value_ - 1; }
    constexpr uint32_t internal() const { return value_; }
    
    constexpr static SlabIndex new_huge(size_t slot) {
        return SlabIndex(slot);
    }
    
    constexpr SlabIndex unchecked_add(uint32_t count) const {
        return SlabIndex(get() + count);
    }
    
    constexpr bool operator==(const SlabIndex& other) const { return value_ == other.value_; }
    constexpr bool operator!=(const SlabIndex& other) const { return value_ != other.value_; }
    
    constexpr static uint64_t pack(const SlabIndex& i) { return i.value_; }
    constexpr static SlabIndex unpack(uint64_t raw) {
        SlabIndex idx;
        idx.value_ = raw;
        return idx;
    }
};

template<typename B, typename T>
struct SlabSlice {
    T* base_;
    
    SlabSlice() : base_(nullptr) {}
    explicit SlabSlice(T* b) : base_(b - 1) {}
    
    T& operator[](SlabIndex<B> idx) {
        return base_[idx.internal()];
    }
    
    const T& operator[](SlabIndex<B> idx) const {
        return base_[idx.internal()];
    }
    
    T* data() { return base_ + 1; }
    const T* data() const { return base_ + 1; }
};

template<typename B>
struct Slab;  // forward declaration

template<typename B>
struct Data {
    Page* base_;
    Page* region_start_;
    size_t slab_count_;
    size_t slab_capacity_;
    size_t segment_va_size_;
    size_t slabs_per_segment_;
    size_t data_offset_;
    Slab<B>* slabs_;
    
    Data() : base_(nullptr), region_start_(nullptr), slab_count_(0), slab_capacity_(0),
             segment_va_size_(0), slabs_per_segment_(0), data_offset_(0), slabs_(nullptr) {}
    
    Data(Page* b, size_t count, size_t capacity)
        : base_(b - B::SLAB_SIZE / sizeof(Page)), region_start_(b), slab_count_(count),
          slab_capacity_(capacity),
          segment_va_size_(capacity > 0 ? capacity * B::SLAB_SIZE : 0),
          slabs_per_segment_(capacity),
          data_offset_(0), slabs_(nullptr) {}
    
    explicit Data(Page* b)
        : base_(b - B::SLAB_SIZE / sizeof(Page)), region_start_(b), slab_count_(0),
          slab_capacity_(0), segment_va_size_(0), slabs_per_segment_(0), data_offset_(0), slabs_(nullptr) {}

    void init_segmented(Page* data_start, size_t count, size_t capacity,
                        size_t seg_va_size, size_t slabs_per_seg, size_t data_off) {
        region_start_ = data_start;
        base_ = reinterpret_cast<Page*>(
            reinterpret_cast<char*>(data_start) - B::SLAB_SIZE);
        slab_count_ = count;
        slab_capacity_ = capacity;
        segment_va_size_ = seg_va_size;
        slabs_per_segment_ = slabs_per_seg;
        data_offset_ = data_off;
    }

    void set_slabs(Slab<B>* s) { slabs_ = s; }
    
    Offset<B> from_block(B class_, SlabIndex<B> slab, Bit block) const {
        uint32_t seg, seg_local;
        if (slabs_) {
            int pid = slabs_->find_process(slab.get());
            size_t local_idx = slab.get() - slabs_->cumulative[pid];
            auto loc = slabs_->find_segment(pid, local_idx);
            // per-process seg → global seg (pid * MAX_SEGMENTS + seg_in_pid)
            seg = static_cast<uint32_t>(pid) * static_cast<uint32_t>(MAX_SEGMENTS)
                + static_cast<uint32_t>(loc.seg);
            seg_local = static_cast<uint32_t>(loc.seg_local);
        } else {
            seg = (slabs_per_segment_ > 0)
                ? slab.get() / slabs_per_segment_ : 0;
            seg_local = (slabs_per_segment_ > 0)
                ? slab.get() % slabs_per_segment_ : static_cast<uint32_t>(slab.get());
        }
        uint64_t offset = static_cast<uint64_t>(seg) * segment_va_size_
                        + data_offset_
                        + static_cast<uint64_t>(seg_local) * B::SLAB_SIZE
                        + static_cast<uint64_t>(block) * class_.size();
        return Offset<B>(offset + B::SLAB_SIZE);
    }
    
    SlabIndex<B> into_index(Offset<B> offset) const {
        uint64_t raw = offset.get() - B::SLAB_SIZE;
        uint32_t seg = segment_va_size_ > 0
            ? static_cast<uint32_t>(raw / segment_va_size_) : 0;
        uint64_t within = segment_va_size_ > 0
            ? raw % segment_va_size_ : raw;
        uint64_t adjusted = (within >= data_offset_) ? (within - data_offset_) : 0;
        uint32_t seg_local = static_cast<uint32_t>(adjusted / B::SLAB_SIZE);
        if (slabs_ && slabs_per_segment_ > 0) {
            size_t pid = seg / MAX_SEGMENTS;
            size_t seg_in_pid = seg % MAX_SEGMENTS;
            size_t global_start = slabs_->segment_global_start(
                static_cast<int>(pid), seg_in_pid);
            return SlabIndex<B>(global_start + seg_local);
        }
        return SlabIndex<B>(seg * static_cast<uint32_t>(slabs_per_segment_) + seg_local);
    }
    
    Bit into_block(Offset<B> offset, B class_) const {
        uint64_t raw = offset.get() - B::SLAB_SIZE;
        uint64_t within = segment_va_size_ > 0
            ? raw % segment_va_size_ : raw;
        uint64_t adjusted = (within >= data_offset_) ? (within - data_offset_) : 0;
        uint64_t block = adjusted % B::SLAB_SIZE / class_.size();
        return Bit::from_loose(static_cast<uint16_t>(block));
    }
    
    template<typename T>
    T* offset_to_pointer(Offset<B> offset) {
        return reinterpret_cast<T*>(reinterpret_cast<char*>(base_) + offset.get());
    }
    
    template<typename T>
    const T* offset_to_pointer(Offset<B> offset) const {
        return reinterpret_cast<const T*>(reinterpret_cast<const char*>(base_) + offset.get());
    }
    
    Offset<B> offset_to_offset(size_t offset) {
        return Offset<B>(offset + B::SLAB_SIZE);
    }
    
    template<typename T>
    std::optional<Offset<B>> pointer_to_offset(T* ptr) {
        uintptr_t diff = reinterpret_cast<uintptr_t>(ptr) - reinterpret_cast<uintptr_t>(base_);
        if (diff == 0) return std::nullopt;
        return Offset<B>(diff);
    }
    
    std::optional<Offset<B>> checked_pointer_to_offset(void* ptr) {
        if (!region_start_) return std::nullopt;
        uintptr_t p = reinterpret_cast<uintptr_t>(ptr);
        uintptr_t start = reinterpret_cast<uintptr_t>(region_start_);
        uintptr_t end = start + static_cast<uintptr_t>(MAX_PROCESSES) * MAX_SEGMENTS * segment_va_size_;
        if (p < start || p >= end) return std::nullopt;
        uintptr_t diff = p - reinterpret_cast<uintptr_t>(base_);
        return Offset<B>(diff);
    }
};

}