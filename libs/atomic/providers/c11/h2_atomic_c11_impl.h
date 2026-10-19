#include "h2_atomic_static.h"

#include <stdatomic.h>
#include <stdlib.h>

#ifndef H2_ATOMIC_C11_ALLOC
#define H2_ATOMIC_C11_ALLOC(size) malloc(size)
#endif
#ifndef H2_ATOMIC_C11_FREE
#define H2_ATOMIC_C11_FREE(pointer) free(pointer)
#endif

/* Include this implementation in exactly one platform source. The platform
 * controls storage placement through H2_ATOMIC_C11_ALLOC/FREE. */
#ifndef H2_ATOMIC_C11_ORDER
static memory_order c11_order(h2_atomic_order_t order) {
    switch (order) {
        case H2_ATOMIC_RELAXED: return memory_order_relaxed;
        case H2_ATOMIC_ACQUIRE: return memory_order_acquire;
        case H2_ATOMIC_RELEASE: return memory_order_release;
        case H2_ATOMIC_ACQ_REL: return memory_order_acq_rel;
        case H2_ATOMIC_SEQ_CST: return memory_order_seq_cst;
    }
    return memory_order_seq_cst;
}
#define H2_ATOMIC_C11_ORDER(order) c11_order(order)
#endif

/* C11 forbids release/acq_rel loads, acquire/acq_rel stores, and release or
 * acq_rel CAS failure orders. Invalid public order combinations are promoted
 * to valid, conservative orders before reaching the C11 builtins. */
static h2_atomic_order_t c11_load_order(h2_atomic_order_t order) {
    return order == H2_ATOMIC_RELAXED || order == H2_ATOMIC_ACQUIRE ||
                   order == H2_ATOMIC_SEQ_CST ? order : H2_ATOMIC_SEQ_CST;
}
static h2_atomic_order_t c11_store_order(h2_atomic_order_t order) {
    return order == H2_ATOMIC_RELAXED || order == H2_ATOMIC_RELEASE ||
                   order == H2_ATOMIC_SEQ_CST ? order : H2_ATOMIC_SEQ_CST;
}
static h2_atomic_order_t c11_failure_order(h2_atomic_order_t success,
                                            h2_atomic_order_t failure) {
    if (failure == H2_ATOMIC_RELAXED) return failure;
    if (failure == H2_ATOMIC_ACQUIRE &&
        (success == H2_ATOMIC_ACQUIRE || success == H2_ATOMIC_ACQ_REL ||
         success == H2_ATOMIC_SEQ_CST)) return failure;
    if (failure == H2_ATOMIC_SEQ_CST && success == H2_ATOMIC_SEQ_CST)
        return failure;
    return H2_ATOMIC_RELAXED;
}
#define H2_ATOMIC_C11_LOAD_ORDER(order) H2_ATOMIC_C11_ORDER(c11_load_order(order))
#define H2_ATOMIC_C11_STORE_ORDER(order) H2_ATOMIC_C11_ORDER(c11_store_order(order))
#define H2_ATOMIC_C11_FAILURE_ORDER(success, failure) \
    H2_ATOMIC_C11_ORDER(c11_failure_order(success, failure))

