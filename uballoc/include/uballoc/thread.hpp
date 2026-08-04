// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <atomic>
#include <optional>
#include <array>
#include <cassert>
#include <functional>

#include "packed.hpp"

namespace uballoc {

constexpr size_t MAX_PROCESSES = 8;
constexpr size_t MAX_NUM_CORES = 384;
constexpr size_t COUNT_THREAD = MAX_NUM_CORES * MAX_PROCESSES;
static_assert(COUNT_THREAD >= MAX_PROCESSES * MAX_NUM_CORES,
              "COUNT_THREAD must accommodate MAX_PROCESSES * MAX_NUM_CORES threads");

class ThreadId {
public:
    using StorageType = uint16_t;
    
private:
    StorageType id_;  // Store id + 1 internally for nonzero optimization
    
public:
    constexpr ThreadId() : id_(1) {}  // Default to 0 (stored as 1)
    
    constexpr explicit ThreadId(uint16_t id) : id_(id + 1) {
        assert(id < COUNT_THREAD);
    }
    
    constexpr uint16_t get() const { return id_ - 1; }
    constexpr StorageType internal() const { return id_; }
    
    constexpr operator uint16_t() const { return get(); }
    
    constexpr bool operator==(const ThreadId& other) const { return id_ == other.id_; }
    constexpr bool operator!=(const ThreadId& other) const { return id_ != other.id_; }
    
    static constexpr ThreadId invalid() { return ThreadId(); }
};

template<typename T>
class ThreadArray {
public:
    static constexpr size_t SIZE = COUNT_THREAD + 1;
    
private:
    std::array<T, SIZE> data_{};
    
public:
    ThreadArray() = default;
    
    T& operator[](ThreadId id) {
        return data_[id.internal()];
    }
    
    const T& operator[](ThreadId id) const {
        return data_[id.internal()];
    }
    
    class Iterator {
        ThreadArray* arr_;
        size_t idx_;
    public:
        Iterator(ThreadArray* arr, size_t idx) : arr_(arr), idx_(idx) {}
        T& operator*() { return arr_->data_[idx_]; }
        Iterator& operator++() { idx_++; return *this; }
        bool operator!=(const Iterator& other) const { return idx_ != other.idx_; }
    };
    
    Iterator begin() { return Iterator(this, 1); }
    Iterator end() { return Iterator(this, SIZE); }
    
    size_t size() const { return COUNT_THREAD; }
};

}