// SPDX-License-Identifier: Apache-2.0

// fault_handler.hpp — SIGSEGV/SIGBUS handler: lazy-attach + UBSE VMA
// destruction safety net.
//
// This module installs SIGSEGV and SIGBUS handlers that:
//   1. Check if the fault address is inside uballoc's VA reservation.
//   2. Scan a signal-safe segment registry for the matching segment.
//   3. If found with an open fd, mmaps it (MAP_FIXED, PROT_RW).
//   4. Returns — the faulting instruction re-executes and succeeds.
//   5. If not found or mmap fails, chains to the previous handler.
//
// *** MAJOR KNOWN ISSUE: UBSE VMA DESTRUCTION ***
// The UBSE device driver or runtime DESTROYS existing mmap'd VMAs at
// fixed addresses as a side effect of cross-process shm_create/shm_attach
// operations. When process P1 creates or attaches a shm, existing DEVICE
// mappings in process P0's address space (at P0's OWN slab VAs or P1's
// slab VAs) are sometimes unmapped — the VMA is completely destroyed
// (not PROT_NONE, just gone from /proc/self/maps).
//
// This means:
//   - uffd CANNOT catch these faults — the VMA is destroyed, not
//     "missing page". uffd only catches missing-page faults on
//     uffd-registered VMAs. A destroyed VMA has no uffd registration.
//   - Without the SIGSEGV handler, the process crashes (raw SIGSEGV).
//   - The handler resolves by re-mmap from the g_fh_entries registry
//     (which has the fd from the original create/attach).
//
// The handler is installed EVEN WHEN uffd IS ACTIVE (in both the
// constructor path uballoc_uffd_auto_init and try_enable_uffd_locked).
// This is necessary because uffd alone cannot handle VMA destruction.
//
// Observed in test_multi_thread (multi-threaded STL container clear+refill
// with cross-process access). See:
//   - "fh_sigsegv: resolved remap fault=0x..." in stderr — handler
//     re-mmaped a lost mapping (the UBSE VMA destruction case)
//   - doc/user_manual.md "Known Issues" section
//
// Root cause: UBSE device driver behavior (outside uballoc's control).
// Fix requires UBSE team investigation.
//
// The registry is populated:
//   - At enable_fault_handler() time, for all currently-attached segments.
//   - By attach_data_segment / create_and_map_own_region / etc., for each
//     segment they attach AFTER the handler is enabled. The fd is registered
//     BEFORE the mmap so that a concurrent fault can use it.
//
// For segments not yet discovered (created by another process after our
// last check_remote_segments()), the handler cannot help — there is no
// open fd and ubs_mem_shm_attach is not async-signal-safe. Those segments
// are attached eagerly on the next check_remote_segments() call.
//
// No sigaltstack: the handler runs on the faulting thread's normal stack.
// This is safe because our faults are page faults on unmapped VAs (not
// stack overflow or corruption). The handler does one mmap and returns —
// minimal stack usage. sigaltstack is only needed when the normal stack
// is unavailable (overflow/corruption), which is not our case.

#pragma once

#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstddef>
#include <sys/mman.h>
#include <unistd.h>
#include <cstring>
#include <cerrno>
#include <cstdio>

#include "shm_provider.hpp"  // VA_RESERVATION_SIZE, g_va_reservation_*

