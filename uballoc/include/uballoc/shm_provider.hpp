// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <string>
#include <cstdint>
#include <cstdlib>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <cerrno>
#include <cstring>
#include <cstdio>
#include <array>
#include <signal.h>

#include "error.hpp"
#include "log.hpp"
#include "size.hpp"
#include "thread.hpp"
#include "published.hpp"

namespace uballoc {

// ---------------------------------------------------------------------------
// VA reservation + bootstrap coordination
// ---------------------------------------------------------------------------
//
// The VA layout is parameterized by a single `va_base`. All other addresses
// are derived as fixed offsets with per-bracket strides:
//
//   metadata_base   = va_base + 0
//   data_small_base = va_base + METADATA_STRIDE
//   data_large_base = va_base + METADATA_STRIDE + DATA_SMALL_STRIDE
//   data_huge_base  = va_base + METADATA_STRIDE + DATA_SMALL_STRIDE + DATA_LARGE_STRIDE
//
// Per-process VAs within each bracket use per-bracket strides:
//   metadata_va[k]   = metadata_base   + k * PROCESS_STRIDE_META
//   data_small_va[k] = data_small_base  + k * PROCESS_STRIDE_SMALL
//   data_large_va[k] = data_large_base  + k * PROCESS_STRIDE_LARGE
//   data_huge_va[k]  = data_huge_base   + k * PROCESS_STRIDE_HUGE
//
// The whole region [va_base, va_base + VA_RESERVATION_SIZE) is reserved
// upfront with PROT_NONE|MAP_NORESERVE so other libraries cannot grab it.
//
// In multi-process mode, P0 probes a list of candidate bases, picks the
// first that succeeds, and publishes the chosen base to a small bootstrap
// shm. Other processes read the published base and reserve the same range.

// One slot per process in the PID table. Slot index IS the rank.
struct BootstrapSlot {
    std::atomic<int32_t>   pid;         // 0 = empty, else OS pid of the claimer
    std::atomic<uint32_t>  generation;  // bumped on slot reuse for re-attach detection
    std::atomic<uint32_t>  ready;       // 1 when this slot's metadata shm is initialized
    std::atomic<uint32_t>  node_slot_id; // NEW (multi-proc/node): UBSE slot_id of this rank's node (0 = unset)
    std::atomic<uint32_t>  node_index;   // NEW (multi-proc/node): logical 0..MAX_PROCESSES-1, UINT32_MAX = unassigned
};

struct BootstrapBlock {
    std::atomic<uint32_t> magic;       // BOOTSTRAP_MAGIC when ready
    std::atomic<uint32_t> ready;       // 1 when va_base is valid
    std::atomic<uint64_t> va_base;     // chosen VA base
    std::atomic<uint32_t> reserved;    // 1 = reserved (PROT_NONE), 0 = per-segment fallback

    // PID table: slot index IS the rank. pid == 0 means empty slot.
    std::array<BootstrapSlot, MAX_PROCESSES> slots;

    // Per-process metadata shm size (bytes). Published by each process
    // before set_slot_ready(). Read by the uffd handler to mmap metadata
    // regions of remote processes on demand.
    std::array<std::atomic<uint64_t>, MAX_PROCESSES> metadata_sizes;

    // NEW (multi-proc/node): per-node live process count. Indexed by
    // node_index. Incremented on init, decremented on exit. The process
    // that decrements to 0 is the last on its node (race-free detection
    // for §5c cleanup). P0 zero-inits this array before publishing
    // BOOTSTRAP_MAGIC (see bootstrap_join).
    std::array<std::atomic<uint32_t>, MAX_PROCESSES> node_proc_count;

    // NEW (multi-proc/node): per-node refcount on the bootstrap shm.
    // Indexed by node_index. Incremented in do_lazy_init after claim_rank
    // + node_index assignment. Decremented in cleanup_bootstrap_mapping.
    // When it reaches 0, the last process on that node calls
    // shm_detach_by_name(bootstrap_name) (§5c, §9c).
    std::array<std::atomic<uint16_t>, MAX_PROCESSES> bootstrap_refcount;

