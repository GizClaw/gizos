#ifndef H2_PAL_ATOMIC_H
#define H2_PAL_ATOMIC_H

#include "h2/pal/core/h2_pal_errors.h"
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#if !defined(__cplusplus) && \
    !(defined(_MSC_VER) && (!defined(__STDC_VERSION__) || __STDC_VERSION__ < 201112L))
#include <stdatomic.h>
#define H2_PAL_ATOMIC_STORAGE(type) _Atomic(type)
#else
/* MSVC C translation units without /std:c11 may include the PAL umbrella. */
#define H2_PAL_ATOMIC_STORAGE(type) type
#endif

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief C11 memory orders, mapped to their stdatomic counterparts.
 *
 * Load accepts relaxed, consume, acquire, or sequential consistency; store
 * accepts relaxed, release, or sequential consistency. Compare-exchange
 * failure order must be no stronger than success and cannot be release or
 * acquire-release. Passing an invalid order combination is a caller error.
 */
typedef enum h2_pal_atomic_order {
    H2_PAL_ATOMIC_RELAXED,
    H2_PAL_ATOMIC_CONSUME,
    H2_PAL_ATOMIC_ACQUIRE,
    H2_PAL_ATOMIC_RELEASE,
    H2_PAL_ATOMIC_ACQ_REL,
    H2_PAL_ATOMIC_SEQ_CST,
} h2_pal_atomic_order_t;

/**
 * @brief Caller-owned atomic storage. Initialize before concurrent use.
 *
 * All operations may be called from tasks and ISRs, do not block or allocate,
 * and require a live borrowed API object. Operations on one object are safe
 * across tasks and cores. Objects must be accessed only through this PAL; do
 * not read or write storage directly. C++ clients may pass storage to the C
 * API but must not access the storage member.
 */
typedef struct h2_pal_atomic_u32 {
    H2_PAL_ATOMIC_STORAGE(uint32_t) storage;
} h2_pal_atomic_u32_t;
typedef struct h2_pal_atomic_i32 {
    H2_PAL_ATOMIC_STORAGE(int) storage;
} h2_pal_atomic_i32_t;
typedef struct h2_pal_atomic_bool {
    H2_PAL_ATOMIC_STORAGE(bool) storage;
} h2_pal_atomic_bool_t;
typedef struct h2_pal_atomic_ptr {
    H2_PAL_ATOMIC_STORAGE(void *) storage;
} h2_pal_atomic_ptr_t;
typedef h2_pal_atomic_bool_t h2_pal_atomic_flag_t;

#define H2_PAL_ATOMIC_U32_INIT(value) { (value) }
#define H2_PAL_ATOMIC_I32_INIT(value) { (value) }
#define H2_PAL_ATOMIC_BOOL_INIT(value) { (value) }
#define H2_PAL_ATOMIC_PTR_INIT(value) { (value) }
#define H2_PAL_ATOMIC_FLAG_INIT { false }

/**
 * @brief Atomic operations on caller-owned storage.
 *
 * The API and vtable are borrowed. Loads and fetch/exchange operations write
 * their output only on success. Strong compare-exchange writes out_exchanged
 * on success and replaces expected with the observed value on mismatch.
 * Fetch operations return the value before modification. Integer add/subtract
 * use C11 two's-complement wrapping semantics. Flag test-and-set returns the
 * previous state. The backend performs no allocation or blocking wait.
 */
