// SPDX-License-Identifier: Apache-2.0

#ifndef UBALLOC_H
#define UBALLOC_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize the allocator for this process.
 *
 * Reads `UBALLOC_HEAP_ID` from the environment and discovers the process rank
 * and total membership at runtime via a bootstrap shared-memory PID table.
 * No need to know process_id or total_processes in advance — every process
 * just sets `UBALLOC_HEAP_ID` to the same string and calls `uballoc_init()`.
 *
 * Usage:
 *   export UBALLOC_HEAP_ID=myheap
 *   ./worker & ./worker & ./worker   # any number, any order
 *
 * CONTRACT: applications MUST call `uballoc_init()` before any direct access
 * to remote shared memory (i.e., before dereferencing pointers obtained from
 * `uballoc_lookup_by_type*` or any out-of-band VA coordination). The
 * `uballoc_malloc`/`uballoc_free`/`uballoc_lookup_by_type*` family auto-calls
 * `uballoc_init()` on first use as a safety net, but explicit init() is the
 * recommended pattern for code that touches remote shms before any allocator
 * call.
 *
 * Calling this explicitly is optional when only using uballoc_malloc/free —
 * those functions auto-init on first use if `UBALLOC_HEAP_ID` is set.
 *
 * Idempotent: calling it again after initialization is a no-op.
 *
 * If `UBALLOC_HEAP_ID` is not set, this function aborts with an error.
 */
void uballoc_init(void);

/**
 * Initialize the allocator for this thread.
 *
 * `thread_id` must be (1) unique for each thread and (2) less than `thread_count`.
 */
void uballoc_init_thread(uint16_t thread_id);

void *uballoc_malloc(size_t size);

void uballoc_free(void *pointer);

void *uballoc_realloc(void *pointer, size_t size);

void *uballoc_memalign(size_t size, size_t alignment);

/**
 * Information about a published allocation, returned by lookup functions.
 * `owner_process` is -1 when the lookup found nothing.
 */
typedef struct {
    void   *address;        /* pointer to the published memory region      */
    size_t  size;           /* size in bytes of the published allocation   */
    uint32_t type_id;       /* application-defined type id                 */
    int     owner_process;  /* process id of the owner, or -1 if not found */
} uballoc_published_info_t;

/**
 * Allocate `size` bytes and publish the pointer under `type_id` in one call.
 *
 * On publish failure, the allocation is freed and NULL is returned.
 *
 * Race-condition note: the pointer is published before the caller can
 * initialize the memory. Callers needing data-readiness sync should store an
 * atomic ready flag inside the published region and set it after this call
 * returns; lookers should spin on that flag.
 *
 * Returns the allocated pointer on success, NULL on failure.
 */
void *uballoc_malloc_published(size_t size, uint32_t type_id);

/**
 * Non-blocking lookup: find the most recent allocation published under
 * `type_id` by any process. Writes the result into `out`. If nothing is
 * found, `out->owner_process` is set to -1.
 *
 * Returns 0 on success (found), non-zero if not found.
 */
int uballoc_lookup_by_type(uint32_t type_id, uballoc_published_info_t *out);

/**
 * Blocking lookup: polls every 100ms until `type_id` is found or the
 * timeout expires. A negative `timeout_ms` waits forever. Writes the
 * result into `out`.
 *
 * Returns 0 on success (found), non-zero on timeout.
 */
int uballoc_lookup_by_type_blocking(uint32_t type_id, int timeout_ms,
                                    uballoc_published_info_t *out);

/* -------------------------------------------------------------------------
 * Fault handler: SIGSEGV-handler-based lazy-attach for remote segments.
 *
 * After enabling, if a thread accesses a remote segment's VA before
 * check_remote_segments() has attached it, the handler mmaps the segment
 * using a pre-registered device fd. The faulting instruction re-executes
 * and succeeds. This is a safety net for the eager-attach race window;
 * the primary attach mechanism is still check_remote_segments().
 *
 * Must be called AFTER uballoc_init().
 * Opt-in — the handler is NOT installed by default. Applications with
 * their own SIGSEGV handler can install uballoc's first; non-uballoc
 * faults chain to the previous handler.
 *
 * Returns true on success, false on failure (e.g., sigaltstack alloc failed).
 * ------------------------------------------------------------------------- */
bool uballoc_enable_fault_handler(void);

/* Disable the fault handler. Restores the previous SIGSEGV handler and
 * clears the segment registry. Called automatically on reset()/soft_reset().
 * Safe to call even if the handler was never enabled. */
void uballoc_disable_fault_handler(void);

/* -------------------------------------------------------------------------
 * userfaultfd handler: primary lazy-attach when uffd is available.
 *
 * Tries userfaultfd first; falls back to the SIGSEGV handler if uffd
 * is unavailable (ENOSYS or EPERM). The uffd handler runs in a separate
 * thread in normal thread context, so it can discover + attach segments
 * on demand (calling ubs_mem_shm_attach, which uses sockets/malloc).
 *
 * Must be called AFTER uballoc_init().
 * Opt-in — the handler is NOT installed by default.
 *
 * Returns true on success, false on failure.
 * ------------------------------------------------------------------------- */
bool uballoc_enable_userfaultfd(void);

/* Disable the uffd handler. Stops the handler thread, closes the uffd fd,
 * and mprotect the reservation back to PROT_NONE. Called automatically on
 * reset()/soft_reset(). Safe to call even if the handler was never enabled. */
void uballoc_disable_userfaultfd(void);

/* -------------------------------------------------------------------------
 * Reset functions.
 *
 * uballoc_reset(): full teardown — detaches/deletes shms (UBSE) or
 * unlinks them (POSIX), then clears all state. Call this when a process
 * is done with the allocator and wants to re-init with a different
 * heap_id.
 *
 * uballoc_soft_reset(): clears in-memory state WITHOUT touching shms.
 * Used by fork children that inherited the parent's state.
 * ------------------------------------------------------------------------- */
void uballoc_reset(void);
void uballoc_soft_reset(void);

/* Returns true if the allocator has been initialized. */
bool uballoc_is_initialized(void);

/* -------------------------------------------------------------------------
 * Macro overload: uballoc_malloc(...) dispatches to uballoc_malloc_published
 * when called with 2 arguments (size, type_id), or to uballoc_malloc when
 * called with 1 argument (size). This provides a uniform call site:
 *
 *     void* a = uballoc_malloc(64);              // plain malloc
 *     void* b = uballoc_malloc(64, TYPE_BUFFER); // malloc + publish
 *
 * C++ callers should prefer the overloaded GlobalAllocator::malloc methods
 * directly. This macro is primarily for C callers who want the convenience
 * of a single entry point.
 *
 * To bypass the macro (e.g., taking a function pointer), use the explicit
 * function name: uballoc_malloc_published or uballoc_malloc.
 * ------------------------------------------------------------------------- */
#ifdef __cplusplus
/* In C++ the compiler resolves the overload directly; no macro needed.  The
 * macro is undefined so the function name refers to the plain C function. */
#else
#define UBALLOC_GET_MACRO(_1, _2, NAME, ...) NAME
#define uballoc_malloc(...) UBALLOC_GET_MACRO(__VA_ARGS__, uballoc_malloc_published, uballoc_malloc)(__VA_ARGS__)
#endif

#ifdef __cplusplus
}
#endif

#endif /* UBALLOC_H */