#define H2_ATOMIC_DEFINE_INTEGER(name, type) \
    h2_atomic_result_t h2_atomic_##name##_init(h2_atomic_##name##_t *object, type initial) { \
        if (object == NULL) return H2_ATOMIC_INVALID_ARG; \
        if (object->storage != NULL) return H2_ATOMIC_INVALID_STATE; \
        object->storage = H2_ATOMIC_C11_ALLOC(sizeof(*object->storage)); \
        if (object->storage == NULL) return H2_ATOMIC_NO_MEMORY; \
        atomic_store_explicit(&object->storage->value, initial, memory_order_relaxed); \
        object->storage->heap_owned = true; \
        return H2_ATOMIC_OK; \
    } \
    void h2_atomic_##name##_destroy(h2_atomic_##name##_t *object) { \
        if (object == NULL) return; \
        if (object->storage != NULL && object->storage->heap_owned) { \
            H2_ATOMIC_C11_FREE(object->storage); \
            object->storage = NULL; \
        } \
    } \
    type h2_atomic_##name##_load(const h2_atomic_##name##_t *object, h2_atomic_order_t order) { \
        return atomic_load_explicit(&object->storage->value, H2_ATOMIC_C11_LOAD_ORDER(order)); \
    } \
    void h2_atomic_##name##_store(h2_atomic_##name##_t *object, type next, h2_atomic_order_t order) { \
        atomic_store_explicit(&object->storage->value, next, H2_ATOMIC_C11_STORE_ORDER(order)); \
    } \
    type h2_atomic_##name##_exchange(h2_atomic_##name##_t *object, type next, h2_atomic_order_t order) { \
        return atomic_exchange_explicit(&object->storage->value, next, H2_ATOMIC_C11_ORDER(order)); \
    } \
    bool h2_atomic_##name##_compare_exchange(h2_atomic_##name##_t *object, type *expected, type desired, h2_atomic_order_t success, h2_atomic_order_t failure) { \
        (void)failure; \
        return atomic_compare_exchange_strong_explicit(&object->storage->value, expected, desired, H2_ATOMIC_C11_ORDER(success), H2_ATOMIC_C11_FAILURE_ORDER(success, failure)); \
    } \
    type h2_atomic_##name##_fetch_add(h2_atomic_##name##_t *object, type amount, h2_atomic_order_t order) { \
        return atomic_fetch_add_explicit(&object->storage->value, amount, H2_ATOMIC_C11_ORDER(order)); \
    } \
    type h2_atomic_##name##_fetch_sub(h2_atomic_##name##_t *object, type amount, h2_atomic_order_t order) { \
        return atomic_fetch_sub_explicit(&object->storage->value, amount, H2_ATOMIC_C11_ORDER(order)); \
    } \
    type h2_atomic_##name##_fetch_or(h2_atomic_##name##_t *object, type mask, h2_atomic_order_t order) { \
        return atomic_fetch_or_explicit(&object->storage->value, mask, H2_ATOMIC_C11_ORDER(order)); \
    } \
    type h2_atomic_##name##_fetch_and(h2_atomic_##name##_t *object, type mask, h2_atomic_order_t order) { \
        return atomic_fetch_and_explicit(&object->storage->value, mask, H2_ATOMIC_C11_ORDER(order)); \
    }

H2_ATOMIC_DEFINE_INTEGER(int, int)
H2_ATOMIC_DEFINE_INTEGER(uint, unsigned int)
H2_ATOMIC_DEFINE_INTEGER(u8, uint8_t)
H2_ATOMIC_DEFINE_INTEGER(u16, uint16_t)
H2_ATOMIC_DEFINE_INTEGER(u32, uint32_t)
H2_ATOMIC_DEFINE_INTEGER(size, size_t)
#undef H2_ATOMIC_DEFINE_INTEGER

h2_atomic_result_t h2_atomic_bool_init(h2_atomic_bool_t *object, bool initial) {
    if (object == NULL) return H2_ATOMIC_INVALID_ARG;
    if (object->storage != NULL) return H2_ATOMIC_INVALID_STATE;
    object->storage = H2_ATOMIC_C11_ALLOC(sizeof(*object->storage));
    if (object->storage == NULL) return H2_ATOMIC_NO_MEMORY;
    atomic_store_explicit(&object->storage->value, initial, memory_order_relaxed);
    object->storage->heap_owned = true;
    return H2_ATOMIC_OK;
}
void h2_atomic_bool_destroy(h2_atomic_bool_t *object) {
    if (object == NULL) return;
    if (object->storage != NULL && object->storage->heap_owned) {
        H2_ATOMIC_C11_FREE(object->storage);
        object->storage = NULL;
    }
}
bool h2_atomic_bool_load(const h2_atomic_bool_t *object, h2_atomic_order_t order) {
    return atomic_load_explicit(&object->storage->value, H2_ATOMIC_C11_LOAD_ORDER(order));
}
void h2_atomic_bool_store(h2_atomic_bool_t *object, bool next, h2_atomic_order_t order) {
    atomic_store_explicit(&object->storage->value, next, H2_ATOMIC_C11_STORE_ORDER(order));
}
bool h2_atomic_bool_exchange(h2_atomic_bool_t *object, bool next, h2_atomic_order_t order) {
    return atomic_exchange_explicit(&object->storage->value, next, H2_ATOMIC_C11_ORDER(order));
}
bool h2_atomic_bool_compare_exchange(h2_atomic_bool_t *object, bool *expected,
                                     bool desired, h2_atomic_order_t success,
                                     h2_atomic_order_t failure) {
    (void)failure;
    return atomic_compare_exchange_strong_explicit(&object->storage->value, expected, desired,
                                       H2_ATOMIC_C11_ORDER(success), H2_ATOMIC_C11_FAILURE_ORDER(success, failure));
}

