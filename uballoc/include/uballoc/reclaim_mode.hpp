// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <cstdlib>
#include <atomic>

#include "log.hpp"

namespace uballoc {

// Reclaim mode: controls when freed memory is returned to the OS.
//   SYNC  — inline in malloc/free (check_one_segment_for_reclaim, round-robin)
//   ASYNC — background thread (ReclaimThread, 5s jemalloc-style wake)
//
// Both modes also support uballoc::purge() for manual immediate reclaim.
enum class ReclaimMode : uint8_t {
    ASYNC = 0,  // default: background thread
    SYNC  = 1,  // inline in malloc/free
};

// Global reclaim mode, set from UBALLOC_RECLAIM_MODE env var in init().
// Read by heap.hpp (allocate/free) to decide whether to call reclaim_fn.
// Default: ASYNC (background thread).
inline std::atomic<uint8_t>& reclaim_mode_atomic() {
    static std::atomic<uint8_t> val{static_cast<uint8_t>(ReclaimMode::ASYNC)};
    return val;
}

inline ReclaimMode reclaim_mode() {
    return static_cast<ReclaimMode>(reclaim_mode_atomic().load(std::memory_order_acquire));
}

inline void set_reclaim_mode(ReclaimMode m) {
    reclaim_mode_atomic().store(static_cast<uint8_t>(m), std::memory_order_release);
}

// Parse UBALLOC_RECLAIM_MODE env var. Called from do_lazy_init.
// Default: ASYNC (background thread). Set "sync" for inline reclaim.
inline ReclaimMode parse_reclaim_mode_from_env() {
    const char* env = std::getenv("UBALLOC_RECLAIM_MODE");
    if (env) {
        std::string s(env);
        if (s == "sync" || s == "SYNC") {
            LOG_INFO("reclaim_mode: SYNC (inline in malloc/free)");
            return ReclaimMode::SYNC;
        }
        if (s == "async" || s == "ASYNC") {
            LOG_INFO("reclaim_mode: ASYNC (background thread)");
            return ReclaimMode::ASYNC;
        }
    }
    LOG_INFO("reclaim_mode: ASYNC (default)");
    return ReclaimMode::ASYNC;
}

} // namespace uballoc