namespace uballoc {

// Maximum number of segments tracked by the fault handler.
// Each entry corresponds to one shm-backed segment (metadata or data).
// MAX_PROCESSES=8, with 1 metadata + up to 3 brackets * ~8 segments each,
// 128 entries provides comfortable headroom.
constexpr size_t FH_MAX_ENTRIES = 128;

// One registry entry per segment. All fields are atomic for signal-safety.
// The handler reads these fields with acquire loads; the backend writes
// them with release stores. The `va` field doubles as the "occupied" flag
// (0 = empty slot).
struct FaultHandlerEntry {
    std::atomic<uintptr_t> va;        // 0 = empty slot, else segment start VA
    std::atomic<uintptr_t> va_end;    // va + size (inclusive-exclusive)
    std::atomic<int> fd;              // -1 = no fd, >= 0 = device/shm fd
    std::atomic<uint32_t> attached;   // 0 = not mmaped, 1 = mmaped (by handler or eager path)
    std::atomic<uint32_t> valid;      // 0 = invalid/stale, 1 = fd is usable
};

// The registry. Scanned linearly by the handler (O(128) = ~200ns, fast
// enough for a signal handler). Inline so each TU sees the same storage.
inline FaultHandlerEntry g_fh_entries[FH_MAX_ENTRIES];

// Signal-safe globals. Populated by enable_fault_handler() before the
// handler is installed. Read-only from the handler (atomic loads).
inline std::atomic<bool> g_fh_active{false};
inline std::atomic<uintptr_t> g_fh_va_base{0};
inline std::atomic<uintptr_t> g_fh_va_end{0};

// Old SIGSEGV handler for chaining. Preserves user-installed handlers
// (e.g., jemalloc's bootstrap fault handler, language runtimes, profilers).
// Stored as a plain struct (not atomic) — written once at install time
// under no concurrent signal, read by the handler.
inline struct sigaction g_fh_old_sa;
inline struct sigaction g_fh_old_sa_sigbus;

// -------------------------------------------------------------------------
// Registry management (called by the backend, NOT from signal context).
// -------------------------------------------------------------------------

// Register a segment for fault-handler-based lazy attach.
//
// Called by attach_data_segment / create_and_map_own_region /
// attach_and_map_remote_region AFTER opening the fd but BEFORE the mmap.
// This ensures that if another thread faults on the segment VA between
// the fd-open and the mmap, the handler can use the cached fd to mmap
// the segment.
//
// Parameters:
//   va                — segment start VA (must be nonzero)
//   size              — segment size in bytes
//   fd                — device/shm fd (>= 0)
//   already_attached  — true if the segment is already mmaped (e.g., by
//                       the eager path or at enable_fault_handler time).
//                       The handler won't try to mmap it unless it faults.
//
// No-op if the handler is not active. Thread-safe (uses CAS to claim a
// slot). If the table is full, the segment won't be handler-attachable
// (a warning is printed to stderr).
inline void fh_register_segment(uintptr_t va, size_t size, int fd,
                                 bool already_attached) {
    if (!g_fh_active.load(std::memory_order_acquire)) return;
    if (va == 0 || fd < 0) return;

    uintptr_t va_end = va + size;
    uint32_t init_attached = already_attached ? 1u : 0u;

    for (size_t i = 0; i < FH_MAX_ENTRIES; ++i) {
        uintptr_t expected = 0;
        // Try to claim an empty slot.
        if (g_fh_entries[i].va.load(std::memory_order_relaxed) != 0) continue;
        if (g_fh_entries[i].va.compare_exchange_strong(
                expected, va,
                std::memory_order_acq_rel, std::memory_order_acquire)) {
            g_fh_entries[i].va_end.store(va_end, std::memory_order_relaxed);
            g_fh_entries[i].fd.store(fd, std::memory_order_relaxed);
            g_fh_entries[i].attached.store(init_attached,
                                            std::memory_order_relaxed);
            g_fh_entries[i].valid.store(1, std::memory_order_release);
            return;
        }
        // Lost the race for this slot — try the next one.
    }

    // Table full. Defer log (not signal context, so this is safe).
    uballoc::log::defer_log(uballoc::LogLevel::Warn,
            "uballoc fault_handler: segment table full (%zu entries), "
            "segment at VA 0x%lx will not be handler-attachable",
            FH_MAX_ENTRIES, (unsigned long)va);
}

// Mark a segment as attached (after eager mmap succeeds).
// Idempotent — safe to call on a segment that's already marked attached.
inline void fh_mark_attached(uintptr_t va) {
    if (!g_fh_active.load(std::memory_order_acquire)) return;
    for (size_t i = 0; i < FH_MAX_ENTRIES; ++i) {
        if (g_fh_entries[i].va.load(std::memory_order_relaxed) == va) {
            g_fh_entries[i].attached.store(1, std::memory_order_release);
            return;
        }
    }
}

// Invalidate a segment entry (e.g., the segment was munmaped or the fd
// was closed). Frees the slot for reuse.
inline void fh_invalidate_segment(uintptr_t va) {
    if (!g_fh_active.load(std::memory_order_acquire)) return;
    for (size_t i = 0; i < FH_MAX_ENTRIES; ++i) {
        if (g_fh_entries[i].va.load(std::memory_order_relaxed) == va) {
            g_fh_entries[i].valid.store(0, std::memory_order_release);
            // Close the cached fd BEFORE clearing it. On UBSE, this fd
            // represents an active attach/borrow — leaking it (storing -1
            // without closing) causes shm_delete to fail with 1024
            // (ATTACH_USING) because the daemon still sees an active
            // attach for this shm.
            int fd = g_fh_entries[i].fd.load(std::memory_order_relaxed);
            if (fd >= 0) {
                ::close(fd);
            }
            g_fh_entries[i].fd.store(-1, std::memory_order_relaxed);
            g_fh_entries[i].attached.store(0, std::memory_order_relaxed);
            g_fh_entries[i].va.store(0, std::memory_order_release);
            return;
        }
    }
}

// Clear all entries (called by disable_fault_handler and soft_reset).
inline void fh_clear_all() {
    for (size_t i = 0; i < FH_MAX_ENTRIES; ++i) {
        g_fh_entries[i].valid.store(0, std::memory_order_relaxed);
        g_fh_entries[i].fd.store(-1, std::memory_order_relaxed);
        g_fh_entries[i].attached.store(0, std::memory_order_relaxed);
        g_fh_entries[i].va.store(0, std::memory_order_release);
    }
}

// -------------------------------------------------------------------------
// Signal-safe diagnostic logging.
//
// The SIGSEGV handler runs in signal context — must NOT use LOG_*,
// malloc, std::cerr, etc. (not async-signal-safe). These helpers use
// only async-signal-safe functions: write(2), manual hex formatting,
// and atomic loads (for g_fh_va_base).
//
// fh_log_fault() is called at every failure path in fh_sigsegv_handler
// where it chains to the old handler (i.e., it gave up resolving the
// fault). It writes the fault address, a reason string, and the offset
// from the VA base to stderr (fd 2). The offset helps identify which
// process's slab the fault is in (compare against the VA layout).
//
// Permanent diagnostic — no compile flag needed. Only fires on handler
// failure, not on every fault (resolved faults are silent).

inline int fh_hex(char* buf, uintptr_t val) {
    if (val == 0) { buf[0] = '0'; return 1; }
    char tmp[16];
    int n = 0;
    while (val > 0) { tmp[n++] = "0123456789abcdef"[val & 0xf]; val >>= 4; }
    for (int i = 0; i < n; i++) buf[i] = tmp[n - 1 - i];
    return n;
}

inline void fh_log_fault(uintptr_t fault_addr, const char* reason) {
    char buf[300];
    int len = 0;

    const char* p = "fh_sigsegv: fault=0x";
    while (*p && len < 280) buf[len++] = *p++;
    len += fh_hex(buf + len, fault_addr);

    p = " reason=";
    while (*p && len < 280) buf[len++] = *p++;
    while (*reason && len < 280) buf[len++] = *reason++;

    uintptr_t base = g_fh_va_base.load(std::memory_order_acquire);
    if (base != 0 && fault_addr >= base) {
        p = " off=0x";
        while (*p && len < 280) buf[len++] = *p++;
        len += fh_hex(buf + len, fault_addr - base);
    }

    buf[len++] = '\n';
    write(2, buf, len);
}

// Signal-safe /proc/self/maps dumper.
// Reads /proc/self/maps line by line, writes lines containing a hex
// prefix to stderr. Uses only async-signal-safe syscalls:
// open, read, write, close. Manual char-by-char prefix matching
// (no strstr — not guaranteed async-signal-safe on all platforms).
//
// Called from fh_log_fault_and_dump on crash — zero timing impact
// during normal operation (only runs when the process is about to
// terminate anyway).
inline void fh_dump_maps_signal_safe(uintptr_t fault_addr) {
    // Build the prefix: top 5 hex digits of fault_addr (e.g., "21020"
    // for 0x210200208070 — matches any line starting with 0x21020...).
    char prefix[8];
    int plen = 0;
    prefix[plen++] = '0';
    prefix[plen++] = 'x';
    // Shift right to get the top nibbles: fault_addr >> 32 gives the
    // top 32 bits. We want the first 5 hex digits of the full address.
    uintptr_t v = fault_addr;
    char digits[16];
    int dn = 0;
    if (v == 0) { digits[dn++] = '0'; }
    while (v > 0) { digits[dn++] = "0123456789abcdef"[v & 0xf]; v >>= 4; }
    // Take top 5 digits (or all if fewer)
    int start = (dn > 5) ? dn - 5 : 0;
    int count = dn - start;
    for (int i = 0; i < count; i++) prefix[plen++] = digits[dn - 1 - i];
    prefix[plen] = '\0';

    // Header
    char hdr[128];
    int hl = 0;
    const char* h = "fh_dump_maps: fault=0x";
    while (*h) hdr[hl++] = *h++;
    hl += fh_hex(hdr + hl, fault_addr);
    h = " prefix=";
    while (*h) hdr[hl++] = *h++;
    for (int i = 0; i < plen; i++) hdr[hl++] = prefix[i];
    hdr[hl++] = '\n';
    write(2, hdr, hl);

    // Read /proc/self/maps
    int fd = ::open("/proc/self/maps", O_RDONLY);
    if (fd < 0) {
        const char* msg = "  (failed to open /proc/self/maps)\n";
        write(2, msg, 29);
        return;
    }

    char rbuf[8192];
    char line[512];
    int line_pos = 0;
    ssize_t n;
    while ((n = ::read(fd, rbuf, sizeof(rbuf))) > 0) {
        for (ssize_t i = 0; i < n; i++) {
            if (rbuf[i] == '\n' || line_pos >= 511) {
                line[line_pos] = '\0';
                // Manual prefix match: does line contain the prefix?
                int match = 0;
                for (int j = 0; j + plen <= line_pos; j++) {
                    int found = 1;
                    for (int k = 0; k < plen; k++) {
                        if (line[j + k] != prefix[k]) { found = 0; break; }
                    }
                    if (found) { match = 1; break; }
                }
                if (match) {
                    write(2, "  ", 2);
                    write(2, line, line_pos);
                    write(2, "\n", 1);
                }
                line_pos = 0;
            } else {
                line[line_pos++] = rbuf[i];
            }
        }
    }
    ::close(fd);
}

// Log fault + dump maps, then chain to old handler.
inline void fh_log_fault_and_dump(uintptr_t fault_addr, const char* reason) {
    fh_log_fault(fault_addr, reason);
    fh_dump_maps_signal_safe(fault_addr);
}

// -------------------------------------------------------------------------
// The SIGSEGV handler.
// -------------------------------------------------------------------------

// Chain to the previous SIGSEGV handler. If there was none (SIG_DFL/SIG_IGN),
// restore the default and re-raise so the process terminates with a core dump.
//
// This function is signal-safe (only uses signal-safe syscalls: signal,
// raise). The old handler itself may or may not be signal-safe — that's
// the user's responsibility.
inline void fh_chain_to_old(int sig, siginfo_t* info) {
    if (g_fh_old_sa.sa_flags & SA_SIGINFO) {
        if (g_fh_old_sa.sa_sigaction != nullptr) {
            g_fh_old_sa.sa_sigaction(sig, info, nullptr);
            return;
        }
    } else {
        auto h = g_fh_old_sa.sa_handler;
        if (h != SIG_DFL && h != SIG_IGN && h != nullptr) {
            h(sig);
            return;
        }
    }
    // No user handler — default action (terminate + core dump).
    ::signal(sig, SIG_DFL);
    ::raise(sig);
}

// The SIGSEGV handler itself.
//
// Signal-safety notes:
//   - Only uses async-signal-safe functions: mmap, signal, raise.
//   - Does NOT call malloc, std::cerr, LOG_*, ubs_mem_shm_attach, etc.
//   - Reads atomic globals with acquire loads.
//   - Uses CAS on `attached` for multi-threaded idempotency (only one
//     thread does the mmap; others return and retry).
//
// SIGSEGV is blocked during handler execution (no SA_NODEFER), so
// re-entrant faults in the same thread trigger the default action
// (terminate). This is intentional — a fault inside the handler means
// something is fundamentally wrong (e.g., the registry is corrupted).
inline void fh_sigsegv_handler(int sig, siginfo_t* info, void* /*uctx*/) {
    uintptr_t fault_addr = reinterpret_cast<uintptr_t>(info->si_addr);

    // Step 1: Check if the fault is in uballoc's VA reservation.
    uintptr_t base = g_fh_va_base.load(std::memory_order_acquire);
    uintptr_t end = g_fh_va_end.load(std::memory_order_acquire);
    if (fault_addr < base || fault_addr >= end) {
        // Not our fault — chain to the previous handler.
        fh_log_fault_and_dump(fault_addr, "out_of_range");
        fh_chain_to_old(sig, info);
        return;
    }

    // Step 2: Scan the registry for the segment containing fault_addr.
    for (size_t i = 0; i < FH_MAX_ENTRIES; ++i) {
        uintptr_t seg_va = g_fh_entries[i].va.load(std::memory_order_acquire);
        if (seg_va == 0) continue;

        uintptr_t seg_end = g_fh_entries[i].va_end.load(std::memory_order_relaxed);
        if (fault_addr < seg_va || fault_addr >= seg_end) continue;

        // Found the segment. Check validity and fd.
        if (g_fh_entries[i].valid.load(std::memory_order_acquire) == 0) {
            // Entry is invalid — chain to old handler.
            fh_log_fault_and_dump(fault_addr, "invalid_entry");
            fh_chain_to_old(sig, info);
            return;
        }

        int fd = g_fh_entries[i].fd.load(std::memory_order_relaxed);
        if (fd < 0) {
            // No fd — can't attach. Chain to old handler.
            fh_log_fault_and_dump(fault_addr, "no_fd");
            fh_chain_to_old(sig, info);
            return;
        }

        uint32_t attached = g_fh_entries[i].attached.load(std::memory_order_acquire);

        if (attached == 0) {
            // Expected case: fd is open but segment not yet mmaped.
            // Try to claim the attach via CAS (multi-threaded idempotency).
            uint32_t expected = 0;
            if (!g_fh_entries[i].attached.compare_exchange_strong(
                    expected, 1,
                    std::memory_order_acq_rel, std::memory_order_acquire)) {
                // Another thread is attaching. Return; the faulting
                // instruction will re-execute. If the other thread
                // succeeds, the re-execution works. If it fails,
                // we'll fault again and retry.
                return;
            }
            // We won the CAS — do the mmap.
            size_t size = seg_end - seg_va;
            void* ptr = ::mmap(reinterpret_cast<void*>(seg_va), size,
                              PROT_READ | PROT_WRITE,
                              MAP_SHARED | MAP_FIXED, fd, 0);
            if (ptr != MAP_FAILED) {
                // Success — the faulting instruction re-executes and
                // succeeds (page is now backed).
                {
                    char mbuf[80];
                    int ml = 0;
                    const char* mp = "fh_sigsegv: resolved fault=0x";
                    while (*mp) mbuf[ml++] = *mp++;
                    ml += fh_hex(mbuf + ml, fault_addr);
                    mbuf[ml++] = '\n';
                    write(2, mbuf, ml);
                }
                return;
            }
            // mmap failed — reset attached so we can retry on the
            // next fault, then chain to the old handler.
            g_fh_entries[i].attached.store(0, std::memory_order_release);
            fh_log_fault_and_dump(fault_addr, "mmap_failed");
            fh_chain_to_old(sig, info);
            return;
        } else {
            // Unexpected case: attached == 1 but we faulted.
            // The mapping was lost (munmap, overwritten, mprotect).
            // Try to re-mmap directly (no CAS — concurrent re-mmaps
            // are safe since mmap(MAP_FIXED) is idempotent).
            size_t size = seg_end - seg_va;
            void* ptr = ::mmap(reinterpret_cast<void*>(seg_va), size,
                              PROT_READ | PROT_WRITE,
                              MAP_SHARED | MAP_FIXED, fd, 0);
            if (ptr != MAP_FAILED) {
                {
                    char mbuf[80];
                    int ml = 0;
                    const char* mp = "fh_sigsegv: resolved remap fault=0x";
                    while (*mp) mbuf[ml++] = *mp++;
                    ml += fh_hex(mbuf + ml, fault_addr);
                    mbuf[ml++] = '\n';
                    write(2, mbuf, ml);
                }
                return;
            }
            // Re-mmap failed — chain to old handler.
            fh_log_fault_and_dump(fault_addr, "remap_failed");
            fh_chain_to_old(sig, info);
            return;
        }
    }

    // No matching segment found in the registry. The faulting segment
    // hasn't been discovered yet (no open fd). Chain to old handler.
    fh_log_fault_and_dump(fault_addr, "no_segment");
    fh_chain_to_old(sig, info);
}

// -------------------------------------------------------------------------
// SIGBUS handler — catches uffd_poison delivery and other SIGBUS.
//
// When uffd is active, uffd_poison() delivers SIGBUS to the faulting
// thread. LOG_ERROR in uffd_poison() should fire first, but if the log
// uses buffered I/O it may not flush before the process dies. This
// handler provides a signal-safe fallback log via fh_log_fault().
//
// Also catches SIGBUS from other sources (e.g., accessing truncated
// shm beyond its actual size after ftruncate/shm_detach_by_name).
//
// Signal-safe: only uses fh_log_fault (write+manual hex) and
// signal-safe syscalls (signal, raise).
// -------------------------------------------------------------------------
inline void fh_sigbus_handler(int sig, siginfo_t* info, void* /*uctx*/) {
    uintptr_t fault_addr = reinterpret_cast<uintptr_t>(info->si_addr);
    fh_log_fault_and_dump(fault_addr, "sigbus");
    // Restore default and re-raise (terminate + core dump).
    ::signal(sig, SIG_DFL);
    ::raise(sig);
}

// -------------------------------------------------------------------------
// Install / uninstall.
// -------------------------------------------------------------------------

// Install the SIGSEGV handler.
// Called by enable_fault_handler() in global.hpp, AFTER populating
// the signal-safe globals (g_fh_va_base, g_fh_va_end, registry entries).
//
// Safe to call multiple times — re-installs with the current old handler.
// Not signal-safe (calls sigaction) — call from normal context only.
//
// No sigaltstack: the handler runs on the faulting thread's normal stack.
// This is safe because our faults are page faults on unmapped VAs (not
// stack overflow or corruption). The handler only does one mmap and
// returns — minimal stack usage. sigaltstack is only needed when the
// normal stack is unavailable (overflow/corruption), which is not our
// case.
inline bool fh_install() {
    // Install the SIGSEGV handler.
    // SA_SIGINFO: use the 3-argument sa_sigaction (provides si_addr).
    // No SA_ONSTACK: run on the normal stack (no sigaltstack configured).
    // No SA_NODEFER: SIGSEGV is blocked during handler execution, so a
    //   re-entrant fault in the handler triggers the default action
    //   (terminate + core dump). This is intentional.
    struct sigaction sa;
    sa.sa_sigaction = fh_sigsegv_handler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    if (::sigaction(SIGSEGV, &sa, &g_fh_old_sa) != 0) {
        uballoc::log::defer_log(uballoc::LogLevel::Warn,
                "uballoc fault_handler: sigaction(SIGSEGV) failed: %s",
                strerror(errno));
        return false;
    }

    // Install the SIGBUS handler (catches uffd_poison delivery + truncated
    // shm access). Same flags as SIGSEGV. Installed even when uffd is
    // active — uffd catches page faults on the VA reservation via the
    // uffd fd, NOT via SIGBUS. SIGBUS from uffd_poison is delivered to
    // the faulting thread and would otherwise kill the process with no
    // diagnostic. Also catches SIGBUS from faults outside the VA
    // reservation (e.g., accessing a truncated shm beyond its size).
    struct sigaction sa_bus;
    sa_bus.sa_sigaction = fh_sigbus_handler;
    sa_bus.sa_flags = SA_SIGINFO;
    sigemptyset(&sa_bus.sa_mask);
    if (::sigaction(SIGBUS, &sa_bus, &g_fh_old_sa_sigbus) != 0) {
        uballoc::log::defer_log(uballoc::LogLevel::Warn,
                "uballoc fault_handler: sigaction(SIGBUS) failed: %s",
                strerror(errno));
        // Non-fatal — SIGSEGV handler is still installed.
    }

    g_fh_active.store(true, std::memory_order_release);
    return true;
}

// Uninstall the handlers and restore the previous SIGSEGV/SIGBUS handlers.
// Called by disable_fault_handler() and soft_reset().
inline void fh_uninstall() {
    if (!g_fh_active.load(std::memory_order_acquire)) return;

    g_fh_active.store(false, std::memory_order_release);
    ::sigaction(SIGSEGV, &g_fh_old_sa, nullptr);
    ::sigaction(SIGBUS, &g_fh_old_sa_sigbus, nullptr);

    fh_clear_all();
}

}  // namespace uballoc
