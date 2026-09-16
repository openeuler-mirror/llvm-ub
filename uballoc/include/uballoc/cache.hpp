// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <cstddef>
#include <atomic>
#include <type_traits>

namespace uballoc {

constexpr size_t SIZE_CACHE_LINE = 64;
constexpr size_t SIZE_PAGE = 4096;

constexpr size_t SEGMENT_VA_SIZE = 128ULL * 1024 * 1024;
constexpr size_t MAX_SEGMENTS = 8192;
constexpr size_t HUGE_SLOTS_PER_SEGMENT = SEGMENT_VA_SIZE / (4 * 1024 * 1024);
constexpr size_t MAX_HUGE_SLOTS = MAX_SEGMENTS * HUGE_SLOTS_PER_SEGMENT;

using GrowFn = bool(*)();

inline std::atomic<GrowFn>& grow_fn_small() {
    static std::atomic<GrowFn> fn{nullptr};
    return fn;
}
inline std::atomic<GrowFn>& grow_fn_large() {
    static std::atomic<GrowFn> fn{nullptr};
    return fn;
}
inline std::atomic<GrowFn>& grow_fn_huge() {
    static std::atomic<GrowFn> fn{nullptr};
    return fn;
}

using CheckRemoteFn = bool(*)();

inline std::atomic<CheckRemoteFn>& check_remote_fn() {
    static std::atomic<CheckRemoteFn> fn{nullptr};
    return fn;
}

using ReclaimFn = void(*)();

inline std::atomic<ReclaimFn>& reclaim_fn_small() {
    static std::atomic<ReclaimFn> fn{nullptr};
    return fn;
}

inline std::atomic<ReclaimFn>& reclaim_fn_large() {
    static std::atomic<ReclaimFn> fn{nullptr};
    return fn;
}

// ReclaimThread wake trigger. Called from heap.hpp free_offset when
// a segment becomes fully idle (live_slab_count drops to 0). This
// is the jemalloc "pac_maybe_wake_bg" pattern: the hot path doesn't
// scan, it just pokes the background thread to re-evaluate its deadline
// and wake from indefinite sleep.
using ReclaimTriggerFn = void(*)();

inline std::atomic<ReclaimTriggerFn>& reclaim_trigger_fn() {
    static std::atomic<ReclaimTriggerFn> fn{nullptr};
    return fn;
}

enum class AffinityMode : size_t {
    Strict     = 0,
    ReuseFirst = 1,
};

inline std::atomic<size_t>& affinity_mode() {
    static std::atomic<size_t> val{static_cast<size_t>(AffinityMode::Strict)};
    return val;
}

struct ReturnStats {
    size_t segments_live = 0;
    size_t segments_detached = 0;
    size_t segments_returned = 0;
    size_t total_detached_count = 0;
    size_t total_returned_count = 0;
    uint64_t requested_from_os_bytes = 0;
    uint64_t returned_to_os_bytes = 0;
    uint64_t allocated_to_app_bytes = 0;
    uint64_t freed_from_app_bytes = 0;
};

// Global atomic counters for hot-path byte tracking.
// heap.hpp (templated on B=Small/Large) can't access BackendT members,
// so these are global — same pattern as grow_fn_small()/reclaim_fn_small().
inline std::atomic<bool>& reclaim_stats_enabled() {
    static std::atomic<bool> enabled{true};
    return enabled;
}

inline std::atomic<uint64_t>& stats_allocated_to_app() {
    static std::atomic<uint64_t> v{0};
    return v;
}

inline std::atomic<uint64_t>& stats_freed_from_app() {
    static std::atomic<uint64_t> v{0};
    return v;
}

enum class Invalidate { No, Yes };

inline void flush(void* address, size_t size, Invalidate invalidate) {
    (void)address; (void)size; (void)invalidate;
#ifdef UBALLOC_RECOVER_FLUSH
    for (size_t offset = 0; offset < size; offset += SIZE_CACHE_LINE) {
        void* line = static_cast<char*>(address) + offset;
        
#ifdef __aarch64__
        if (invalidate == Invalidate::Yes) {
            asm volatile("dc civac, %0" : : "r" (line) : "memory");
        } else {
            asm volatile("dc cvac, %0" : : "r" (line) : "memory");
        }
#endif
    }
    
#ifdef __aarch64__
    asm volatile("dsb sy" : : : "memory");
#endif
#endif
}

inline void fence() {
#ifdef UBALLOC_RECOVER_FLUSH
#ifdef __aarch64__
    asm volatile("dmb sy" : : : "memory");
#endif
#endif
}

template<typename T>
inline void flush(T* address, Invalidate invalidate) {
    flush(address, sizeof(T), invalidate);
}

inline void cache_line_clean(void* address) {
#ifdef __aarch64__
    asm volatile("dc cvac, %0" : : "r" (address) : "memory");
#endif
}

inline void cache_line_clean_and_invalidate(void* address) {
#ifdef __aarch64__
    asm volatile("dc civac, %0" : : "r" (address) : "memory");
#endif
}

inline void cache_line_clean_to_poc(void* address) {
#ifdef __aarch64__
    asm volatile("dc cvac, %0" : : "r" (address) : "memory");
#endif
}

inline void data_sync_barrier() {
#ifdef __aarch64__
    asm volatile("dsb sy" : : : "memory");
#endif
}

inline void data_memory_barrier() {
#ifdef __aarch64__
    asm volatile("dmb sy" : : : "memory");
#endif
}

}