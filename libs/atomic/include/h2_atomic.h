#ifndef H2_ATOMIC_H
#define H2_ATOMIC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The wrapper may live anywhere. Its storage is allocated by the linked
 * platform implementation. Initialize once, do not copy after initialization,
 * and destroy only after all concurrent users have stopped. */
typedef struct h2_atomic_int_storage h2_atomic_int_storage_t;
typedef struct h2_atomic_uint_storage h2_atomic_uint_storage_t;
typedef struct h2_atomic_u8_storage h2_atomic_u8_storage_t;
typedef struct h2_atomic_u16_storage h2_atomic_u16_storage_t;
typedef struct h2_atomic_u32_storage h2_atomic_u32_storage_t;
typedef struct h2_atomic_bool_storage h2_atomic_bool_storage_t;
typedef struct h2_atomic_size_storage h2_atomic_size_storage_t;
typedef struct h2_atomic_ptr_storage h2_atomic_ptr_storage_t;
typedef struct h2_atomic_flag_storage h2_atomic_flag_storage_t;

typedef struct { h2_atomic_int_storage_t *storage; } h2_atomic_int_t;
typedef struct { h2_atomic_uint_storage_t *storage; } h2_atomic_uint_t;
typedef struct { h2_atomic_u8_storage_t *storage; } h2_atomic_u8_t;
typedef struct { h2_atomic_u16_storage_t *storage; } h2_atomic_u16_t;
typedef struct { h2_atomic_u32_storage_t *storage; } h2_atomic_u32_t;
typedef struct { h2_atomic_bool_storage_t *storage; } h2_atomic_bool_t;
typedef struct { h2_atomic_size_storage_t *storage; } h2_atomic_size_t;
typedef struct { h2_atomic_ptr_storage_t *storage; } h2_atomic_ptr_t;
typedef struct { h2_atomic_flag_storage_t *storage; } h2_atomic_flag_t;

typedef enum h2_atomic_result {
    H2_ATOMIC_OK = 0,
    H2_ATOMIC_INVALID_ARG,
    H2_ATOMIC_INVALID_STATE,
    H2_ATOMIC_NO_MEMORY,
} h2_atomic_result_t;

typedef enum h2_atomic_order {
    H2_ATOMIC_RELAXED,
    H2_ATOMIC_ACQUIRE,
    H2_ATOMIC_RELEASE,
    H2_ATOMIC_ACQ_REL,
    H2_ATOMIC_SEQ_CST,
} h2_atomic_order_t;

/* No implementation or fallback is supplied by this target. A final binary
 * must link one implementation of these symbols for its platform. */
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
#endif
