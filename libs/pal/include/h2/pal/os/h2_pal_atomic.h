#ifndef H2_PAL_ATOMIC_H
#define H2_PAL_ATOMIC_H

#include "h2/pal/core/h2_pal_errors.h"
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Allocation policy for C11 atomics. The borrowed API and vtable remain live.
 * Alloc returns storage aligned to `alignment` and at least `size` bytes.
 * Implementations must place it in memory where native C11 operations are
 * atomic across tasks and cores. A successful alloc must satisfy the requested
 * alignment. Only the five typed helpers below are supported PAL allocation
 * entry points; providers may return UNSUPPORTED for other sizes/alignments.
 * The caller owns each successful allocation. */
typedef struct h2_pal_atomic_vtable {
    h2_pal_result_t (*alloc)(void *user, size_t size, size_t alignment, void **out);
    void (*free)(void *user, void *value);
} h2_pal_atomic_vtable_t;

typedef struct h2_pal_atomic_api {
    void *user;
    const h2_pal_atomic_vtable_t *vtable;
} h2_pal_atomic_api_t;

/** Free a PAL allocation after all concurrent access has stopped. NULL is
 * accepted. Allocation and free are not ISR-safe unless a provider says so. */
static inline h2_pal_result_t h2_pal_atomic_free(const h2_pal_atomic_api_t *api,
                                                  void *value) {
    if (api == NULL || api->vtable == NULL || api->vtable->free == NULL)
        return H2_PAL_ERR_UNSUPPORTED;
    api->vtable->free(api->user, value);
    return H2_PAL_OK;
}

#ifdef __cplusplus
}
/* C++ may include the PAL umbrella, but these C11 _Atomic allocation helpers
 * are C-only. C++ callers should use a C translation unit for this API. */
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#include <stdatomic.h>
#include <string.h>

/** Internal helper for the five typed allocations below. Other alignments
 * may be rejected; a successful result always has the requested alignment. */
static inline h2_pal_result_t h2_pal_atomic_alloc_raw(
    const h2_pal_atomic_api_t *api, size_t size, size_t alignment, void **out) {
    if (out == NULL) return H2_PAL_ERR_INVALID_ARG;
    *out = NULL;
    if (api == NULL || api->vtable == NULL || api->vtable->alloc == NULL)
        return H2_PAL_ERR_UNSUPPORTED;
    if (size == 0 || alignment == 0 || (alignment & (alignment - 1)) != 0)
        return H2_PAL_ERR_INVALID_ARG;
    return api->vtable->alloc(api->user, size, alignment, out);
}
/** Allocate initialized, cross-core C11 atomic storage. Use standard C11
 * load/store/exchange/compare_exchange/fetch operations directly on returned
 * values, including in ISRs when the operation itself is lock-free. On
 * ESP32-S3, static/global atomics in internal DRAM are usable directly;
 * atomics shared by tasks must not reside in heap objects or task stacks that
 * may be in PSRAM. Never free during access. No 64-bit helper is provided. */
#define H2_PAL_ATOMIC_ALLOC_TYPED(name, type) \
static inline h2_pal_result_t h2_pal_atomic_alloc_##name( \
    const h2_pal_atomic_api_t *api, type initial, _Atomic(type) **out) { \
    if (out == NULL) return H2_PAL_ERR_INVALID_ARG; \
    *out = NULL; \
    void *storage = NULL; \
    h2_pal_result_t result = h2_pal_atomic_alloc_raw(api, sizeof(_Atomic(type)), \
        _Alignof(_Atomic(type)), &storage); \
    if (result != H2_PAL_OK) return result; \
    if (storage == NULL) return H2_PAL_ERR_NO_MEMORY; \
    *out = (_Atomic(type) *)storage; \
    atomic_init(*out, initial); \
    return H2_PAL_OK; \
}
H2_PAL_ATOMIC_ALLOC_TYPED(u32, uint32_t)
H2_PAL_ATOMIC_ALLOC_TYPED(i32, int32_t)
H2_PAL_ATOMIC_ALLOC_TYPED(bool, bool)
H2_PAL_ATOMIC_ALLOC_TYPED(ptr, void *)
#undef H2_PAL_ATOMIC_ALLOC_TYPED

/** Allocate an atomic_flag initialized to its clear state. */
static inline h2_pal_result_t h2_pal_atomic_alloc_flag(
    const h2_pal_atomic_api_t *api, atomic_flag **out) {
    if (out == NULL) return H2_PAL_ERR_INVALID_ARG;
    *out = NULL;
    void *storage = NULL;
    h2_pal_result_t result = h2_pal_atomic_alloc_raw(api, sizeof(atomic_flag),
        _Alignof(atomic_flag), &storage);
    if (result != H2_PAL_OK) return result;
    if (storage == NULL) return H2_PAL_ERR_NO_MEMORY;
    *out = (atomic_flag *)storage;
    atomic_flag clear = ATOMIC_FLAG_INIT;
    memcpy(*out, &clear, sizeof(clear));
    return H2_PAL_OK;
}
#endif

#endif
