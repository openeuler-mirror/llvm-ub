// SPDX-License-Identifier: Apache-2.0

#include "uballoc.hpp"
#include "uballoc.h"
#include <cstring>

extern "C" {

void uballoc_init(void) {
    uballoc::get_global_allocator().init();
}

void uballoc_init_thread(uint16_t thread_id) {
    uballoc::get_global_allocator().init_thread(thread_id);
}

void *uballoc_malloc(size_t size) {
    return uballoc::get_global_allocator().malloc(size);
}

void uballoc_free(void *pointer) {
    uballoc::get_global_allocator().free(pointer);
}

void *uballoc_realloc(void *pointer, size_t size) {
    return uballoc::get_global_allocator().realloc(pointer, size);
}

void *uballoc_memalign(size_t size, size_t alignment) {
    return uballoc::get_global_allocator().memalign(size, alignment);
}

void *uballoc_malloc_published(size_t size, uint32_t type_id) {
    return uballoc::get_global_allocator().malloc(size, type_id);
}

int uballoc_lookup_by_type(uint32_t type_id, uballoc_published_info_t *out) {
    if (!out) return -1;
    uballoc::PublishedInfo info =
        uballoc::get_global_allocator().lookup_by_type(type_id);
    out->address = info.address;
    out->size = info.size;
    out->type_id = info.type_id;
    out->owner_process = info.owner_process;
    return (info.owner_process >= 0) ? 0 : 1;
}

int uballoc_lookup_by_type_blocking(uint32_t type_id, int timeout_ms,
                                    uballoc_published_info_t *out) {
    if (!out) return -1;
    uballoc::PublishedInfo info =
        uballoc::get_global_allocator().lookup_by_type_blocking(type_id, timeout_ms);
    out->address = info.address;
    out->size = info.size;
    out->type_id = info.type_id;
    out->owner_process = info.owner_process;
    return (info.owner_process >= 0) ? 0 : 1;
}

bool uballoc_enable_fault_handler(void) {
    return uballoc::get_global_allocator().enable_fault_handler();
}

void uballoc_disable_fault_handler(void) {
    uballoc::get_global_allocator().disable_fault_handler();
}

bool uballoc_enable_userfaultfd(void) {
    return uballoc::get_global_allocator().enable_userfaultfd();
}

void uballoc_disable_userfaultfd(void) {
    uballoc::get_global_allocator().disable_userfaultfd();
}

void uballoc_reset(void) {
    uballoc::get_global_allocator().reset();
}

void uballoc_soft_reset(void) {
    uballoc::get_global_allocator().soft_reset();
}

bool uballoc_is_initialized(void) {
    return uballoc::get_global_allocator().is_initialized();
}

}