typedef struct h2_pal_atomic_vtable {
    h2_pal_result_t (*u32_load)(void * user, const h2_pal_atomic_u32_t * value, uint32_t * out_value, h2_pal_atomic_order_t order);
    h2_pal_result_t (*u32_store)(void * user, h2_pal_atomic_u32_t * value, uint32_t desired, h2_pal_atomic_order_t order);
    h2_pal_result_t (*u32_exchange)(void * user, h2_pal_atomic_u32_t * value, uint32_t desired, uint32_t * out_previous, h2_pal_atomic_order_t order);
    h2_pal_result_t (*u32_compare_exchange)(void * user, h2_pal_atomic_u32_t * value, uint32_t * expected, uint32_t desired, bool * out_exchanged, h2_pal_atomic_order_t order, h2_pal_atomic_order_t failure_order);
    h2_pal_result_t (*u32_fetch_add)(void * user, h2_pal_atomic_u32_t * value, uint32_t operand, uint32_t * out_previous, h2_pal_atomic_order_t order);
    h2_pal_result_t (*u32_fetch_sub)(void * user, h2_pal_atomic_u32_t * value, uint32_t operand, uint32_t * out_previous, h2_pal_atomic_order_t order);
    h2_pal_result_t (*u32_fetch_or)(void * user, h2_pal_atomic_u32_t * value, uint32_t operand, uint32_t * out_previous, h2_pal_atomic_order_t order);
    h2_pal_result_t (*u32_fetch_and)(void * user, h2_pal_atomic_u32_t * value, uint32_t operand, uint32_t * out_previous, h2_pal_atomic_order_t order);
    h2_pal_result_t (*i32_load)(void * user, const h2_pal_atomic_i32_t * value, int * out_value, h2_pal_atomic_order_t order);
    h2_pal_result_t (*i32_store)(void * user, h2_pal_atomic_i32_t * value, int desired, h2_pal_atomic_order_t order);
    h2_pal_result_t (*i32_exchange)(void * user, h2_pal_atomic_i32_t * value, int desired, int * out_previous, h2_pal_atomic_order_t order);
    h2_pal_result_t (*i32_compare_exchange)(void * user, h2_pal_atomic_i32_t * value, int * expected, int desired, bool * out_exchanged, h2_pal_atomic_order_t order, h2_pal_atomic_order_t failure_order);
    h2_pal_result_t (*i32_fetch_add)(void * user, h2_pal_atomic_i32_t * value, int operand, int * out_previous, h2_pal_atomic_order_t order);
    h2_pal_result_t (*i32_fetch_sub)(void * user, h2_pal_atomic_i32_t * value, int operand, int * out_previous, h2_pal_atomic_order_t order);
    h2_pal_result_t (*i32_fetch_or)(void * user, h2_pal_atomic_i32_t * value, int operand, int * out_previous, h2_pal_atomic_order_t order);
    h2_pal_result_t (*i32_fetch_and)(void * user, h2_pal_atomic_i32_t * value, int operand, int * out_previous, h2_pal_atomic_order_t order);
    h2_pal_result_t (*bool_load)(void * user, const h2_pal_atomic_bool_t * value, bool * out_value, h2_pal_atomic_order_t order);
    h2_pal_result_t (*bool_store)(void * user, h2_pal_atomic_bool_t * value, bool desired, h2_pal_atomic_order_t order);
    h2_pal_result_t (*bool_exchange)(void * user, h2_pal_atomic_bool_t * value, bool desired, bool * out_previous, h2_pal_atomic_order_t order);
    h2_pal_result_t (*bool_compare_exchange)(void * user, h2_pal_atomic_bool_t * value, bool * expected, bool desired, bool * out_exchanged, h2_pal_atomic_order_t order, h2_pal_atomic_order_t failure_order);
    h2_pal_result_t (*ptr_load)(void * user, const h2_pal_atomic_ptr_t * value, void * * out_value, h2_pal_atomic_order_t order);
    h2_pal_result_t (*ptr_store)(void * user, h2_pal_atomic_ptr_t * value, void * desired, h2_pal_atomic_order_t order);
    h2_pal_result_t (*ptr_exchange)(void * user, h2_pal_atomic_ptr_t * value, void * desired, void * * out_previous, h2_pal_atomic_order_t order);
    h2_pal_result_t (*ptr_compare_exchange)(void * user, h2_pal_atomic_ptr_t * value, void * * expected, void * desired, bool * out_exchanged, h2_pal_atomic_order_t order, h2_pal_atomic_order_t failure_order);
    h2_pal_result_t (*flag_test_and_set)(void * user, h2_pal_atomic_flag_t * value, bool * out_previous, h2_pal_atomic_order_t order);
    h2_pal_result_t (*flag_clear)(void * user, h2_pal_atomic_flag_t * value, h2_pal_atomic_order_t order);
} h2_pal_atomic_vtable_t;

