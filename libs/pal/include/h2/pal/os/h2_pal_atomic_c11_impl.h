#ifndef H2_C11_PAL_ATOMIC_IMPL_H
#define H2_C11_PAL_ATOMIC_IMPL_H

#include "h2/pal/os/h2_pal_atomic.h"
#include <stdatomic.h>

_Static_assert(sizeof(int) == sizeof(uint32_t), "i32 atomic requires 32-bit int");

#ifndef H2_C11_ATOMIC_ATTR
#define H2_C11_ATOMIC_ATTR
#endif

static H2_C11_ATOMIC_ATTR memory_order h2_c11_atomic_order(h2_pal_atomic_order_t order) {
    if (order == H2_PAL_ATOMIC_RELAXED) return memory_order_relaxed;
    if (order == H2_PAL_ATOMIC_CONSUME) return memory_order_consume;
    if (order == H2_PAL_ATOMIC_ACQUIRE) return memory_order_acquire;
    if (order == H2_PAL_ATOMIC_RELEASE) return memory_order_release;
    if (order == H2_PAL_ATOMIC_ACQ_REL) return memory_order_acq_rel;
    return memory_order_seq_cst;
}

static H2_C11_ATOMIC_ATTR h2_pal_result_t h2_c11_u32_load(void * user, const h2_pal_atomic_u32_t * value, uint32_t * out_value, h2_pal_atomic_order_t order) {
    (void)user;
    /* Older embedded Clang requires a mutable _Atomic pointer for a read. */
    *out_value = atomic_load_explicit((_Atomic(uint32_t) *)&value->storage, h2_c11_atomic_order(order));
    return H2_PAL_OK;
}

static H2_C11_ATOMIC_ATTR h2_pal_result_t h2_c11_u32_store(void * user, h2_pal_atomic_u32_t * value, uint32_t desired, h2_pal_atomic_order_t order) {
    (void)user;
    atomic_store_explicit(&value->storage, desired, h2_c11_atomic_order(order));
    return H2_PAL_OK;
}

static H2_C11_ATOMIC_ATTR h2_pal_result_t h2_c11_u32_exchange(void * user, h2_pal_atomic_u32_t * value, uint32_t desired, uint32_t * out_previous, h2_pal_atomic_order_t order) {
    (void)user;
    *out_previous = atomic_exchange_explicit(&value->storage, desired, h2_c11_atomic_order(order));
    return H2_PAL_OK;
}

static H2_C11_ATOMIC_ATTR h2_pal_result_t h2_c11_u32_compare_exchange(void * user, h2_pal_atomic_u32_t * value, uint32_t * expected, uint32_t desired, bool * out_exchanged, h2_pal_atomic_order_t order, h2_pal_atomic_order_t failure_order) {
    (void)user;
    *out_exchanged = atomic_compare_exchange_strong_explicit(&value->storage, expected, desired, h2_c11_atomic_order(order), h2_c11_atomic_order(failure_order));
    return H2_PAL_OK;
}

static H2_C11_ATOMIC_ATTR h2_pal_result_t h2_c11_u32_fetch_add(void * user, h2_pal_atomic_u32_t * value, uint32_t operand, uint32_t * out_previous, h2_pal_atomic_order_t order) {
    (void)user;
    *out_previous = atomic_fetch_add_explicit(&value->storage, operand, h2_c11_atomic_order(order));
    return H2_PAL_OK;
}

static H2_C11_ATOMIC_ATTR h2_pal_result_t h2_c11_u32_fetch_sub(void * user, h2_pal_atomic_u32_t * value, uint32_t operand, uint32_t * out_previous, h2_pal_atomic_order_t order) {
    (void)user;
    *out_previous = atomic_fetch_sub_explicit(&value->storage, operand, h2_c11_atomic_order(order));
    return H2_PAL_OK;
}

static H2_C11_ATOMIC_ATTR h2_pal_result_t h2_c11_u32_fetch_or(void * user, h2_pal_atomic_u32_t * value, uint32_t operand, uint32_t * out_previous, h2_pal_atomic_order_t order) {
    (void)user;
    *out_previous = atomic_fetch_or_explicit(&value->storage, operand, h2_c11_atomic_order(order));
    return H2_PAL_OK;
}

static H2_C11_ATOMIC_ATTR h2_pal_result_t h2_c11_u32_fetch_and(void * user, h2_pal_atomic_u32_t * value, uint32_t operand, uint32_t * out_previous, h2_pal_atomic_order_t order) {
    (void)user;
    *out_previous = atomic_fetch_and_explicit(&value->storage, operand, h2_c11_atomic_order(order));
    return H2_PAL_OK;
}