    // NEW (multi-proc/node): metadata shm refcount. Indexed by
    // [lender_rank][borrower_node_index]. The borrower-side refcount
    // tracks how many processes on borrower_node_index have an open device
    // fd for lender_rank's metadata shm. Incremented in
    // attach_with_refcount BEFORE shm_attach (R1). Decremented in
    // detach_with_refcount. When it reaches 0, the last process on that
    // node calls shm_detach_by_name (§2a, §2c).
    std::array<std::array<std::atomic<uint16_t>, MAX_PROCESSES>, MAX_PROCESSES> metadata_refcount;

    // Global type_id → owner_process mapping. Lives in the bootstrap
    // block (not in any process's metadata) so it is always available
    // to all processes without attaching P0's metadata. This prevents
    // SIGSEGV when attach_new_processes releases P0's metadata.
    TypeIdMap type_id_map;

    // TEMPORARY: Data segment refcount, indexed as
    // data_refcount[lender_rank][bracket][seg_idx][node_index].
    // Stored in the global bootstrap so it's always accessible
    // (not dependent on lender's metadata being mapped). This prevents
    // the "null refcount ptr" issue when attach_new_processes munmaps
    // lender's metadata before detach_all_regions can access the
    // refcount.
    //
    // TODO: Move to a per-node bootstrap shm. The current 2 MB array
    // in the global bootstrap is a temporary solution. A per-node
    // bootstrap would be more efficient (no cross-node access needed)
    // and smaller (only needs space for this node's borrows, not all
    // nodes').
    static constexpr size_t DATA_REFCOUNT_LENDERS = MAX_PROCESSES;
    static constexpr size_t DATA_REFCOUNT_BRACKETS = 3;  // Small, Large, Huge
    static constexpr size_t DATA_REFCOUNT_SEGMENTS = MAX_SEGMENTS;
    static constexpr size_t DATA_REFCOUNT_NODES = MAX_PROCESSES;
    static constexpr size_t DATA_REFCOUNT_SIZE =
        DATA_REFCOUNT_LENDERS * DATA_REFCOUNT_BRACKETS *
        DATA_REFCOUNT_SEGMENTS * DATA_REFCOUNT_NODES;
    std::array<std::atomic<uint16_t>, DATA_REFCOUNT_SIZE> data_refcount;