typedef struct h2_pal_atomic_api {
    void *user;
    const h2_pal_atomic_vtable_t *vtable;
} h2_pal_atomic_api_t;

static inline bool h2_pal_atomic_order_valid(h2_pal_atomic_order_t order) {
    return (unsigned)order <= (unsigned)H2_PAL_ATOMIC_SEQ_CST;
}

static inline bool h2_pal_atomic_load_order_valid(h2_pal_atomic_order_t order) {
    return order == H2_PAL_ATOMIC_RELAXED || order == H2_PAL_ATOMIC_CONSUME ||
           order == H2_PAL_ATOMIC_ACQUIRE || order == H2_PAL_ATOMIC_SEQ_CST;
}

static inline bool h2_pal_atomic_store_order_valid(h2_pal_atomic_order_t order) {
    return order == H2_PAL_ATOMIC_RELAXED || order == H2_PAL_ATOMIC_RELEASE ||
           order == H2_PAL_ATOMIC_SEQ_CST;
}

static inline bool h2_pal_atomic_cas_orders_valid(
    h2_pal_atomic_order_t success, h2_pal_atomic_order_t failure) {
    if (!h2_pal_atomic_order_valid(success)) return false;
    if (failure == H2_PAL_ATOMIC_RELAXED) return true;
    if (failure == H2_PAL_ATOMIC_CONSUME) {
        return success == H2_PAL_ATOMIC_CONSUME ||
               success == H2_PAL_ATOMIC_ACQUIRE ||
               success == H2_PAL_ATOMIC_ACQ_REL ||
               success == H2_PAL_ATOMIC_SEQ_CST;
    }
    if (failure == H2_PAL_ATOMIC_ACQUIRE) {
        return success == H2_PAL_ATOMIC_ACQUIRE ||
               success == H2_PAL_ATOMIC_ACQ_REL ||
               success == H2_PAL_ATOMIC_SEQ_CST;
    }
    return failure == H2_PAL_ATOMIC_SEQ_CST && success == H2_PAL_ATOMIC_SEQ_CST;
}

