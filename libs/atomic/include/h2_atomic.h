#ifndef H2_ATOMIC_H
#define H2_ATOMIC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Value wrappers may live anywhere. Non-flag storage is allocated by the linked
 * platform implementation. Initialize once, do not copy after initialization,
 * and destroy only after all concurrent users have stopped. Flags support
 * static zero initialization and never allocate storage. */
typedef struct h2_atomic_int_storage h2_atomic_int_storage_t;
typedef struct h2_atomic_uint_storage h2_atomic_uint_storage_t;
typedef struct h2_atomic_u8_storage h2_atomic_u8_storage_t;
typedef struct h2_atomic_u16_storage h2_atomic_u16_storage_t;
typedef struct h2_atomic_u32_storage h2_atomic_u32_storage_t;
typedef struct h2_atomic_bool_storage h2_atomic_bool_storage_t;
typedef struct h2_atomic_size_storage h2_atomic_size_storage_t;
typedef struct h2_atomic_ptr_storage h2_atomic_ptr_storage_t;

typedef struct { h2_atomic_int_storage_t *storage; } h2_atomic_int_t;
typedef struct { h2_atomic_uint_storage_t *storage; } h2_atomic_uint_t;
typedef struct { h2_atomic_u8_storage_t *storage; } h2_atomic_u8_t;
typedef struct { h2_atomic_u16_storage_t *storage; } h2_atomic_u16_t;
typedef struct { h2_atomic_u32_storage_t *storage; } h2_atomic_u32_t;
typedef struct { h2_atomic_bool_storage_t *storage; } h2_atomic_bool_t;
typedef struct { h2_atomic_size_storage_t *storage; } h2_atomic_size_t;
typedef struct { h2_atomic_ptr_storage_t *storage; } h2_atomic_ptr_t;
/* Zero initialization is valid for flags, including process-wide static flags.
 * Platform providers guard this byte; callers must never access it directly. */
typedef struct { uint8_t _state; } h2_atomic_flag_t;

typedef enum h2_atomic_result {
    H2_ATOMIC_OK = 0,
    H2_ATOMIC_INVALID_ARG,
    H2_ATOMIC_INVALID_STATE,
    H2_ATOMIC_NO_MEMORY,
    /* The linked platform provider has no atomic implementation. */
    H2_ATOMIC_UNSUPPORTED,
} h2_atomic_result_t;

typedef enum h2_atomic_order {
    H2_ATOMIC_RELAXED,
    H2_ATOMIC_ACQUIRE,
    H2_ATOMIC_RELEASE,
    H2_ATOMIC_ACQ_REL,
    H2_ATOMIC_SEQ_CST,
} h2_atomic_order_t;

/* No implementation or fallback is supplied by this target. A final binary
 * must link one implementation of these symbols for its platform. Callers
 * must check init before using a value; unsupported providers trap on use. */
#define H2_ATOMIC_DECLARE_INTEGER(name, type) \
    h2_atomic_result_t h2_atomic_##name##_init(h2_atomic_##name##_t *value, type initial); \
    void h2_atomic_##name##_destroy(h2_atomic_##name##_t *value); \
    type h2_atomic_##name##_load(const h2_atomic_##name##_t *value, h2_atomic_order_t order); \
    void h2_atomic_##name##_store(h2_atomic_##name##_t *value, type next, h2_atomic_order_t order); \
    type h2_atomic_##name##_exchange(h2_atomic_##name##_t *value, type next, h2_atomic_order_t order); \
    bool h2_atomic_##name##_compare_exchange(h2_atomic_##name##_t *value, type *expected, type desired, h2_atomic_order_t success, h2_atomic_order_t failure); \
    type h2_atomic_##name##_fetch_add(h2_atomic_##name##_t *value, type amount, h2_atomic_order_t order); \
    type h2_atomic_##name##_fetch_sub(h2_atomic_##name##_t *value, type amount, h2_atomic_order_t order); \
    type h2_atomic_##name##_fetch_or(h2_atomic_##name##_t *value, type mask, h2_atomic_order_t order); \
    type h2_atomic_##name##_fetch_and(h2_atomic_##name##_t *value, type mask, h2_atomic_order_t order)

