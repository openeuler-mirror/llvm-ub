// SPDX-License-Identifier: Apache-2.0

// uffd_handler.hpp — userfaultfd-based lazy-attach for remote segments.
//
// This module is the PRIMARY fault handler when userfaultfd is available.
// It replaces the SIGSEGV handler (fault_handler.hpp) as the primary
// mechanism, falling back to SIGSEGV when uffd is unavailable
// (ENOSYS or EPERM).
//
// The key advantage over the SIGSEGV handler: uffd runs the handler in a
// SEPARATE THREAD in NORMAL THREAD CONTEXT (not signal context). The
// kernel parks the faulting thread and sends a message to the uffd fd.
// The handler thread reads this message, resolves the fault (which may
// involve calling ubs_mem_shm_attach() — sockets, malloc, all allowed
// in normal context), and wakes the faulting thread via UFFDIO_WAKE.
//
// The SIGSEGV handler could only mmap with pre-registered fds — it could
// NOT discover segments because ubs_mem_shm_attach() is not async-signal-
// safe. uffd solves this: the handler can discover + attach on demand.
//
// Multi-level discovery avoids recursive uffd faults:
//   Level 0: Bootstrap block (always attached during init — safe to read)
//            → va_base, slots[k].ready, metadata_sizes[k]
//   Level 1: Metadata regions (attached by handler on demand)
//            → SharedLayout header with seg_dir offsets
//            → SegmentDirectory with count, descs[s].name/ready/total_size
//   Level 2: Data segments (attached by handler if published ready=1)
//
// Validated on UBSE 2-node hardware (Stage B probes B1-B4, all passed).

#pragma once

#include <atomic>
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <thread>
#include <mutex>
#include <string>
#include <vector>

#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>

#include "shm_provider.hpp"     // VA layout constants, BootstrapBlock
#include "fault_handler.hpp"    // g_fh_entries, fh_register_segment, etc.
#include "segment.hpp"          // SegmentDirectory, SegmentDescriptor
#include "shared_layout.hpp"    // SharedLayout
#include "cache.hpp"            // SEGMENT_VA_SIZE, MAX_SEGMENTS
#include "log.hpp"

#ifdef UBALLOC_USE_UBSE
#include "ubshm_provider.hpp"   // UBShmProvider (for CtorShmProvider typedef)
#endif

// userfaultfd headers — system header if available, otherwise fallback.
#if __has_include(<linux/userfaultfd.h>)
#  include <linux/userfaultfd.h>
#  define HAVE_USERFAULTFD_H 1
#else
#  define HAVE_USERFAULTFD_H 0

struct uffdio_api {
    uint64_t api;
    uint64_t features;
    uint64_t ioctls;
};

struct uffdio_range {
    uint64_t start;
    uint64_t len;
};

struct uffdio_register {
    struct uffdio_range range;
    uint64_t mode;
    uint64_t ioctls;
};

struct uffd_msg {
    uint8_t  event;
    uint8_t  reserved1;
    uint16_t reserved2;
    uint32_t reserved3;
    union {
        struct {
            uint64_t flags;
            uint64_t address;
            union {
                uint32_t ptid;
            } feat;
        } pagefault;
    } arg;
};

#  define UFFD_API 0xAA
#  define _UFFDIO_API      0x3F
#  define _UFFDIO_REGISTER 0x00
#  define _UFFDIO_WAKE     0x02
#  define UFFDIO_API \
     ((3U<<30) | (sizeof(struct uffdio_api)<<16) | ('u'<<8) | _UFFDIO_API)
#  define UFFDIO_REGISTER \
     ((3U<<30) | (sizeof(struct uffdio_register)<<16) | ('u'<<8) | _UFFDIO_REGISTER)
#  define UFFDIO_WAKE \
     (2U<<30 | (sizeof(struct uffdio_range)<<16) | ('u'<<8) | _UFFDIO_WAKE)
#endif // __has_include

#ifndef UFFDIO_REGISTER_MODE_MISSING
#  define UFFDIO_REGISTER_MODE_MISSING 1ULL
#endif

#ifndef UFFD_FEATURE_MISSING_SHMEM
#  define UFFD_FEATURE_MISSING_SHMEM 0x0400
#endif

#ifndef UFFD_EVENT_PAGEFAULT
#  define UFFD_EVENT_PAGEFAULT 0x12
#endif

// UFFDIO_POISON — added in Linux 6.6 (2023). May not be in older headers.
// Used to deliver SIGBUS to the faulting thread when the fault is
// unrecoverable (segment not published, process not joined, invalid VA).
#ifndef UFFDIO_POISON
struct uffdio_poison {
    struct uffdio_range range;
    uint64_t mode;
    int64_t updated;  // kernel-written output (per Linux 6.6 UAPI)
};
#  define _UFFDIO_POISON 0x05
#  define UFFDIO_POISON \
     _IOWR('u', _UFFDIO_POISON, struct uffdio_poison)
#endif

#ifndef SYS_userfaultfd
#  define SYS_userfaultfd __NR_userfaultfd
#endif