    // No padding needed — the bootstrap shm is created with
    // round_up_shm_size(sizeof(BootstrapBlock)) which rounds up
    // to the minimum shm size (4 MB on UBSE, page size on POSIX).
};
static_assert(sizeof(BootstrapBlock) > 0, "BootstrapBlock must be non-empty");

static constexpr uint32_t BOOTSTRAP_MAGIC = 0xDEADBEEFu;
static constexpr size_t BOOTSTRAP_SIZE = sizeof(BootstrapBlock);

// Per-process VA strides (different per bracket).
// Small/Large get 1 TB each; Huge gets 1.5 TB for the larger huge slabs.
// Metadata gets 1 GB (metadata is ~7 MB per process).
static constexpr uintptr_t PROCESS_STRIDE_META  = 0x40000000ULL;            // 1 GB
static constexpr uintptr_t PROCESS_STRIDE_SMALL = 0x010000000000ULL;        // 1 TB
static constexpr uintptr_t PROCESS_STRIDE_LARGE = 0x010000000000ULL;        // 1 TB
static constexpr uintptr_t PROCESS_STRIDE_HUGE  = 0x18000000000ULL;        // 1.5 TB

// Bracket strides: MAX_PROCESSES × per-process stride.
static constexpr uintptr_t METADATA_STRIDE    = MAX_PROCESSES * PROCESS_STRIDE_META;    // 8 GB
static constexpr uintptr_t DATA_SMALL_STRIDE  = MAX_PROCESSES * PROCESS_STRIDE_SMALL;   // 8 TB
static constexpr uintptr_t DATA_LARGE_STRIDE  = MAX_PROCESSES * PROCESS_STRIDE_LARGE;   // 8 TB
static constexpr uintptr_t DATA_HUGE_STRIDE   = MAX_PROCESSES * PROCESS_STRIDE_HUGE;     // 12 TB

static constexpr size_t VA_RESERVATION_SIZE =
    METADATA_STRIDE + DATA_SMALL_STRIDE + DATA_LARGE_STRIDE + DATA_HUGE_STRIDE;  // ~28 TB

// Default base — used as fallback if all candidates are exhausted (per-segment mode).
static constexpr uintptr_t DEFAULT_VA_BASE = 0x200000000000ULL;

// Candidate list ordered by preference. Spacing >= VA_RESERVATION_SIZE so a
// single conflict cannot bleed into the next candidate. All candidates fit
// within 48-bit VA (max user VA = 256TB).
inline constexpr std::array<uintptr_t, 6> kCandidateBases = {
    0x200000000000ULL,   // 32TB  - preferred (matches historical default)
    0x600000000000ULL,   // 96TB  - 1st fallback (47-bit safe)
    0xA00000000000ULL,   // 160TB - 2nd fallback (48-bit)
    0xC00000000000ULL,   // 192TB - 3rd fallback (48-bit)
    0xE00000000000ULL,   // 224TB - 4th fallback (48-bit, near vDSO region)
    0x100000000000ULL,   // 16TB  - last resort (more conflict-prone, low VA)
};

// Tracks whether this process (or its fork parent) created a PROT_NONE VA
// reservation. Set by probe_and_reserve_va / try_reserve_va /
// uballoc_early_va_init. Inherited via fork — a child process sees the same
// value as the parent. Used by do_init to distinguish "inherited
// PROT_NONE" from "real conflict" when try_reserve_va fails.
// soft_reset() does NOT touch this — only release_va_reservation() clears it.
inline bool g_va_reservation_active = false;
inline uintptr_t g_va_reservation_base = 0;

// Probe the candidate list and reserve the first available range.
// Returns the chosen base on success, 0 on failure (all candidates exhausted).
inline uintptr_t probe_and_reserve_va() {
    for (uintptr_t base : kCandidateBases) {
        void* p = ::mmap(reinterpret_cast<void*>(base), VA_RESERVATION_SIZE,
                         PROT_NONE,
                         MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE,
                         -1, 0);
        if (p != MAP_FAILED) {
            LOG_INFO("VA reserved at 0x" << std::hex << base
                     << " size=0x" << VA_RESERVATION_SIZE << std::dec);
            g_va_reservation_active = true;
            g_va_reservation_base = base;
            return base;
        }
        LOG_WARN("VA probe at 0x" << std::hex << base
                 << " failed: " << std::dec << strerror(errno));
    }
    return 0;
}

// Try to reserve a specific base (used by non-P0 processes to claim the
// same range P0 published via the bootstrap shm).
inline bool try_reserve_va(uintptr_t base) {
    void* p = ::mmap(reinterpret_cast<void*>(base), VA_RESERVATION_SIZE,
                     PROT_NONE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE,
                     -1, 0);
    if (p == MAP_FAILED) {
        LOG_WARN("try_reserve_va at 0x" << std::hex << base
                 << " failed: " << std::dec << strerror(errno)
                 << " (may be inherited from fork)");
        return false;
    }
    g_va_reservation_active = true;
    g_va_reservation_base = base;
    return true;
}

// Early VA reservation state (POD — populated by uballoc_early_va_init below).
struct EarlyVaReservation {
    uintptr_t base = 0;
    bool reserved = false;
    bool attempted = false;
};

inline EarlyVaReservation g_early_va;

inline void release_va_reservation(uintptr_t base) {
    if (base != 0) {
        ::munmap(reinterpret_cast<void*>(base), VA_RESERVATION_SIZE);
        if (g_va_reservation_base == base) {
            g_va_reservation_active = false;
        }
        // Clear g_early_va so bootstrap_join doesn't use the stale
        // reservation — the VA was just munmapped. The next init()
        // will call probe_and_reserve_va() to re-create it.
        if (g_early_va.base == base) {
            g_early_va.reserved = false;
        }
    }
}

// Early VA reservation at constructor priority 101 — runs before
// default-priority (65535) static initializers such as jemalloc, CUDA, or
// ASAN.  At this stage std::cout / std::cerr are not yet constructed and
// g_log_level has not been dynamically initialized, so we must use
// fprintf(stderr, ...) for any diagnostics.
//
// Gated on UBALLOC_HEAP_ID: only reserves when the user has opted in to
// dynamic discovery, so ordinary programs that link libuballoc but never
// use it pay zero cost.  init() reuses the reservation if available.
//
// Declared static (internal linkage).  Each TU that includes this header
// registers its own copy; g_early_va.attempted (constant-initialized to
// false, then set true by the first to run) prevents double execution.
__attribute__((constructor(101)))
static void uballoc_early_va_init() {
    if (g_early_va.attempted) return;

    const char* heap_id = ::getenv("UBALLOC_HEAP_ID");
    if (heap_id == nullptr || *heap_id == '\0') return;

    g_early_va.attempted = true;

    for (uintptr_t candidate : kCandidateBases) {
        void* p = ::mmap(reinterpret_cast<void*>(candidate),
                         VA_RESERVATION_SIZE,
                         PROT_NONE,
                         MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE,
                         -1, 0);
        if (p != MAP_FAILED) {
            g_early_va.base = candidate;
            g_early_va.reserved = true;
            g_va_reservation_active = true;
            g_va_reservation_base = candidate;
            uballoc::log::defer_log(uballoc::LogLevel::Info,
                    "uballoc: early VA reserved at 0x%lx size=0x%lx",
                    (unsigned long)candidate, (unsigned long)VA_RESERVATION_SIZE);
            return;
        }
        uballoc::log::defer_log(uballoc::LogLevel::Warn,
                "uballoc: VA probe at 0x%lx failed: %s",
                (unsigned long)candidate, strerror(errno));
    }
    uballoc::log::defer_log(uballoc::LogLevel::Warn,
            "uballoc: Early VA reservation failed at all candidates; "
            "deferring to init");
}

struct PosixShmProvider {
    using ShmHandle = int;
    static constexpr ShmHandle INVALID_HANDLE = -1;