h2_atomic_result_t h2_atomic_ptr_init(h2_atomic_ptr_t *object, void *initial) {
    if (object == NULL) return H2_ATOMIC_INVALID_ARG;
    if (object->storage != NULL) return H2_ATOMIC_INVALID_STATE;
    object->storage = H2_ATOMIC_C11_ALLOC(sizeof(*object->storage));
    if (object->storage == NULL) return H2_ATOMIC_NO_MEMORY;
    atomic_store_explicit(&object->storage->value, initial, memory_order_relaxed);
    object->storage->heap_owned = true;
    return H2_ATOMIC_OK;
}
void h2_atomic_ptr_destroy(h2_atomic_ptr_t *object) {
    if (object == NULL) return;
    if (object->storage != NULL && object->storage->heap_owned) {
        H2_ATOMIC_C11_FREE(object->storage);
        object->storage = NULL;
    }
}
void *h2_atomic_ptr_load(const h2_atomic_ptr_t *object, h2_atomic_order_t order) {
    return atomic_load_explicit(&object->storage->value, H2_ATOMIC_C11_LOAD_ORDER(order));
}
void h2_atomic_ptr_store(h2_atomic_ptr_t *object, void *next, h2_atomic_order_t order) {
    atomic_store_explicit(&object->storage->value, next, H2_ATOMIC_C11_STORE_ORDER(order));
}
void *h2_atomic_ptr_exchange(h2_atomic_ptr_t *object, void *next, h2_atomic_order_t order) {
    return atomic_exchange_explicit(&object->storage->value, next, H2_ATOMIC_C11_ORDER(order));
}
bool h2_atomic_ptr_compare_exchange(h2_atomic_ptr_t *object, void **expected,
                                    void *desired, h2_atomic_order_t success,
                                    h2_atomic_order_t failure) {
    (void)failure;
    return atomic_compare_exchange_strong_explicit(&object->storage->value, expected, desired,
                                       H2_ATOMIC_C11_ORDER(success), H2_ATOMIC_C11_FAILURE_ORDER(success, failure));
}

/* Use a word-sized value so ESP's internal-RAM flag takes the native atomic
 * instruction path rather than a byte-width compiler helper. */
h2_atomic_result_t h2_atomic_flag_init(h2_atomic_flag_t *object) {
    if (object == NULL) return H2_ATOMIC_INVALID_ARG;
    if (object->storage != NULL) return H2_ATOMIC_INVALID_STATE;
    object->storage = H2_ATOMIC_C11_ALLOC(sizeof(*object->storage));
    if (object->storage == NULL) return H2_ATOMIC_NO_MEMORY;
    atomic_store_explicit(&object->storage->value, 0u, memory_order_relaxed);
    object->storage->heap_owned = true;
    return H2_ATOMIC_OK;
}
void h2_atomic_flag_destroy(h2_atomic_flag_t *object) {
    if (object == NULL) return;
    if (object->storage != NULL && object->storage->heap_owned) {
        H2_ATOMIC_C11_FREE(object->storage);
        object->storage = NULL;
    }
}
bool h2_atomic_flag_test_and_set(h2_atomic_flag_t *object, h2_atomic_order_t order) {
    return atomic_exchange_explicit(&object->storage->value, 1u,
                                    H2_ATOMIC_C11_ORDER(order)) != 0u;
}
void h2_atomic_flag_clear(h2_atomic_flag_t *object, h2_atomic_order_t order) {
    atomic_store_explicit(&object->storage->value, 0u,
                          H2_ATOMIC_C11_STORE_ORDER(order));
}
