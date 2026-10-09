// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <iostream>
#include <cassert>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <cstring>
#include <fstream>

#include "uballoc.hpp"

//  Named ArBackend (not Backend) so it can never shadow/conflict
//  with the local aliases used by the pre-existing unit tests.
using ArBackend = uballoc::DistributedShmBackend<>;

// Uses the backend's ShmProvider for proper naming conventions.
// On UBSE, shm names use "ub-" prefix; on POSIX, they use "/" prefix.
// This function handles both transparently.
static void cleanup_all_distributed_shms(int max_procs, const std::string& heap_id = "heap") {
    using ShmProvider = uballoc::DistributedShmBackend<>::ShmProvider;
    // Register no-op log callback before any UBSE calls to silence
    // verbose UBSE runtime logs during cleanup.
    ShmProvider::provider_init();
    ShmProvider::shm_unlink(ShmProvider::bootstrap_shm_name(heap_id));
    for (int pid = 0; pid < max_procs; ++pid) {
        for (int r = 0; r < 4; ++r) {
            for (int seg = 0; seg < 8; ++seg) {
                ShmProvider::shm_unlink(
                    ShmProvider::shm_name(heap_id, pid, r, seg));
            }
        }
    }
}

static inline void check_child(int status) {
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        std::cerr << "  CHILD FAILED: ";
        if (WIFSIGNALED(status)) std::cerr << "signal " << WTERMSIG(status);
        else std::cerr << "exit " << WEXITSTATUS(status);
        std::cerr << std::endl;
        assert(0);
    }
}

// ---------------------------------------------------------------------------
// AR heap-id management
// ---------------------------------------------------------------------------

// Per-binary heap id; AR_HEAP_ID overrides the default for parallel runs.
inline std::string ar_heap_id(const char* default_id) {
    const char* env = ::getenv("AR_HEAP_ID");
    return (env && *env) ? std::string(env) : std::string(default_id);
}

inline void ar_set_heap_id(const std::string& id) {
    ::setenv("UBALLOC_HEAP_ID", id.c_str(), 1);
}

inline void ar_unset_heap_id() {
    ::unsetenv("UBALLOC_HEAP_ID");
}

inline uballoc::GlobalAllocator<ArBackend>& ar_alloc() {
    return uballoc::get_global_allocator<ArBackend>();
}

inline void ar_pre_case_cleanup(const std::string& heap_id) {
    cleanup_all_distributed_shms(static_cast<int>(uballoc::MAX_PROCESSES), heap_id);
}

// Parent-side (P0) init: env must already be set.
inline void ar_parent_init() {
    auto& alloc = ar_alloc();
    alloc.soft_reset();
    alloc.init();
    assert(alloc.is_initialized());
    alloc.init_thread(0);
}

// Child-side init after fork. soft_reset() clears inherited fork state.
// Thread init is handled inside init() (all paths call ensure_thread_init
// within the library) -- do NOT call ensure_thread_init() here: it is a
// small inline function whose inlined body references the TLS init wrapper
// (_ZTH) that libuballoc.so never exports, so a direct call from the binary
// SIGSEGVs at pc=0x0 under the default extern-template mode.
inline void ar_child_init() {
    auto& alloc = ar_alloc(); 
    alloc.soft_reset();
    alloc.init();
    assert(alloc.is_initialized());
}

// ---------------------------------------------------------------------------
// AR /proc/self/maps parsing
// ---------------------------------------------------------------------------

struct ArMapRegion {
    uintptr_t start = 0;
    uintptr_t end = 0;
    char perms[5] = "----";
};

inline std::vector<ArMapRegion> ar_parse_maps() {
    std::vector<ArMapRegion> out;
    std::ifstream f("/proc/self/maps");
    std::string line;
    while (std::getline(f, line)) {
        // format: start-end perms offset dev inode [path]
        uintptr_t start = 0, end = 0;
        char perms[5] = "----";
        if (std::sscanf(line.c_str(), "%lx-%lx %4s", &start, &end, perms) == 3) {
            ArMapRegion r;
            r.start = start;
            r.end = end;
            std::memcpy(r.perms, perms, sizeof(r.perms));
            out.push_back(r);
        }
    }
    // /proc/self/maps is sorted by start; keep the invariant for callers.
    std::sort(out.begin(), out.end(),
              [](const ArMapRegion& a, const ArMapRegion& b) { return a.start < b.start; });
    return out;
}