    static constexpr bool has_reliable_unreferenced_check = false;
    static constexpr bool is_single_node = true;
    static constexpr size_t min_shm_size = 0;
    static constexpr size_t shm_size_granularity = SIZE_PAGE;

    static void provider_init() {}

    // Bootstrap shm name (POSIX convention: leading "/").
    static std::string bootstrap_shm_name(const std::string& heap_id) {
        return "/" + heap_id + "-bootstrap";
    }

    // Kept for backwards-compatibility / debugging. init() no
    // longer accepts these as parameters — they are derived from va_base.
    static constexpr uintptr_t metadata_base      = DEFAULT_VA_BASE;
    static constexpr uintptr_t process_stride      = PROCESS_STRIDE_SMALL;
    static constexpr uintptr_t data_small_base     = DEFAULT_VA_BASE + METADATA_STRIDE;
    static constexpr uintptr_t data_large_base     = DEFAULT_VA_BASE + METADATA_STRIDE + DATA_SMALL_STRIDE;
    static constexpr uintptr_t data_huge_base     = DEFAULT_VA_BASE + METADATA_STRIDE + DATA_SMALL_STRIDE + DATA_LARGE_STRIDE;

    static std::string shm_name(const std::string& heap_id, int rank, int region_type, int segment_idx = 0) {
        static const char* suffixes[] = {"-m", "-ds", "-dl", "-dh"};
        std::string name = "/" + heap_id + "-p" + std::to_string(rank) + suffixes[region_type];
        if (region_type > 0 && segment_idx > 0) {
            name += "-s" + std::to_string(segment_idx);
        }
        return name;
    }

