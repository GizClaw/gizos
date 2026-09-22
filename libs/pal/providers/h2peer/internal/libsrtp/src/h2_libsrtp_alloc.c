#include "config.h"
#include "h2_libsrtp_internal.h"

#include "alloc.h"
#include "crypto_kernel.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

srtp_debug_module_t srtp_mod_alloc = {
    0,
    "h2 pal alloc",
};

static _Thread_local h2_libsrtp_session_t *h2_libsrtp_active_session;

typedef union h2_libsrtp_arena_alignment {
    long double long_double_value;
    long long long_long_value;
    void *pointer_value;
} h2_libsrtp_arena_alignment_t;

void h2_libsrtp_allocator_enter(h2_libsrtp_session_t *session) {
    h2_libsrtp_active_session = session;
}

void h2_libsrtp_allocator_leave(void) {
    h2_libsrtp_active_session = NULL;
}

/* Heap blocks retain their owner even when another thread destroys a session.
 * Arena blocks keep the existing bounded layout; every operation, including
 * destruction, enters the owning session before calling upstream code. */
typedef union h2_libsrtp_block {
    max_align_t alignment;
    h2_pal_mem_api_t owner;
} h2_libsrtp_block_t;

static int h2_libsrtp_arena_contains(
    const h2_libsrtp_session_t *session,
    const void *ptr) {
    if (session == NULL || session->operation_arena == NULL || ptr == NULL) {
        return 0;
    }
    uintptr_t address = (uintptr_t)ptr;
    uintptr_t start = (uintptr_t)session->operation_arena;
    return address >= start &&
           address - start < session->operation_arena_capacity;
}

void *srtp_crypto_alloc(size_t size) {
    const size_t alignment = _Alignof(h2_libsrtp_arena_alignment_t);
    h2_libsrtp_session_t *session = h2_libsrtp_active_session;
    size_t aligned_used;
    h2_libsrtp_block_t *block;
    if (!h2_libsrtp_state.ready || size == 0u ||
        size > SIZE_MAX - sizeof(*block)) {
        return NULL;
    }
    if (session != NULL && session->operation_arena != NULL) {
        if (session->operation_arena_used > SIZE_MAX - (alignment - 1u)) {
            return NULL;
        }
        aligned_used =
            (session->operation_arena_used + alignment - 1u) &
            ~(alignment - 1u);
        if (aligned_used > session->operation_arena_capacity ||
            size > session->operation_arena_capacity - aligned_used) {
            return NULL;
        }
        void *ptr = session->operation_arena + aligned_used;
        session->operation_arena_used = aligned_used + size;
        memset(ptr, 0, size);
        return ptr;
    }
    const h2_pal_mem_api_t *mem = session != NULL
                                      ? &session->mem : &h2_libsrtp_state.mem;
    block = h2_pal_mem_alloc(mem, sizeof(*block) + size);
    if (block == NULL) return NULL;
    block->owner = *mem;
    memset(block + 1, 0, size);
    return block + 1;
}

void srtp_crypto_free(void *ptr) {
    if (ptr != NULL &&
        !h2_libsrtp_arena_contains(h2_libsrtp_active_session, ptr)) {
        h2_libsrtp_block_t *block = (h2_libsrtp_block_t *)ptr - 1;
        h2_pal_mem_api_t owner = block->owner;
        h2_pal_mem_free(&owner, block);
    }
}