namespace uballoc {

// -------------------------------------------------------------------------
// Globals (set during enable_userfaultfd, read-only from handler thread).
// -------------------------------------------------------------------------

inline int g_uffd_fd = -1;
inline std::atomic<bool> g_uffd_running{false};
inline std::thread g_uffd_thread;
inline std::atomic<bool> g_uffd_active{false};

inline std::atomic<uintptr_t> g_uffd_va_base{0};
inline std::atomic<void*> g_uffd_bootstrap_ptr{nullptr};
inline std::atomic<const char*> g_uffd_heap_id{nullptr};

// Tracks the constructor(102)'s separate read-only bootstrap mmap.
// This is DIFFERENT from g_uffd_bootstrap_ptr, which may be overwritten
// by try_enable_uffd_locked() to point at backend_.bootstrap_block_.
// g_uffd_ctor_bs_mmap always tracks ONLY the constructor's mmap, so
// uffd_uninstall() can safely munmap it without affecting bootstrap_block_.
// Must be munmapped before ubs_mem_shm_detach, or the OBMM driver rejects
// the detach with 10700 (OBMM_OP_FAILED) due to an active mmap.
inline std::atomic<void*> g_uffd_ctor_bs_mmap{nullptr};

// Guard to prevent the constructor(102) from running multiple times
// across translation units (same pattern as g_early_va.attempted).
inline bool g_uffd_ctor_attempted = false;

// -------------------------------------------------------------------------
// Tracking uffd-attached segments for cleanup.
//
// The uffd handler attaches remote segments on demand (shm_attach + mmap)
// but does NOT register them in backend_.segments_[k][b] (to avoid data
// races on the vector). Instead, each attached segment is pushed here.
//
// detach_all_regions() (templated, knows ShmProviderT) iterates this list
// after uffd_uninstall() has stopped the handler thread, calling munmap +
// shm_close for each entry. Without this, the OBMM borrow on UBSE is never
// released (shm_close → ubs_mem_shm_detach), causing the peer process to
// hang in wait_for_unreferenced().
//
// handle_buf is a fixed-size buffer large enough for any ShmHandle type:
//   POSIX: int (4 bytes)
//   UBSE:  struct { int fd; char name[64]; } (68 bytes)
// -------------------------------------------------------------------------
struct UffdAttachedSeg {
    void* addr = nullptr;
    size_t size = 0;
    char handle_buf[72] = {};
    // NEW (multi-proc/node): refcount metadata for detach_with_refcount.
    // Populated by the uffd handler when attaching a remote segment.
    // Used by detach_all_regions to call detach_with_refcount (which
    // decrements the per-node refcount and conditionally calls
    // shm_detach_by_name). For POSIX (is_single_node=true), these are
    // unused (detach_with_refcount skips refcount logic).
    int lender_rank = -1;
    int region_type = -1;  // RegionType enum value (0=METADATA, 1=DATA_SMALL, etc.)
    int seg_idx = 0;
};

inline std::mutex g_uffd_attached_mtx;
inline std::vector<UffdAttachedSeg> g_uffd_attached;

// -------------------------------------------------------------------------
// Helper: wake the faulting thread.
// -------------------------------------------------------------------------

inline void uffd_wake(uintptr_t fault_addr, size_t len = 4096) {
    struct uffdio_range range;
    range.start = fault_addr & ~static_cast<uintptr_t>(4095);
    range.len = len;
    ::ioctl(g_uffd_fd, UFFDIO_WAKE, &range);
}

// -------------------------------------------------------------------------
// Helper: poison the faulting thread (SIGBUS).
// Falls back to UFFDIO_ZEROPAGE if UFFDIO_POISON is not supported.
//
// `reason` is logged via LOG_ERROR before poisoning. The uffd handler runs
// in normal thread context (not signal context), so LOG_* is safe.
// Permanent diagnostic — fires only on uffd handler failure paths.
// -------------------------------------------------------------------------

inline void uffd_poison(uintptr_t fault_addr, const char* reason, size_t len = 4096) {
    LOG_ERROR("uffd_poison: fault=0x" << std::hex << fault_addr
              << " reason=" << reason << std::dec);

    struct uffdio_range range;
    range.start = fault_addr & ~static_cast<uintptr_t>(4095);
    range.len = len;

    // Zero the whole struct first, then set only the fields we know exist
    // across all kernel UAPI versions ('range' and 'mode'). The third
    // field name varies across kernel versions (e.g., 'updated' in Linux
    // 6.6); it is a kernel-written output, so we never need to set it.
    struct uffdio_poison poison;
    std::memset(&poison, 0, sizeof(poison));
    poison.range = range;
    poison.mode = 0;

    if (::ioctl(g_uffd_fd, UFFDIO_POISON, &poison) == 0) {
        return;  // SIGBUS delivered to faulting thread
    }

    // Fallback: UFFDIO_ZEROPAGE (maps a zero page, thread reads zeros).
    // Not ideal, but better than hanging. The thread will likely segfault
    // on the resulting null/zero data.
    struct {
        struct uffdio_range range;
        uint64_t mode;
        int64_t zeropage;
    } zp;
    zp.range = range;
    zp.mode = 0;
    zp.zeropage = 0;
    ::ioctl(g_uffd_fd, UFFDIO_ZEROPAGE, &zp);
}

// -------------------------------------------------------------------------
// Helper: find a registry entry by VA range.
// Returns index in g_fh_entries, or -1 if not found.
// -------------------------------------------------------------------------

inline int uffd_find_entry(uintptr_t addr) {
    for (size_t i = 0; i < FH_MAX_ENTRIES; ++i) {
        uintptr_t va = g_fh_entries[i].va.load(std::memory_order_acquire);
        if (va == 0) continue;
        uintptr_t end = g_fh_entries[i].va_end.load(std::memory_order_relaxed);
        if (addr >= va && addr < end) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

// -------------------------------------------------------------------------
// Fault type determination from VA layout.
// All strides are compile-time constants from shm_provider.hpp / cache.hpp.
// -------------------------------------------------------------------------

enum UffdFaultType {
    UFFD_FAULT_METADATA = 0,
    UFFD_FAULT_DATA_SMALL = 1,
    UFFD_FAULT_DATA_LARGE = 2,
    UFFD_FAULT_DATA_HUGE = 3,
    UFFD_FAULT_INVALID = 4,
};

struct UffdFaultInfo {
    UffdFaultType type;
    int k;      // process index
    int s;      // segment index (always 0 for METADATA)
    int bracket; // 0=small, 1=large, 2=huge (for DATA types)
};

inline UffdFaultInfo uffd_classify(uintptr_t fault_addr) {
    UffdFaultInfo info;
    info.type = UFFD_FAULT_INVALID;
    info.k = -1;
    info.s = -1;
    info.bracket = -1;

    uintptr_t va_base = g_uffd_va_base.load(std::memory_order_acquire);
    uintptr_t offset = fault_addr - va_base;

    if (offset < METADATA_STRIDE) {
        info.type = UFFD_FAULT_METADATA;
        info.k = static_cast<int>(offset / PROCESS_STRIDE_META);
        info.s = 0;
    } else if (offset < METADATA_STRIDE + DATA_SMALL_STRIDE) {
        info.type = UFFD_FAULT_DATA_SMALL;
        info.bracket = 0;
        uintptr_t within = offset - METADATA_STRIDE;
        info.k = static_cast<int>(within / PROCESS_STRIDE_SMALL);
        info.s = static_cast<int>((within % PROCESS_STRIDE_SMALL) / SEGMENT_VA_SIZE);
    } else if (offset < METADATA_STRIDE + DATA_SMALL_STRIDE + DATA_LARGE_STRIDE) {
        info.type = UFFD_FAULT_DATA_LARGE;
        info.bracket = 1;
        uintptr_t within = offset - METADATA_STRIDE - DATA_SMALL_STRIDE;
        info.k = static_cast<int>(within / PROCESS_STRIDE_LARGE);
        info.s = static_cast<int>((within % PROCESS_STRIDE_LARGE) / SEGMENT_VA_SIZE);
    } else if (offset < VA_RESERVATION_SIZE) {
        info.type = UFFD_FAULT_DATA_HUGE;
        info.bracket = 2;
        uintptr_t within = offset - METADATA_STRIDE - DATA_SMALL_STRIDE - DATA_LARGE_STRIDE;
        info.k = static_cast<int>(within / PROCESS_STRIDE_HUGE);
        info.s = static_cast<int>((within % PROCESS_STRIDE_HUGE) / SEGMENT_VA_SIZE);
    }

    return info;
}

// -------------------------------------------------------------------------
// Helper: compute metadata VA for process k.
// -------------------------------------------------------------------------

inline uintptr_t uffd_metadata_va(int k) {
    return g_uffd_va_base.load(std::memory_order_acquire)
           + static_cast<uintptr_t>(k) * PROCESS_STRIDE_META;
}

// -------------------------------------------------------------------------
// Helper: compute data segment VA for process k, bracket b, segment s.
// -------------------------------------------------------------------------

inline uintptr_t uffd_data_va(int k, int bracket, int s) {
    uintptr_t base = g_uffd_va_base.load(std::memory_order_acquire);
    uintptr_t process_stride;
    uintptr_t bracket_offset;

    switch (bracket) {
        case 0:
            process_stride = PROCESS_STRIDE_SMALL;
            bracket_offset = METADATA_STRIDE;
            break;
        case 1:
            process_stride = PROCESS_STRIDE_LARGE;
            bracket_offset = METADATA_STRIDE + DATA_SMALL_STRIDE;
            break;
        case 2:
            process_stride = PROCESS_STRIDE_HUGE;
            bracket_offset = METADATA_STRIDE + DATA_SMALL_STRIDE + DATA_LARGE_STRIDE;
            break;
        default:
            return 0;
    }

    return base + bracket_offset
           + static_cast<uintptr_t>(k) * process_stride
           + static_cast<uintptr_t>(s) * SEGMENT_VA_SIZE;
}

// -------------------------------------------------------------------------
// The handler thread function (templated on ShmProviderT).
//
// Runs in normal thread context — can call malloc, sockets, shm_attach.
// Handles one uffd event at a time (sequential).
// -------------------------------------------------------------------------

template<typename ShmProviderT>
void uffd_handler_thread() {
    LOG_INFO("uffd handler thread started");

    while (g_uffd_running.load(std::memory_order_acquire)) {
        // Poll with 100ms timeout for clean shutdown.
        struct pollfd pfd;
        pfd.fd = g_uffd_fd;
        pfd.events = POLLIN;
        int pret = ::poll(&pfd, 1, 100);
        if (pret <= 0) continue;
        if (!(pfd.revents & POLLIN)) continue;

        // Read the fault message.
        struct uffd_msg msg;
        ssize_t n = ::read(g_uffd_fd, &msg, sizeof(msg));
        if (n != static_cast<ssize_t>(sizeof(msg))) continue;
        if (msg.event != UFFD_EVENT_PAGEFAULT) continue;

        uintptr_t fault_addr = static_cast<uintptr_t>(msg.arg.pagefault.address);

        // Step 1: classify the fault.
        UffdFaultInfo fi = uffd_classify(fault_addr);
        if (fi.type == UFFD_FAULT_INVALID) {
            uffd_poison(fault_addr, "invalid_fault_type");
            continue;
        }

        // Step 2: fast path — check the registry.
        int entry_idx = uffd_find_entry(fault_addr);
        if (entry_idx >= 0) {
            uint32_t attached = g_fh_entries[entry_idx].attached.load(
                std::memory_order_acquire);
            int fd = g_fh_entries[entry_idx].fd.load(std::memory_order_relaxed);
            uintptr_t seg_va = g_fh_entries[entry_idx].va.load(std::memory_order_relaxed);
            uintptr_t seg_end = g_fh_entries[entry_idx].va_end.load(std::memory_order_relaxed);
            size_t seg_size = seg_end - seg_va;

            if (attached == 0 && fd >= 0) {
                // fd is open but segment not yet mmaped — mmap now.
                void* ptr = ::mmap(reinterpret_cast<void*>(seg_va), seg_size,
                                  PROT_READ | PROT_WRITE,
                                  MAP_SHARED | MAP_FIXED, fd, 0);
                if (ptr != MAP_FAILED) {
                    g_fh_entries[entry_idx].attached.store(
                        1, std::memory_order_release);
                    uffd_wake(fault_addr);
                    continue;
                }
                // mmap failed — poison.
                uffd_poison(fault_addr, "fast_path_mmap_failed");
                continue;
            } else if (attached == 1) {
                // Mapping was lost (rare). Try to re-mmap.
                if (fd >= 0) {
                    void* ptr = ::mmap(reinterpret_cast<void*>(seg_va), seg_size,
                                      PROT_READ | PROT_WRITE,
                                      MAP_SHARED | MAP_FIXED, fd, 0);
                    if (ptr != MAP_FAILED) {
                        uffd_wake(fault_addr);
                        continue;
                    }
                }
                uffd_poison(fault_addr, "fast_path_remap_failed");
                continue;
            }
        }

        // Step 3: slow path — discover and attach.
        if (fi.type == UFFD_FAULT_METADATA) {
            // Metadata fault: attach process k's metadata on demand.
            BootstrapBlock* bootstrap = static_cast<BootstrapBlock*>(
                g_uffd_bootstrap_ptr.load(std::memory_order_acquire));

            if (!bootstrap || fi.k < 0 || fi.k >= static_cast<int>(MAX_PROCESSES)) {
                uffd_poison(fault_addr, "meta_bootstrap_null");
                continue;
            }

            // Check if process k has joined.
            uint32_t slot_ready = bootstrap->slots[fi.k].ready.load(
                std::memory_order_acquire);
            if (slot_ready != 1) {
                uffd_poison(fault_addr, "meta_slot_not_ready");
                continue;
            }

            // Read metadata size (published before slots[k].ready).
            uint64_t meta_size = bootstrap->metadata_sizes[fi.k].load(
                std::memory_order_acquire);
            if (meta_size == 0) {
                uffd_poison(fault_addr, "meta_size_zero");
                continue;
            }

            // Construct metadata shm name and attach.
            const char* heap_id = g_uffd_heap_id.load(std::memory_order_acquire);
            if (!heap_id) {
                uffd_poison(fault_addr, "meta_heap_id_null");
                continue;
            }

            std::string name = ShmProviderT::shm_name(
                std::string(heap_id), fi.k, 0 /*METADATA*/, 0);

            auto attach_res = ShmProviderT::shm_attach(name);
            if (is_err(attach_res)) {
                LOG_WARN("uffd handler: shm_attach failed for metadata "
                          << name);
                uffd_poison(fault_addr, "meta_attach_failed");
                continue;
            }

            auto handle = unwrap(attach_res);
            int fd = ShmProviderT::shm_handle_fd(handle);
            if (fd < 0) {
                uffd_poison(fault_addr, "meta_fd_invalid");
                continue;
            }

            // mmap the metadata region.
            uintptr_t meta_va = uffd_metadata_va(fi.k);
            void* ptr = ::mmap(reinterpret_cast<void*>(meta_va),
                              static_cast<size_t>(meta_size),
                              PROT_READ | PROT_WRITE,
                              MAP_SHARED | MAP_FIXED, fd, 0);
            if (ptr == MAP_FAILED) {
                LOG_WARN("uffd handler: mmap failed for metadata "
                          << name << ": " << strerror(errno));
                // §9a: Use shm_close_fd (NOT shm_close) to avoid premature
                // detach. The refcount was never incremented (uffd handler
                // uses shm_attach directly, not attach_with_refcount).
                ShmProviderT::shm_close_fd(handle);
                uffd_poison(fault_addr, "meta_mmap_failed");
                continue;
            }

            // Register in g_fh_entries for future fast path.
            fh_register_segment(meta_va, static_cast<size_t>(meta_size),
                               fd, /*already_attached=*/true);

            // Track for cleanup in detach_all_regions().
            {
                UffdAttachedSeg e;
                e.addr = ptr;
                e.size = static_cast<size_t>(meta_size);
                static_assert(sizeof(handle) <= sizeof(e.handle_buf),
                              "ShmHandle too large for handle_buf");
                std::memcpy(e.handle_buf, &handle, sizeof(handle));
                // Multi-proc/node: record context for detach_with_refcount.
                e.lender_rank = fi.k;
                e.region_type = 0;  // METADATA
                e.seg_idx = 0;
                std::lock_guard<std::mutex> lk(g_uffd_attached_mtx);
                g_uffd_attached.push_back(std::move(e));
            }

            LOG_INFO("uffd handler: attached metadata for process "
                     << fi.k << " at VA 0x" << std::hex << meta_va
                     << " size=" << std::dec << meta_size);

            uffd_wake(fault_addr);
            continue;
        }

        // DATA fault: ensure metadata is attached, then discover segment.
        // First, check if process k's metadata is in the registry.
        uintptr_t meta_va = uffd_metadata_va(fi.k);
        int meta_entry = uffd_find_entry(meta_va);

        if (meta_entry < 0) {
            // Metadata for process k is not attached. Attach it first
            // (same logic as the METADATA path above).
            BootstrapBlock* bootstrap = static_cast<BootstrapBlock*>(
                g_uffd_bootstrap_ptr.load(std::memory_order_acquire));

            if (!bootstrap || fi.k < 0 || fi.k >= static_cast<int>(MAX_PROCESSES)) {
                uffd_poison(fault_addr, "data_meta_bootstrap_null");
                continue;
            }

            uint32_t slot_ready = bootstrap->slots[fi.k].ready.load(
                std::memory_order_acquire);
            if (slot_ready != 1) {
                uffd_poison(fault_addr, "data_meta_slot_not_ready");
                continue;
            }

            uint64_t meta_size = bootstrap->metadata_sizes[fi.k].load(
                std::memory_order_acquire);
            if (meta_size == 0) {
                uffd_poison(fault_addr, "data_meta_size_zero");
                continue;
            }

            const char* heap_id = g_uffd_heap_id.load(std::memory_order_acquire);
            if (!heap_id) {
                uffd_poison(fault_addr, "data_meta_heap_id_null");
                continue;
            }

            std::string name = ShmProviderT::shm_name(
                std::string(heap_id), fi.k, 0 /*METADATA*/, 0);

            auto attach_res = ShmProviderT::shm_attach(name);
            if (is_err(attach_res)) {
                LOG_WARN("uffd handler: shm_attach failed for metadata "
                          << name);
                uffd_poison(fault_addr, "data_meta_attach_failed");
                continue;
            }

            auto handle = unwrap(attach_res);
            int fd = ShmProviderT::shm_handle_fd(handle);
            if (fd < 0) {
                uffd_poison(fault_addr, "data_meta_fd_invalid");
                continue;
            }

            void* ptr = ::mmap(reinterpret_cast<void*>(meta_va),
                              static_cast<size_t>(meta_size),
                              PROT_READ | PROT_WRITE,
                              MAP_SHARED | MAP_FIXED, fd, 0);
            if (ptr == MAP_FAILED) {
                LOG_WARN("uffd handler: mmap failed for metadata "
                          << name << ": " << strerror(errno));
                // §9a: Use shm_close_fd (NOT shm_close) to avoid premature detach.
                ShmProviderT::shm_close_fd(handle);
                uffd_poison(fault_addr, "data_meta_mmap_failed");
                continue;
            }

            fh_register_segment(meta_va, static_cast<size_t>(meta_size),
                               fd, /*already_attached=*/true);

            // Track for cleanup in detach_all_regions().
            {
                UffdAttachedSeg e;
                e.addr = ptr;
                e.size = static_cast<size_t>(meta_size);
                static_assert(sizeof(handle) <= sizeof(e.handle_buf),
                              "ShmHandle too large for handle_buf");
                std::memcpy(e.handle_buf, &handle, sizeof(handle));
                // NEW (multi-proc/node): context for detach_with_refcount.
                e.lender_rank = fi.k;
                e.region_type = 0;  // METADATA
                e.seg_idx = 0;
                std::lock_guard<std::mutex> lk(g_uffd_attached_mtx);
                g_uffd_attached.push_back(std::move(e));
            }

            LOG_INFO("uffd handler: attached metadata for process "
                     << fi.k << " (data fault path) at VA 0x"
                     << std::hex << meta_va << std::dec);

            meta_entry = uffd_find_entry(meta_va);
        }

        // Now safe to read metadata — PTE is present.
        // Read SharedLayout header at metadata base+0 for seg_dir offsets.
        char* meta_base = reinterpret_cast<char*>(meta_va);
        SharedLayout* layout = reinterpret_cast<SharedLayout*>(meta_base);

        size_t seg_dir_offset;
        switch (fi.bracket) {
            case 0: seg_dir_offset = layout->seg_dir_small_offset; break;
            case 1: seg_dir_offset = layout->seg_dir_large_offset; break;
            case 2: seg_dir_offset = layout->seg_dir_huge_offset; break;
            default: uffd_poison(fault_addr, "invalid_bracket"); continue;
        }

        if (seg_dir_offset == 0) {
            uffd_poison(fault_addr, "seg_dir_offset_zero");
            continue;
        }

        SegmentDirectory* dir = reinterpret_cast<SegmentDirectory*>(
            meta_base + seg_dir_offset);

        uint32_t dir_count = dir->count.load(std::memory_order_acquire);
        if (fi.s >= static_cast<int>(dir_count)) {
            // Segment doesn't exist yet.
            uffd_poison(fault_addr, "seg_not_in_dir");
            continue;
        }

        uint32_t seg_ready = dir->descs[fi.s].ready.load(
            std::memory_order_acquire);
        if (seg_ready != 1) {
            // Segment not published yet.
            uffd_poison(fault_addr, "seg_not_ready");
            continue;
        }

        // Read segment name and size from the descriptor.
        char seg_name[64];
        std::memcpy(seg_name, dir->descs[fi.s].name, 64);
        size_t seg_size = dir->descs[fi.s].total_size;

        if (seg_name[0] == '\0' || seg_size == 0) {
            uffd_poison(fault_addr, "seg_name_or_size_zero");
            continue;
        }

        // Attach the data segment.
        std::string data_name(seg_name);
        auto data_res = ShmProviderT::shm_attach(data_name);
        if (is_err(data_res)) {
            LOG_WARN("uffd handler: shm_attach failed for data segment "
                      << data_name);
            uffd_poison(fault_addr, "data_attach_failed");
            continue;
        }

        auto data_handle = unwrap(data_res);
        int data_fd = ShmProviderT::shm_handle_fd(data_handle);
        if (data_fd < 0) {
            uffd_poison(fault_addr, "data_fd_invalid");
            continue;
        }

        // mmap the entire data segment.
        uintptr_t data_va = uffd_data_va(fi.k, fi.bracket, fi.s);
        void* dptr = ::mmap(reinterpret_cast<void*>(data_va), seg_size,
                           PROT_READ | PROT_WRITE,
                           MAP_SHARED | MAP_FIXED, data_fd, 0);
        if (dptr == MAP_FAILED) {
            LOG_WARN("uffd handler: mmap failed for data segment "
                      << data_name << ": " << strerror(errno));
            // §9a: Use shm_close_fd (NOT shm_close) to avoid premature detach.
            ShmProviderT::shm_close_fd(data_handle);
            uffd_poison(fault_addr, "data_mmap_failed");
            continue;
        }

        // Register in g_fh_entries for future fast path.
        fh_register_segment(data_va, seg_size, data_fd,
                           /*already_attached=*/true);

        // Track for cleanup in detach_all_regions().
        {
            UffdAttachedSeg e;
            e.addr = dptr;
            e.size = seg_size;
            static_assert(sizeof(data_handle) <= sizeof(e.handle_buf),
                          "ShmHandle too large for handle_buf");
            std::memcpy(e.handle_buf, &data_handle, sizeof(data_handle));
            // NEW (multi-proc/node): context for detach_with_refcount.
            e.lender_rank = fi.k;
            e.region_type = fi.bracket + 1;  // DATA_SMALL=1, DATA_LARGE=2, DATA_HUGE=3
            e.seg_idx = fi.s;
            std::lock_guard<std::mutex> lk(g_uffd_attached_mtx);
            g_uffd_attached.push_back(std::move(e));
        }

        LOG_INFO("uffd handler: attached data segment p" << fi.k
                 << " b" << fi.bracket << " s" << fi.s
                 << " at VA 0x" << std::hex << data_va
                 << " size=" << std::dec << seg_size);

        uffd_wake(fault_addr);
    }

    LOG_INFO("uffd handler thread exiting");
}

// -------------------------------------------------------------------------
// Uninstall the uffd handler.
// Called by disable_userfaultfd(), soft_reset(), and reset().
// In fork children, the handler thread is not inherited — set
// is_fork_child=true to skip join and reset the thread object.
// -------------------------------------------------------------------------

inline void uffd_uninstall(bool is_fork_child = false) {
    // Always detach/reset the thread first to prevent std::terminate
    // from std::thread destructor (especially in fork children where the
    // thread object is copied but the thread itself doesn't exist).
    //
    // NOTE: std::thread::operator= on a joinable thread calls std::terminate,
    //       so we use detach() instead — it makes the object non-joinable
    //       without trying to join (safe for fork children where the OS
    //       thread doesn't exist in this process).
    if (g_uffd_thread.joinable()) {
        if (!is_fork_child && g_uffd_active.load(std::memory_order_acquire)) {
            // Main process: stop and join for clean shutdown.
            g_uffd_running.store(false, std::memory_order_release);
            try {
                g_uffd_thread.join();
            } catch (...) {
                // join() threw (rare) — detach so the destructor is safe.
                try { g_uffd_thread.detach(); } catch (...) {}
            }
        } else {
            // Fork child or already inactive: detach. detach() updates the
            // local thread object to non-joinable even if the underlying
            // pthread doesn't exist in this process (pthread_detach error
            // is ignored by std::thread::detach).
            try { g_uffd_thread.detach(); } catch (...) {}
        }
    }

    if (!g_uffd_active.load(std::memory_order_acquire)) return;

    g_uffd_active.store(false, std::memory_order_release);

    // Stop the handler thread (already joined above in main process).
    g_uffd_running.store(false, std::memory_order_release);

    // Close the uffd fd.
    if (g_uffd_fd >= 0) {
        ::close(g_uffd_fd);
        g_uffd_fd = -1;
    }

    // NOTE: We deliberately do NOT mprotect the reservation back to
    // PROT_NONE here. On UBSE, the 28TB range contains device-backed
    // mappings (metadata + data segments) created after the uffd was
    // started. mprotect(PROT_NONE) on those ranges causes a kernel-level
    // crash (core dump). Since uffd_uninstall() is called from the
    // destructor (process is exiting), the OS will reclaim all mappings
    // anyway — the PROT_NONE safety measure is unnecessary.

    // Clear uffd globals.
    g_uffd_va_base.store(0, std::memory_order_release);

    // Munmap the constructor(102)'s separate read-only bootstrap mmap.
    // This is tracked separately (g_uffd_ctor_bs_mmap) because
    // g_uffd_bootstrap_ptr may have been overwritten by
    // try_enable_uffd_locked() to point at backend_.bootstrap_block_.
    // The constructor's mmap must be released before cleanup_bootstrap_mapping()
    // calls ubs_mem_shm_detach — the OBMM driver rejects detach (10700)
    // while there's an active mmap on the device-backed shm.
    void* ctor_bs = g_uffd_ctor_bs_mmap.exchange(nullptr,
                                                   std::memory_order_acq_rel);
    if (ctor_bs) {
        ::munmap(ctor_bs, sizeof(BootstrapBlock));
    }
    g_uffd_bootstrap_ptr.store(nullptr, std::memory_order_release);

    const char* heap_id = g_uffd_heap_id.exchange(nullptr,
                                                   std::memory_order_acq_rel);
    if (heap_id) ::free(const_cast<char*>(heap_id));

    // Clear the segment registry (fds are stale, especially in fork child).
    g_fh_active.store(false, std::memory_order_release);
    fh_clear_all();

    LOG_INFO("uffd handler uninstalled (fork_child=" << is_fork_child << ")");
}

// -------------------------------------------------------------------------
// Constructor (priority 102): auto-enable uffd for consumers that don't
// call any uballoc API. Runs after uballoc_early_va_init (priority 101)
// which creates the PROT_NONE VA reservation.
//
// If the bootstrap shm already exists (P0 has started), this constructor:
//   1. Attaches bootstrap read-only (PROT_READ, MAP_SHARED)
//   2. Reads va_base from the BootstrapBlock
//   3. mprotect the VA reservation from PROT_NONE → PROT_READ|PROT_WRITE
//   4. Creates the uffd fd, UFFDIO_API, UFFDIO_REGISTER
//   5. Spawns the handler thread
//
// If the bootstrap doesn't exist yet (P0 hasn't started), the constructor
// silently defers — do_init() will call try_enable_uffd_locked()
// as a fallback.
//
// The double bootstrap mapping is acceptable: the constructor's PROT_READ
// mapping (used by the handler thread to read bootstrap fields) and
// bootstrap_join()'s PROT_READ|PROT_WRITE mapping (used by the backend
// for read/write access) are both MAP_SHARED to the same shm. ~4KB waste.
// -------------------------------------------------------------------------

#ifdef UBALLOC_USE_UBSE
  using CtorShmProvider = UBShmProvider;
#else
  using CtorShmProvider = PosixShmProvider;
#endif

__attribute__((constructor(102)))
static void uballoc_uffd_auto_init() {
    if (g_uffd_ctor_attempted) return;
    g_uffd_ctor_attempted = true;

    if (!g_early_va.reserved) return;  // UBALLOC_HEAP_ID not set or VA failed

    const char* heap_id = ::getenv("UBALLOC_HEAP_ID");
    if (!heap_id || !*heap_id) return;

    // If uffd is already active (e.g., init already ran), skip.
    if (g_uffd_active.load(std::memory_order_acquire)) return;
    if (g_fh_active.load(std::memory_order_acquire)) return;

    // Register the UBSE no-op log callback BEFORE calling shm_attach.
    // provider_init() is normally called from init()/init(),
    // but the constructor runs BEFORE those — without this call, UBSE
    // prints verbose runtime logs during the bootstrap attach probe.
    CtorShmProvider::provider_init();

    // Try to attach bootstrap shm.
    std::string bs_name = CtorShmProvider::bootstrap_shm_name(
        std::string(heap_id));
    auto bs_res = CtorShmProvider::shm_attach(bs_name);
    if (is_err(bs_res)) {
        // Bootstrap doesn't exist yet (P0 hasn't started). Defer to init.
        return;
    }
    auto bs_handle = unwrap(bs_res);
    int bs_fd = CtorShmProvider::shm_handle_fd(bs_handle);

    // mmap bootstrap read-only to read va_base.
    void* bs_ptr = ::mmap(nullptr, sizeof(BootstrapBlock),
                          PROT_READ, MAP_SHARED, bs_fd, 0);
    // Close the fd but DON'T detach the borrow relationship (shm_close
    // would call ubs_mem_shm_detach). The mmap stays alive (Linux keeps
    // the mapping after fd close), and the uffd handler can still read
    // from it as long as the borrow persists.
    //
    // Detaching here and re-attaching in bootstrap_join() creates a rapid
    // attach→detach→attach→detach cycle that confuses the UBSE daemon.
    // At cleanup, ubs_mem_shm_detach fails with 1005 (INTERNAL) because
    // the borrow relationship is in an inconsistent state from the cycle.
    //
    // By NOT detaching here, bootstrap_join() sees EXISTED and reuses the
    // existing borrow. cleanup_bootstrap_mapping() is the ONLY detach call,
    // which works correctly.
    ::close(bs_fd);

    if (bs_ptr == MAP_FAILED) {
        return;
    }

    BootstrapBlock* bootstrap = static_cast<BootstrapBlock*>(bs_ptr);

    // Check if bootstrap is ready (P0 has published va_base).
    if (bootstrap->magic.load(std::memory_order_acquire) != BOOTSTRAP_MAGIC ||
        bootstrap->ready.load(std::memory_order_acquire) != 1) {
        ::munmap(bs_ptr, sizeof(BootstrapBlock));
        return;
    }

    uintptr_t va_base = bootstrap->va_base.load(std::memory_order_acquire);

    // mprotect the VA reservation from PROT_NONE to PROT_READ|PROT_WRITE.
    if (::mprotect(reinterpret_cast<void*>(va_base),
                   VA_RESERVATION_SIZE,
                   PROT_READ | PROT_WRITE) != 0) {
        ::munmap(bs_ptr, sizeof(BootstrapBlock));
        return;
    }

    // Create uffd fd.
    int uffd = static_cast<int>(
        ::syscall(SYS_userfaultfd, O_CLOEXEC | O_NONBLOCK));
    if (uffd < 0) {
        // uffd unavailable — init will fall back to SIGSEGV handler.
        ::mprotect(reinterpret_cast<void*>(va_base),
                   VA_RESERVATION_SIZE, PROT_NONE);
        ::munmap(bs_ptr, sizeof(BootstrapBlock));
        return;
    }

    // UFFDIO_API.
    struct uffdio_api api;
    api.api = UFFD_API;
    api.features = UFFD_FEATURE_MISSING_SHMEM;
    api.ioctls = 0;
    if (::ioctl(uffd, UFFDIO_API, &api) != 0) {
        ::close(uffd);
        ::mprotect(reinterpret_cast<void*>(va_base),
                   VA_RESERVATION_SIZE, PROT_NONE);
        ::munmap(bs_ptr, sizeof(BootstrapBlock));
        return;
    }

    // UFFDIO_REGISTER the entire VA reservation.
    struct uffdio_register reg;
    reg.range.start = va_base;
    reg.range.len = VA_RESERVATION_SIZE;
    reg.mode = UFFDIO_REGISTER_MODE_MISSING;
    reg.ioctls = 0;
    if (::ioctl(uffd, UFFDIO_REGISTER, &reg) != 0) {
        ::close(uffd);
        ::mprotect(reinterpret_cast<void*>(va_base),
                   VA_RESERVATION_SIZE, PROT_NONE);
        ::munmap(bs_ptr, sizeof(BootstrapBlock));
        return;
    }

    // Set globals.
    g_uffd_fd = uffd;
    g_uffd_va_base.store(va_base, std::memory_order_release);
    g_uffd_bootstrap_ptr.store(bootstrap, std::memory_order_release);
    g_uffd_ctor_bs_mmap.store(bs_ptr, std::memory_order_release);
    g_uffd_heap_id.store(::strdup(heap_id), std::memory_order_release);
    g_fh_active.store(true, std::memory_order_release);

    // Spawn handler thread.
    g_uffd_active.store(true, std::memory_order_release);
    g_uffd_running.store(true, std::memory_order_release);
    g_uffd_thread = std::thread(uffd_handler_thread<CtorShmProvider>);

    // Install SIGSEGV+SIGBUS handlers as PERMANENT SAFETY NET.
    // *** MAJOR KNOWN ISSUE: UBSE VMA DESTRUCTION ***
    // The UBSE device driver DESTROYS existing mmap'd VMAs as a side
    // effect of cross-process shm operations. uffd CANNOT catch these
    // faults (VMA destroyed, not missing page). The SIGSEGV handler
    // re-mmaps from g_fh_entries. See fault_handler.hpp for full details.
    // Same logic as step 10 in try_enable_uffd_locked (global.hpp).
    // MUST set g_fh_va_base/end BEFORE fh_install.
    g_fh_va_base.store(va_base, std::memory_order_release);
    g_fh_va_end.store(va_base + VA_RESERVATION_SIZE,
                      std::memory_order_release);
    fh_install();

    LOG_INFO("uballoc_uffd_auto_init: uffd handler started at VA 0x"
             << std::hex << va_base << std::dec
             << " (constructor priority 102)");
}

}  // namespace uballoc