    static Result<ShmHandle> shm_create(const std::string& name, size_t size) {
        LOG_INFO("shm_create: name=" << name << " size=" << size);
        int fd = ::shm_open(name.c_str(), O_RDWR | O_CREAT | O_EXCL, 0666);
        if (fd < 0) {
            LOG_WARN("shm_open create failed for " << name << ": " << strerror(errno));
            return Error(ErrorKind::ShmOpen, std::error_code(errno, std::system_category()));
        }
        if (::ftruncate(fd, static_cast<off_t>(size)) != 0) {
            LOG_WARN("ftruncate failed for " << name << ": " << strerror(errno));
            ::close(fd);
            return Error(ErrorKind::Ftruncate, std::error_code(errno, std::system_category()));
        }
        return fd;
    }

    // Try to create a shm without aborting on EEXIST.
    // Returns ShmHandle on success, Error(ShmExists) if it already exists,
    // or Error(ShmOpen/Ftruncate) on other errors.
    static Result<ShmHandle> shm_try_create(const std::string& name, size_t size) {
        LOG_INFO("shm_try_create: name=" << name << " size=" << size);
        int fd = ::shm_open(name.c_str(), O_RDWR | O_CREAT | O_EXCL, 0666);
        if (fd < 0) {
            if (errno == EEXIST) {
                return Error(ErrorKind::ShmExists);
            }
            LOG_WARN("shm_open try_create failed for " << name << ": " << strerror(errno));
            return Error(ErrorKind::ShmOpen, std::error_code(errno, std::system_category()));
        }
        if (::ftruncate(fd, static_cast<off_t>(size)) != 0) {
            LOG_WARN("ftruncate failed for " << name << ": " << strerror(errno));
            ::close(fd);
            return Error(ErrorKind::Ftruncate, std::error_code(errno, std::system_category()));
        }
        return fd;
    }

    static Result<ShmHandle> shm_attach(const std::string& name) {
        LOG_INFO("shm_attach: name=" << name);
        int fd = ::shm_open(name.c_str(), O_RDWR, 0);
        if (fd < 0) {
            LOG_WARN("shm_open attach failed for " << name << ": " << strerror(errno));
            return Error(ErrorKind::ShmOpen, std::error_code(errno, std::system_category()));
        }
        return fd;
    }

    static void shm_unlink(const std::string& name) {
        ::shm_unlink(name.c_str());
    }

    static bool shm_exists(const std::string& name) {
        int fd = ::shm_open(name.c_str(), O_RDWR, 0);
        if (fd >= 0) {
            ::close(fd);
            return true;
        }
        return false;
    }

    static int shm_handle_fd(ShmHandle handle) {
        return handle;
    }

    // Close the device fd only. Does NOT release the node borrow (no-op
    // for POSIX since there is no per-node borrow concept).
    static void shm_close_fd(ShmHandle handle) {
        if (handle != INVALID_HANDLE) {
            ::close(handle);
        }
    }

    // Release the node-level borrow. No-op for POSIX (single-node, no
    // borrow concept). Called by detach_with_refcount only when
    // refcount == 0 (last process on the node). NO_ATTACH-equivalent
    // is harmless here (no-op).
    static void shm_detach_by_name(const std::string& /*name*/) {
        // POSIX: no per-node borrow to release.
    }

    // DEPRECATED — wrapper kept for backward compat. New code should use
    // shm_close_fd + shm_detach_by_name (typically via the
    // attach_with_refcount / detach_with_refcount wrappers in
    // DistributedShmBackend).
    static void shm_close(ShmHandle handle) {
        shm_close_fd(handle);
        // POSIX: shm_detach_by_name is a no-op, so just close fd.
    }

    static bool shm_is_unreferenced(const std::string& name) {
        int fd = ::shm_open(name.c_str(), O_RDWR, 0);
        if (fd >= 0) {
            ::close(fd);
            return false;
        }
        return true;
    }
};

}