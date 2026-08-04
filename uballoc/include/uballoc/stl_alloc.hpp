// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <utility>
#include <vector>
#include <set>
#include <map>
#include <unordered_map>
#include <functional>
#include "uballoc/global.hpp"

namespace uballoc {

template <typename T>
struct ShmAlloc {
    using value_type = T;
    using propagate_on_container_copy_assignment = std::true_type;
    using propagate_on_container_move_assignment = std::true_type;
    using propagate_on_container_swap = std::true_type;
    using is_always_equal = std::true_type;

    ShmAlloc() noexcept = default;
    template <typename U>
    ShmAlloc(const ShmAlloc<U>&) noexcept {}

    T* allocate(std::size_t n) {
        void* p = get_global_allocator<>().malloc(n * sizeof(T));
        if (!p) throw std::bad_alloc();
        return static_cast<T*>(p);
    }

    void deallocate(T* p, std::size_t) noexcept {
        get_global_allocator<>().free(p);
    }
};

template <typename T, typename U>
inline bool operator==(const ShmAlloc<T>&, const ShmAlloc<U>&) noexcept { return true; }

template <typename T, typename U>
inline bool operator!=(const ShmAlloc<T>&, const ShmAlloc<U>&) noexcept { return false; }

template <typename T>
using ShmVector = std::vector<T, ShmAlloc<T>>;

template <typename T, typename Compare = std::less<T>>
using ShmSet = std::set<T, Compare, ShmAlloc<T>>;

template <typename K, typename V, typename Compare = std::less<K>>
using ShmMap = std::map<K, V, Compare, ShmAlloc<std::pair<const K, V>>>;

template <typename K, typename V,
          typename Hash = std::hash<K>,
          typename KeyEqual = std::equal_to<K>>
using ShmUmap = std::unordered_map<K, V, Hash, KeyEqual,
                                   ShmAlloc<std::pair<const K, V>>>;

// Wrapper for type_id in the publishing overload of shm_new.
// The distinct user-defined type guarantees unambiguous overload
// resolution — it can never clash with a constructor argument.
//   auto* v = uballoc::shm_new<T>(uballoc::pub_tid{42});
// or simply:
//   auto* v = uballoc::shm_new<T>({42});
struct pub_tid {
    uint32_t value;
    explicit constexpr pub_tid(uint32_t v) noexcept : value(v) {}
};

// Allocate + construct (no publish).
template <typename T, typename... Args>
T* shm_new(Args&&... args) {
    void* p = get_global_allocator<>().malloc(sizeof(T));
    if (!p) throw std::bad_alloc();
    return new (p) T(std::forward<Args>(args)...);
}

// Allocate + construct + publish (combined).
// Replaces the two-step: shm_new<T>(args...) + publish(ptr, sizeof(T), type_id).
template <typename T, typename... Args>
T* shm_new(pub_tid type_id, Args&&... args) {
    void* p = get_global_allocator<>().malloc(sizeof(T), type_id.value);
    if (!p) throw std::bad_alloc();
    return new (p) T(std::forward<Args>(args)...);
}

template <typename T>
void shm_delete(T* p) {
    if (!p) return;
    p->~T();
    get_global_allocator<>().free(static_cast<void*>(p));
}

} // namespace uballoc
