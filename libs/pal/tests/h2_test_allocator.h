#ifndef H2_TEST_ALLOCATOR_H
#define H2_TEST_ALLOCATOR_H

#include "h2/pal/os/h2_pal_mem.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>

typedef struct h2_test_allocator {
    h2_pal_mem_api_t api;
    atomic_size_t calls;
    atomic_size_t live;
    atomic_size_t fail_on_call;
} h2_test_allocator_t;

typedef union h2_test_allocation {
#if defined(_MSC_VER) && !defined(__clang__)
    /* MSVC's C headers omit max_align_t; cover its scalar alignments. */
    long double alignment;
    long long integer_alignment;
    void *pointer_alignment;
#else
    max_align_t alignment;
#endif
    struct {
        h2_test_allocator_t *owner;
        size_t size;
    } info;
} h2_test_allocation_t;

static inline void *h2_test_alloc(void *user, size_t size) {
    h2_test_allocator_t *owner = user;
    size_t call = atomic_fetch_add(&owner->calls, 1u) + 1u;
    if (call == atomic_load(&owner->fail_on_call) ||
        size > SIZE_MAX - sizeof(h2_test_allocation_t)) return NULL;
    h2_test_allocation_t *block = malloc(sizeof(*block) + size);
    if (block == NULL) return NULL;
    block->info.owner = owner;
    block->info.size = size;
    atomic_fetch_add(&owner->live, 1u);
    return block + 1;
}

static inline void h2_test_free(void *user, void *ptr) {
    if (ptr == NULL) return;
    h2_test_allocator_t *owner = user;
    h2_test_allocation_t *block = (h2_test_allocation_t *)ptr - 1;
    assert(block->info.owner == owner);
    size_t live = atomic_fetch_sub(&owner->live, 1u);
    assert(live > 0u);
    (void)live;
    free(block);
}

static inline void *h2_test_realloc(void *user, void *ptr, size_t size) {
    if (ptr == NULL) return h2_test_alloc(user, size);
    if (size == 0u) {
        h2_test_free(user, ptr);
        return NULL;
    }
    h2_test_allocator_t *owner = user;
    h2_test_allocation_t *block = (h2_test_allocation_t *)ptr - 1;
    assert(block->info.owner == owner);
    size_t call = atomic_fetch_add(&owner->calls, 1u) + 1u;
    if (call == atomic_load(&owner->fail_on_call) ||
        size > SIZE_MAX - sizeof(*block)) return NULL;
    block = realloc(block, sizeof(*block) + size);
    if (block == NULL) return NULL;
    block->info.size = size;
    return block + 1;
}

static inline void h2_test_allocator_init(h2_test_allocator_t *allocator) {
    static const h2_pal_mem_vtable_t vtable = {
        .alloc = h2_test_alloc, .realloc = h2_test_realloc, .free = h2_test_free,
    };
    allocator->api = (h2_pal_mem_api_t){.user = allocator, .vtable = &vtable};
    atomic_init(&allocator->calls, 0u);
    atomic_init(&allocator->live, 0u);
    atomic_init(&allocator->fail_on_call, 0u);
}

#endif