// True if any mapping contains the byte at addr.
inline bool ar_maps_contains(const std::vector<ArMapRegion>& regions, uintptr_t addr) {
    for (const auto& r : regions) {
        if (addr >= r.start && addr < r.end) return true;
    }
    return false;
}

// Bytes of [lo, hi) covered by the union of mappings. /proc/self/maps
// regions are non-overlapping and sorted, so a sequential scan detects holes.
inline uint64_t ar_maps_covered_bytes(const std::vector<ArMapRegion>& regions,
                                      uintptr_t lo, uintptr_t hi) {
    uint64_t covered = 0;
    uintptr_t cursor = lo;
    for (const auto& r : regions) {
        if (r.end <= cursor) continue;
        if (r.start > cursor) break;  // hole before this region
        uintptr_t e = (r.end < hi) ? r.end : hi;
        if (e > cursor) {
            covered += e - cursor;
            cursor = e;
        }
        if (cursor >= hi) break;
    }
    return covered;
}

// Find the start of the first contiguous covered span of at least min_size
// bytes. 
inline uintptr_t ar_find_large_span_start(const std::vector<ArMapRegion>& regions,
                                          uint64_t min_size) {
    for (size_t i = 0; i < regions.size(); ++i) {
        uintptr_t start = regions[i].start;
        uintptr_t cursor = regions[i].end;
        for (size_t j = i + 1; j < regions.size(); ++j) {
            if (regions[j].start > cursor) break;  // hole
            if (regions[j].end > cursor) cursor = regions[j].end;
        }
        if (cursor - start >= min_size) return start;
    }
    return 0;
}

// Same as ar_find_large_span_start but also returns the span's end. Used by
// cases that verify an address falls within the shm VA reservation
// [start, end).
struct ArLargeSpan { uintptr_t start = 0; uintptr_t end = 0; };
inline ArLargeSpan ar_find_large_span(const std::vector<ArMapRegion>& regions,
                                      uint64_t min_size) {
    for (size_t i = 0; i < regions.size(); ++i) {
        uintptr_t start = regions[i].start;
        uintptr_t cursor = regions[i].end;
        for (size_t j = i + 1; j < regions.size(); ++j) {
            if (regions[j].start > cursor) break;  // hole
            if (regions[j].end > cursor) cursor = regions[j].end;
        }
        if (cursor - start >= min_size) return {start, cursor};
    }
    return {0, 0};
}

// ---------------------------------------------------------------------------
// AR /proc/self/status fields (kB)
// ---------------------------------------------------------------------------

inline long ar_status_kB(const char* key) {
    std::ifstream f("/proc/self/status");
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind(key, 0) == 0) {
            long kb = -1;
            std::sscanf(line.c_str(), "%*s %ld", &kb);
            return kb;
        }
    }
    return -1;
}

// ---------------------------------------------------------------------------
// AR pipe helpers (fork synchronization)
// ---------------------------------------------------------------------------

inline void ar_write_fd(int fd, const void* buf, size_t n) {
    const char* p = static_cast<const char*>(buf);
    size_t off = 0;
    while (off < n) {
        ssize_t w = ::write(fd, p + off, n - off);
        assert(w > 0);
        off += static_cast<size_t>(w);
    }
}

inline void ar_read_fd(int fd, void* buf, size_t n) {
    char* p = static_cast<char*>(buf);
    size_t off = 0;
    while (off < n) {
        ssize_t r = ::read(fd, p + off, n - off);
        assert(r > 0);
        off += static_cast<size_t>(r);
    }
}

inline void ar_write_byte(int fd, char c) { ar_write_fd(fd, &c, 1); }

inline char ar_read_byte(int fd) {
    char c = 0;
    ar_read_fd(fd, &c, 1);
    return c;
}