/** @return INVALID_ARG for bad storage, output, or order; UNSUPPORTED for a missing API or slot. */
static inline h2_pal_result_t h2_pal_atomic_u32_load(
    const h2_pal_atomic_api_t *api, const h2_pal_atomic_u32_t * value, uint32_t * out_value, h2_pal_atomic_order_t order) {
    if (out_value != NULL) *out_value = 0;
    if (value == NULL || out_value == NULL || !h2_pal_atomic_load_order_valid(order)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL || api->vtable->u32_load == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->u32_load(api->user, value, out_value, order);
}

static inline h2_pal_result_t h2_pal_atomic_u32_store(
    const h2_pal_atomic_api_t *api, h2_pal_atomic_u32_t * value, uint32_t desired, h2_pal_atomic_order_t order) {
    if (value == NULL || !h2_pal_atomic_store_order_valid(order)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL || api->vtable->u32_store == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->u32_store(api->user, value, desired, order);
}

static inline h2_pal_result_t h2_pal_atomic_u32_exchange(
    const h2_pal_atomic_api_t *api, h2_pal_atomic_u32_t * value, uint32_t desired, uint32_t * out_previous, h2_pal_atomic_order_t order) {
    if (out_previous != NULL) *out_previous = 0;
    if (value == NULL || out_previous == NULL || !h2_pal_atomic_order_valid(order)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL || api->vtable->u32_exchange == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->u32_exchange(api->user, value, desired, out_previous, order);
}

static inline h2_pal_result_t h2_pal_atomic_u32_compare_exchange(
    const h2_pal_atomic_api_t *api, h2_pal_atomic_u32_t * value, uint32_t * expected, uint32_t desired, bool * out_exchanged, h2_pal_atomic_order_t order, h2_pal_atomic_order_t failure_order) {
    if (out_exchanged != NULL) *out_exchanged = false;
    if (value == NULL || expected == NULL || out_exchanged == NULL || !h2_pal_atomic_cas_orders_valid(order, failure_order)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL || api->vtable->u32_compare_exchange == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->u32_compare_exchange(api->user, value, expected, desired, out_exchanged, order, failure_order);
}

static inline h2_pal_result_t h2_pal_atomic_u32_fetch_add(
    const h2_pal_atomic_api_t *api, h2_pal_atomic_u32_t * value, uint32_t operand, uint32_t * out_previous, h2_pal_atomic_order_t order) {
    if (out_previous != NULL) *out_previous = 0;
    if (value == NULL || out_previous == NULL || !h2_pal_atomic_order_valid(order)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL || api->vtable->u32_fetch_add == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->u32_fetch_add(api->user, value, operand, out_previous, order);
}

static inline h2_pal_result_t h2_pal_atomic_u32_fetch_sub(
    const h2_pal_atomic_api_t *api, h2_pal_atomic_u32_t * value, uint32_t operand, uint32_t * out_previous, h2_pal_atomic_order_t order) {
    if (out_previous != NULL) *out_previous = 0;
    if (value == NULL || out_previous == NULL || !h2_pal_atomic_order_valid(order)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL || api->vtable->u32_fetch_sub == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->u32_fetch_sub(api->user, value, operand, out_previous, order);
}

static inline h2_pal_result_t h2_pal_atomic_u32_fetch_or(
    const h2_pal_atomic_api_t *api, h2_pal_atomic_u32_t * value, uint32_t operand, uint32_t * out_previous, h2_pal_atomic_order_t order) {
    if (out_previous != NULL) *out_previous = 0;
    if (value == NULL || out_previous == NULL || !h2_pal_atomic_order_valid(order)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL || api->vtable->u32_fetch_or == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->u32_fetch_or(api->user, value, operand, out_previous, order);
}

static inline h2_pal_result_t h2_pal_atomic_u32_fetch_and(
    const h2_pal_atomic_api_t *api, h2_pal_atomic_u32_t * value, uint32_t operand, uint32_t * out_previous, h2_pal_atomic_order_t order) {
    if (out_previous != NULL) *out_previous = 0;
    if (value == NULL || out_previous == NULL || !h2_pal_atomic_order_valid(order)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL || api->vtable->u32_fetch_and == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->u32_fetch_and(api->user, value, operand, out_previous, order);
}

static inline h2_pal_result_t h2_pal_atomic_i32_load(
    const h2_pal_atomic_api_t *api, const h2_pal_atomic_i32_t * value, int * out_value, h2_pal_atomic_order_t order) {
    if (out_value != NULL) *out_value = 0;
    if (value == NULL || out_value == NULL || !h2_pal_atomic_load_order_valid(order)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL || api->vtable->i32_load == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->i32_load(api->user, value, out_value, order);
}

static inline h2_pal_result_t h2_pal_atomic_i32_store(
    const h2_pal_atomic_api_t *api, h2_pal_atomic_i32_t * value, int desired, h2_pal_atomic_order_t order) {
    if (value == NULL || !h2_pal_atomic_store_order_valid(order)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL || api->vtable->i32_store == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->i32_store(api->user, value, desired, order);
}

static inline h2_pal_result_t h2_pal_atomic_i32_exchange(
    const h2_pal_atomic_api_t *api, h2_pal_atomic_i32_t * value, int desired, int * out_previous, h2_pal_atomic_order_t order) {
    if (out_previous != NULL) *out_previous = 0;
    if (value == NULL || out_previous == NULL || !h2_pal_atomic_order_valid(order)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL || api->vtable->i32_exchange == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->i32_exchange(api->user, value, desired, out_previous, order);
}

static inline h2_pal_result_t h2_pal_atomic_i32_compare_exchange(
    const h2_pal_atomic_api_t *api, h2_pal_atomic_i32_t * value, int * expected, int desired, bool * out_exchanged, h2_pal_atomic_order_t order, h2_pal_atomic_order_t failure_order) {
    if (out_exchanged != NULL) *out_exchanged = false;
    if (value == NULL || expected == NULL || out_exchanged == NULL || !h2_pal_atomic_cas_orders_valid(order, failure_order)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL || api->vtable->i32_compare_exchange == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->i32_compare_exchange(api->user, value, expected, desired, out_exchanged, order, failure_order);
}

static inline h2_pal_result_t h2_pal_atomic_i32_fetch_add(
    const h2_pal_atomic_api_t *api, h2_pal_atomic_i32_t * value, int operand, int * out_previous, h2_pal_atomic_order_t order) {
    if (out_previous != NULL) *out_previous = 0;
    if (value == NULL || out_previous == NULL || !h2_pal_atomic_order_valid(order)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL || api->vtable->i32_fetch_add == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->i32_fetch_add(api->user, value, operand, out_previous, order);
}

static inline h2_pal_result_t h2_pal_atomic_i32_fetch_sub(
    const h2_pal_atomic_api_t *api, h2_pal_atomic_i32_t * value, int operand, int * out_previous, h2_pal_atomic_order_t order) {
    if (out_previous != NULL) *out_previous = 0;
    if (value == NULL || out_previous == NULL || !h2_pal_atomic_order_valid(order)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL || api->vtable->i32_fetch_sub == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->i32_fetch_sub(api->user, value, operand, out_previous, order);
}

static inline h2_pal_result_t h2_pal_atomic_i32_fetch_or(
    const h2_pal_atomic_api_t *api, h2_pal_atomic_i32_t * value, int operand, int * out_previous, h2_pal_atomic_order_t order) {
    if (out_previous != NULL) *out_previous = 0;
    if (value == NULL || out_previous == NULL || !h2_pal_atomic_order_valid(order)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL || api->vtable->i32_fetch_or == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->i32_fetch_or(api->user, value, operand, out_previous, order);
}

static inline h2_pal_result_t h2_pal_atomic_i32_fetch_and(
    const h2_pal_atomic_api_t *api, h2_pal_atomic_i32_t * value, int operand, int * out_previous, h2_pal_atomic_order_t order) {
    if (out_previous != NULL) *out_previous = 0;
    if (value == NULL || out_previous == NULL || !h2_pal_atomic_order_valid(order)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL || api->vtable->i32_fetch_and == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->i32_fetch_and(api->user, value, operand, out_previous, order);
}

static inline h2_pal_result_t h2_pal_atomic_bool_load(
    const h2_pal_atomic_api_t *api, const h2_pal_atomic_bool_t * value, bool * out_value, h2_pal_atomic_order_t order) {
    if (out_value != NULL) *out_value = 0;
    if (value == NULL || out_value == NULL || !h2_pal_atomic_load_order_valid(order)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL || api->vtable->bool_load == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->bool_load(api->user, value, out_value, order);
}

static inline h2_pal_result_t h2_pal_atomic_bool_store(
    const h2_pal_atomic_api_t *api, h2_pal_atomic_bool_t * value, bool desired, h2_pal_atomic_order_t order) {
    if (value == NULL || !h2_pal_atomic_store_order_valid(order)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL || api->vtable->bool_store == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->bool_store(api->user, value, desired, order);
}

static inline h2_pal_result_t h2_pal_atomic_bool_exchange(
    const h2_pal_atomic_api_t *api, h2_pal_atomic_bool_t * value, bool desired, bool * out_previous, h2_pal_atomic_order_t order) {
    if (out_previous != NULL) *out_previous = 0;
    if (value == NULL || out_previous == NULL || !h2_pal_atomic_order_valid(order)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL || api->vtable->bool_exchange == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->bool_exchange(api->user, value, desired, out_previous, order);
}

static inline h2_pal_result_t h2_pal_atomic_bool_compare_exchange(
    const h2_pal_atomic_api_t *api, h2_pal_atomic_bool_t * value, bool * expected, bool desired, bool * out_exchanged, h2_pal_atomic_order_t order, h2_pal_atomic_order_t failure_order) {
    if (out_exchanged != NULL) *out_exchanged = false;
    if (value == NULL || expected == NULL || out_exchanged == NULL || !h2_pal_atomic_cas_orders_valid(order, failure_order)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL || api->vtable->bool_compare_exchange == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->bool_compare_exchange(api->user, value, expected, desired, out_exchanged, order, failure_order);
}

static inline h2_pal_result_t h2_pal_atomic_ptr_load(
    const h2_pal_atomic_api_t *api, const h2_pal_atomic_ptr_t * value, void * * out_value, h2_pal_atomic_order_t order) {
    if (out_value != NULL) *out_value = 0;
    if (value == NULL || out_value == NULL || !h2_pal_atomic_load_order_valid(order)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL || api->vtable->ptr_load == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->ptr_load(api->user, value, out_value, order);
}

static inline h2_pal_result_t h2_pal_atomic_ptr_store(
    const h2_pal_atomic_api_t *api, h2_pal_atomic_ptr_t * value, void * desired, h2_pal_atomic_order_t order) {
    if (value == NULL || !h2_pal_atomic_store_order_valid(order)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL || api->vtable->ptr_store == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->ptr_store(api->user, value, desired, order);
}

static inline h2_pal_result_t h2_pal_atomic_ptr_exchange(
    const h2_pal_atomic_api_t *api, h2_pal_atomic_ptr_t * value, void * desired, void * * out_previous, h2_pal_atomic_order_t order) {
    if (out_previous != NULL) *out_previous = 0;
    if (value == NULL || out_previous == NULL || !h2_pal_atomic_order_valid(order)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL || api->vtable->ptr_exchange == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->ptr_exchange(api->user, value, desired, out_previous, order);
}

static inline h2_pal_result_t h2_pal_atomic_ptr_compare_exchange(
    const h2_pal_atomic_api_t *api, h2_pal_atomic_ptr_t * value, void * * expected, void * desired, bool * out_exchanged, h2_pal_atomic_order_t order, h2_pal_atomic_order_t failure_order) {
    if (out_exchanged != NULL) *out_exchanged = false;
    if (value == NULL || expected == NULL || out_exchanged == NULL || !h2_pal_atomic_cas_orders_valid(order, failure_order)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL || api->vtable->ptr_compare_exchange == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->ptr_compare_exchange(api->user, value, expected, desired, out_exchanged, order, failure_order);
}

static inline h2_pal_result_t h2_pal_atomic_flag_test_and_set(
    const h2_pal_atomic_api_t *api, h2_pal_atomic_flag_t * value, bool * out_previous, h2_pal_atomic_order_t order) {
    if (out_previous != NULL) *out_previous = 0;
    if (value == NULL || out_previous == NULL || !h2_pal_atomic_order_valid(order)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL || api->vtable->flag_test_and_set == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->flag_test_and_set(api->user, value, out_previous, order);
}

static inline h2_pal_result_t h2_pal_atomic_flag_clear(
    const h2_pal_atomic_api_t *api, h2_pal_atomic_flag_t * value, h2_pal_atomic_order_t order) {
    if (value == NULL || !h2_pal_atomic_store_order_valid(order)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL || api->vtable->flag_clear == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->flag_clear(api->user, value, order);
}

#ifdef __cplusplus
}
#endif

#undef H2_PAL_ATOMIC_STORAGE
#endif