static H2_C11_ATOMIC_ATTR h2_pal_result_t h2_c11_i32_load(void * user, const h2_pal_atomic_i32_t * value, int * out_value, h2_pal_atomic_order_t order) {
    (void)user;
    *out_value = atomic_load_explicit((_Atomic(int) *)&value->storage, h2_c11_atomic_order(order));
    return H2_PAL_OK;
}

static H2_C11_ATOMIC_ATTR h2_pal_result_t h2_c11_i32_store(void * user, h2_pal_atomic_i32_t * value, int desired, h2_pal_atomic_order_t order) {
    (void)user;
    atomic_store_explicit(&value->storage, desired, h2_c11_atomic_order(order));
    return H2_PAL_OK;
}

static H2_C11_ATOMIC_ATTR h2_pal_result_t h2_c11_i32_exchange(void * user, h2_pal_atomic_i32_t * value, int desired, int * out_previous, h2_pal_atomic_order_t order) {
    (void)user;
    *out_previous = atomic_exchange_explicit(&value->storage, desired, h2_c11_atomic_order(order));
    return H2_PAL_OK;
}

static H2_C11_ATOMIC_ATTR h2_pal_result_t h2_c11_i32_compare_exchange(void * user, h2_pal_atomic_i32_t * value, int * expected, int desired, bool * out_exchanged, h2_pal_atomic_order_t order, h2_pal_atomic_order_t failure_order) {
    (void)user;
    *out_exchanged = atomic_compare_exchange_strong_explicit(&value->storage, expected, desired, h2_c11_atomic_order(order), h2_c11_atomic_order(failure_order));
    return H2_PAL_OK;
}

static H2_C11_ATOMIC_ATTR h2_pal_result_t h2_c11_i32_fetch_add(void * user, h2_pal_atomic_i32_t * value, int operand, int * out_previous, h2_pal_atomic_order_t order) {
    (void)user;
    *out_previous = atomic_fetch_add_explicit(&value->storage, operand, h2_c11_atomic_order(order));
    return H2_PAL_OK;
}

static H2_C11_ATOMIC_ATTR h2_pal_result_t h2_c11_i32_fetch_sub(void * user, h2_pal_atomic_i32_t * value, int operand, int * out_previous, h2_pal_atomic_order_t order) {
    (void)user;
    *out_previous = atomic_fetch_sub_explicit(&value->storage, operand, h2_c11_atomic_order(order));
    return H2_PAL_OK;
}

static H2_C11_ATOMIC_ATTR h2_pal_result_t h2_c11_i32_fetch_or(void * user, h2_pal_atomic_i32_t * value, int operand, int * out_previous, h2_pal_atomic_order_t order) {
    (void)user;
    *out_previous = atomic_fetch_or_explicit(&value->storage, operand, h2_c11_atomic_order(order));
    return H2_PAL_OK;
}

static H2_C11_ATOMIC_ATTR h2_pal_result_t h2_c11_i32_fetch_and(void * user, h2_pal_atomic_i32_t * value, int operand, int * out_previous, h2_pal_atomic_order_t order) {
    (void)user;
    *out_previous = atomic_fetch_and_explicit(&value->storage, operand, h2_c11_atomic_order(order));
    return H2_PAL_OK;
}

static H2_C11_ATOMIC_ATTR h2_pal_result_t h2_c11_bool_load(void * user, const h2_pal_atomic_bool_t * value, bool * out_value, h2_pal_atomic_order_t order) {
    (void)user;
    *out_value = atomic_load_explicit((_Atomic(bool) *)&value->storage, h2_c11_atomic_order(order));
    return H2_PAL_OK;
}

static H2_C11_ATOMIC_ATTR h2_pal_result_t h2_c11_bool_store(void * user, h2_pal_atomic_bool_t * value, bool desired, h2_pal_atomic_order_t order) {
    (void)user;
    atomic_store_explicit(&value->storage, desired, h2_c11_atomic_order(order));
    return H2_PAL_OK;
}

static H2_C11_ATOMIC_ATTR h2_pal_result_t h2_c11_bool_exchange(void * user, h2_pal_atomic_bool_t * value, bool desired, bool * out_previous, h2_pal_atomic_order_t order) {
    (void)user;
    *out_previous = atomic_exchange_explicit(&value->storage, desired, h2_c11_atomic_order(order));
    return H2_PAL_OK;
}