H2_ATOMIC_DECLARE_INTEGER(int, int);
H2_ATOMIC_DECLARE_INTEGER(uint, unsigned int);
H2_ATOMIC_DECLARE_INTEGER(u8, uint8_t);
H2_ATOMIC_DECLARE_INTEGER(u16, uint16_t);
H2_ATOMIC_DECLARE_INTEGER(u32, uint32_t);
H2_ATOMIC_DECLARE_INTEGER(size, size_t);
#undef H2_ATOMIC_DECLARE_INTEGER

h2_atomic_result_t h2_atomic_bool_init(h2_atomic_bool_t *value, bool initial);
void h2_atomic_bool_destroy(h2_atomic_bool_t *value);
bool h2_atomic_bool_load(const h2_atomic_bool_t *value, h2_atomic_order_t order);
void h2_atomic_bool_store(h2_atomic_bool_t *value, bool next, h2_atomic_order_t order);
bool h2_atomic_bool_exchange(h2_atomic_bool_t *value, bool next, h2_atomic_order_t order);
bool h2_atomic_bool_compare_exchange(h2_atomic_bool_t *value, bool *expected,
                                     bool desired, h2_atomic_order_t success,
                                     h2_atomic_order_t failure);

h2_atomic_result_t h2_atomic_ptr_init(h2_atomic_ptr_t *value, void *initial);
void h2_atomic_ptr_destroy(h2_atomic_ptr_t *value);
void *h2_atomic_ptr_load(const h2_atomic_ptr_t *value, h2_atomic_order_t order);
void h2_atomic_ptr_store(h2_atomic_ptr_t *value, void *next, h2_atomic_order_t order);
void *h2_atomic_ptr_exchange(h2_atomic_ptr_t *value, void *next, h2_atomic_order_t order);
bool h2_atomic_ptr_compare_exchange(h2_atomic_ptr_t *value, void **expected,
                                    void *desired, h2_atomic_order_t success,
                                    h2_atomic_order_t failure);

h2_atomic_result_t h2_atomic_flag_init(h2_atomic_flag_t *value);
void h2_atomic_flag_destroy(h2_atomic_flag_t *value);
bool h2_atomic_flag_test_and_set(h2_atomic_flag_t *value, h2_atomic_order_t order);
void h2_atomic_flag_clear(h2_atomic_flag_t *value, h2_atomic_order_t order);

#ifdef __cplusplus
}
#endif

/* C convenience dispatch: these macros select typed, linkable functions. */
#ifndef __cplusplus
#define h2_atomic_init_dispatch(value) _Generic((value), \
    h2_atomic_int_t *: h2_atomic_int_init, \
    h2_atomic_uint_t *: h2_atomic_uint_init, \
    h2_atomic_u8_t *: h2_atomic_u8_init, \
    h2_atomic_u16_t *: h2_atomic_u16_init, \
    h2_atomic_u32_t *: h2_atomic_u32_init, \
    h2_atomic_size_t *: h2_atomic_size_init, \
    h2_atomic_bool_t *: h2_atomic_bool_init, \
    h2_atomic_ptr_t *: h2_atomic_ptr_init \
)
#define h2_atomic_init_explicit(value, initial) h2_atomic_init_dispatch(value)(value, initial)
#define h2_atomic_init(value, initial) h2_atomic_init_explicit(value, initial)
#define h2_atomic_destroy_dispatch(value) _Generic((value), \
    h2_atomic_int_t *: h2_atomic_int_destroy, \
    h2_atomic_uint_t *: h2_atomic_uint_destroy, \
    h2_atomic_u8_t *: h2_atomic_u8_destroy, \
    h2_atomic_u16_t *: h2_atomic_u16_destroy, \
    h2_atomic_u32_t *: h2_atomic_u32_destroy, \
    h2_atomic_size_t *: h2_atomic_size_destroy, \
    h2_atomic_bool_t *: h2_atomic_bool_destroy, \
    h2_atomic_ptr_t *: h2_atomic_ptr_destroy, \
    h2_atomic_flag_t *: h2_atomic_flag_destroy \
)
#define h2_atomic_destroy_explicit(value) h2_atomic_destroy_dispatch(value)(value)
#define h2_atomic_destroy(value) h2_atomic_destroy_explicit(value)
#define h2_atomic_load_dispatch(value) _Generic((value), \
    h2_atomic_int_t *: h2_atomic_int_load, \
    const h2_atomic_int_t *: h2_atomic_int_load, \
    h2_atomic_uint_t *: h2_atomic_uint_load, \
    const h2_atomic_uint_t *: h2_atomic_uint_load, \
    h2_atomic_u8_t *: h2_atomic_u8_load, \
    const h2_atomic_u8_t *: h2_atomic_u8_load, \
    h2_atomic_u16_t *: h2_atomic_u16_load, \
    const h2_atomic_u16_t *: h2_atomic_u16_load, \
    h2_atomic_u32_t *: h2_atomic_u32_load, \
    const h2_atomic_u32_t *: h2_atomic_u32_load, \
    h2_atomic_size_t *: h2_atomic_size_load, \
    const h2_atomic_size_t *: h2_atomic_size_load, \
    h2_atomic_bool_t *: h2_atomic_bool_load, \
    const h2_atomic_bool_t *: h2_atomic_bool_load, \
    h2_atomic_ptr_t *: h2_atomic_ptr_load, \
    const h2_atomic_ptr_t *: h2_atomic_ptr_load \
)
#define h2_atomic_load_explicit(value, order) h2_atomic_load_dispatch(value)(value, order)
#define h2_atomic_load(value) h2_atomic_load_explicit(value, H2_ATOMIC_SEQ_CST)
#define h2_atomic_store_dispatch(value) _Generic((value), \
    h2_atomic_int_t *: h2_atomic_int_store, \
    h2_atomic_uint_t *: h2_atomic_uint_store, \
    h2_atomic_u8_t *: h2_atomic_u8_store, \
    h2_atomic_u16_t *: h2_atomic_u16_store, \
    h2_atomic_u32_t *: h2_atomic_u32_store, \
    h2_atomic_size_t *: h2_atomic_size_store, \
    h2_atomic_bool_t *: h2_atomic_bool_store, \
    h2_atomic_ptr_t *: h2_atomic_ptr_store \
)
#define h2_atomic_store_explicit(value, next, order) h2_atomic_store_dispatch(value)(value, next, order)
#define h2_atomic_store(value, next) h2_atomic_store_explicit(value, next, H2_ATOMIC_SEQ_CST)
#define h2_atomic_exchange_dispatch(value) _Generic((value), \
    h2_atomic_int_t *: h2_atomic_int_exchange, \
    h2_atomic_uint_t *: h2_atomic_uint_exchange, \
    h2_atomic_u8_t *: h2_atomic_u8_exchange, \
    h2_atomic_u16_t *: h2_atomic_u16_exchange, \
    h2_atomic_u32_t *: h2_atomic_u32_exchange, \
    h2_atomic_size_t *: h2_atomic_size_exchange, \
    h2_atomic_bool_t *: h2_atomic_bool_exchange, \
    h2_atomic_ptr_t *: h2_atomic_ptr_exchange \
)
#define h2_atomic_exchange_explicit(value, next, order) h2_atomic_exchange_dispatch(value)(value, next, order)
#define h2_atomic_exchange(value, next) h2_atomic_exchange_explicit(value, next, H2_ATOMIC_SEQ_CST)
#define h2_atomic_compare_exchange_dispatch(value) _Generic((value), \
    h2_atomic_int_t *: h2_atomic_int_compare_exchange, \
    h2_atomic_uint_t *: h2_atomic_uint_compare_exchange, \
    h2_atomic_u8_t *: h2_atomic_u8_compare_exchange, \
    h2_atomic_u16_t *: h2_atomic_u16_compare_exchange, \
    h2_atomic_u32_t *: h2_atomic_u32_compare_exchange, \
    h2_atomic_size_t *: h2_atomic_size_compare_exchange, \
    h2_atomic_bool_t *: h2_atomic_bool_compare_exchange, \
    h2_atomic_ptr_t *: h2_atomic_ptr_compare_exchange \
)
#define h2_atomic_compare_exchange_explicit(value, expected, desired, success, failure) h2_atomic_compare_exchange_dispatch(value)(value, expected, desired, success, failure)
#define h2_atomic_compare_exchange_strong(value, expected, desired) h2_atomic_compare_exchange_strong_explicit(value, expected, desired, H2_ATOMIC_SEQ_CST, H2_ATOMIC_SEQ_CST)
#define h2_atomic_compare_exchange_strong_explicit(value, expected, desired, success, failure) h2_atomic_compare_exchange_explicit(value, expected, desired, success, failure)
#define h2_atomic_compare_exchange_weak_explicit(value, expected, desired, success, failure) h2_atomic_compare_exchange_explicit(value, expected, desired, success, failure)
#define h2_atomic_fetch_add_dispatch(value) _Generic((value), \
    h2_atomic_int_t *: h2_atomic_int_fetch_add, \
    h2_atomic_uint_t *: h2_atomic_uint_fetch_add, \
    h2_atomic_u8_t *: h2_atomic_u8_fetch_add, \
    h2_atomic_u16_t *: h2_atomic_u16_fetch_add, \
    h2_atomic_u32_t *: h2_atomic_u32_fetch_add, \
    h2_atomic_size_t *: h2_atomic_size_fetch_add \
)
#define h2_atomic_fetch_add_explicit(value, amount, order) h2_atomic_fetch_add_dispatch(value)(value, amount, order)
#define h2_atomic_fetch_add(value, amount) h2_atomic_fetch_add_explicit(value, amount, H2_ATOMIC_SEQ_CST)
#define h2_atomic_fetch_sub_dispatch(value) _Generic((value), \
    h2_atomic_int_t *: h2_atomic_int_fetch_sub, \
    h2_atomic_uint_t *: h2_atomic_uint_fetch_sub, \
    h2_atomic_u8_t *: h2_atomic_u8_fetch_sub, \
    h2_atomic_u16_t *: h2_atomic_u16_fetch_sub, \
    h2_atomic_u32_t *: h2_atomic_u32_fetch_sub, \
    h2_atomic_size_t *: h2_atomic_size_fetch_sub \
)
#define h2_atomic_fetch_sub_explicit(value, amount, order) h2_atomic_fetch_sub_dispatch(value)(value, amount, order)
#define h2_atomic_fetch_sub(value, amount) h2_atomic_fetch_sub_explicit(value, amount, H2_ATOMIC_SEQ_CST)
#define h2_atomic_fetch_or_dispatch(value) _Generic((value), \
    h2_atomic_int_t *: h2_atomic_int_fetch_or, \
    h2_atomic_uint_t *: h2_atomic_uint_fetch_or, \
    h2_atomic_u8_t *: h2_atomic_u8_fetch_or, \
    h2_atomic_u16_t *: h2_atomic_u16_fetch_or, \
    h2_atomic_u32_t *: h2_atomic_u32_fetch_or, \
    h2_atomic_size_t *: h2_atomic_size_fetch_or \
)
#define h2_atomic_fetch_or_explicit(value, mask, order) h2_atomic_fetch_or_dispatch(value)(value, mask, order)
#define h2_atomic_fetch_or(value, mask) h2_atomic_fetch_or_explicit(value, mask, H2_ATOMIC_SEQ_CST)
#define h2_atomic_fetch_and_dispatch(value) _Generic((value), \
    h2_atomic_int_t *: h2_atomic_int_fetch_and, \
    h2_atomic_uint_t *: h2_atomic_uint_fetch_and, \
    h2_atomic_u8_t *: h2_atomic_u8_fetch_and, \
    h2_atomic_u16_t *: h2_atomic_u16_fetch_and, \
    h2_atomic_u32_t *: h2_atomic_u32_fetch_and, \
    h2_atomic_size_t *: h2_atomic_size_fetch_and \
)
#define h2_atomic_fetch_and_explicit(value, mask, order) h2_atomic_fetch_and_dispatch(value)(value, mask, order)
#define h2_atomic_fetch_and(value, mask) h2_atomic_fetch_and_explicit(value, mask, H2_ATOMIC_SEQ_CST)

#endif /* !__cplusplus */
#endif