static H2_C11_ATOMIC_ATTR h2_pal_result_t h2_c11_bool_compare_exchange(void * user, h2_pal_atomic_bool_t * value, bool * expected, bool desired, bool * out_exchanged, h2_pal_atomic_order_t order, h2_pal_atomic_order_t failure_order) {
    (void)user;
    *out_exchanged = atomic_compare_exchange_strong_explicit(&value->storage, expected, desired, h2_c11_atomic_order(order), h2_c11_atomic_order(failure_order));
    return H2_PAL_OK;
}

static H2_C11_ATOMIC_ATTR h2_pal_result_t h2_c11_ptr_load(void * user, const h2_pal_atomic_ptr_t * value, void * * out_value, h2_pal_atomic_order_t order) {
    (void)user;
    *out_value = atomic_load_explicit((_Atomic(void *) *)&value->storage, h2_c11_atomic_order(order));
    return H2_PAL_OK;
}

static H2_C11_ATOMIC_ATTR h2_pal_result_t h2_c11_ptr_store(void * user, h2_pal_atomic_ptr_t * value, void * desired, h2_pal_atomic_order_t order) {
    (void)user;
    atomic_store_explicit(&value->storage, desired, h2_c11_atomic_order(order));
    return H2_PAL_OK;
}

static H2_C11_ATOMIC_ATTR h2_pal_result_t h2_c11_ptr_exchange(void * user, h2_pal_atomic_ptr_t * value, void * desired, void * * out_previous, h2_pal_atomic_order_t order) {
    (void)user;
    *out_previous = atomic_exchange_explicit(&value->storage, desired, h2_c11_atomic_order(order));
    return H2_PAL_OK;
}

static H2_C11_ATOMIC_ATTR h2_pal_result_t h2_c11_ptr_compare_exchange(void * user, h2_pal_atomic_ptr_t * value, void * * expected, void * desired, bool * out_exchanged, h2_pal_atomic_order_t order, h2_pal_atomic_order_t failure_order) {
    (void)user;
    *out_exchanged = atomic_compare_exchange_strong_explicit(&value->storage, expected, desired, h2_c11_atomic_order(order), h2_c11_atomic_order(failure_order));
    return H2_PAL_OK;
}

static H2_C11_ATOMIC_ATTR h2_pal_result_t h2_c11_flag_test_and_set(void * user, h2_pal_atomic_flag_t * value, bool * out_previous, h2_pal_atomic_order_t order) {
    (void)user;
    *out_previous = atomic_exchange_explicit(&value->storage, true, h2_c11_atomic_order(order));
    return H2_PAL_OK;
}

static H2_C11_ATOMIC_ATTR h2_pal_result_t h2_c11_flag_clear(void * user, h2_pal_atomic_flag_t * value, h2_pal_atomic_order_t order) {
    (void)user;
    atomic_store_explicit(&value->storage, false, h2_c11_atomic_order(order));
    return H2_PAL_OK;
}

#define H2_C11_PAL_ATOMIC_VTABLE_INIT { \
    .u32_load = h2_c11_u32_load, \
    .u32_store = h2_c11_u32_store, \
    .u32_exchange = h2_c11_u32_exchange, \
    .u32_compare_exchange = h2_c11_u32_compare_exchange, \
    .u32_fetch_add = h2_c11_u32_fetch_add, \
    .u32_fetch_sub = h2_c11_u32_fetch_sub, \
    .u32_fetch_or = h2_c11_u32_fetch_or, \
    .u32_fetch_and = h2_c11_u32_fetch_and, \
    .i32_load = h2_c11_i32_load, \
    .i32_store = h2_c11_i32_store, \
    .i32_exchange = h2_c11_i32_exchange, \
    .i32_compare_exchange = h2_c11_i32_compare_exchange, \
    .i32_fetch_add = h2_c11_i32_fetch_add, \
    .i32_fetch_sub = h2_c11_i32_fetch_sub, \
    .i32_fetch_or = h2_c11_i32_fetch_or, \
    .i32_fetch_and = h2_c11_i32_fetch_and, \
    .bool_load = h2_c11_bool_load, \
    .bool_store = h2_c11_bool_store, \
    .bool_exchange = h2_c11_bool_exchange, \
    .bool_compare_exchange = h2_c11_bool_compare_exchange, \
    .ptr_load = h2_c11_ptr_load, \
    .ptr_store = h2_c11_ptr_store, \
    .ptr_exchange = h2_c11_ptr_exchange, \
    .ptr_compare_exchange = h2_c11_ptr_compare_exchange, \
    .flag_test_and_set = h2_c11_flag_test_and_set, \
    .flag_clear = h2_c11_flag_clear, \
}

#endif
