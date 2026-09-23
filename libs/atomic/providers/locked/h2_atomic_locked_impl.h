/* Included by exactly one platform source. The platform supplies allocation
 * from atomic-safe memory and a scheduler-safe critical section. */
#ifndef H2_ATOMIC_PLATFORM_ALLOC
#error "H2_ATOMIC_PLATFORM_ALLOC is required"
#endif
#ifndef H2_ATOMIC_PLATFORM_FREE
#error "H2_ATOMIC_PLATFORM_FREE is required"
#endif

#include "h2_atomic.h"

#define H2_ATOMIC_DEFINE_INTEGER(name, type) \
    struct h2_atomic_##name##_storage { type value; }; \
    h2_atomic_result_t h2_atomic_##name##_init(h2_atomic_##name##_t *object, type initial) { \
        if (object == NULL) return H2_ATOMIC_INVALID_ARG; \
        if (object->storage != NULL) return H2_ATOMIC_INVALID_STATE; \
        object->storage = H2_ATOMIC_PLATFORM_ALLOC(sizeof(*object->storage)); \
        if (object->storage == NULL) return H2_ATOMIC_NO_MEMORY; \
        object->storage->value = initial; \
        return H2_ATOMIC_OK; \
    } \
    void h2_atomic_##name##_destroy(h2_atomic_##name##_t *object) { \
        if (object == NULL) return; \
        if (object->storage != NULL) H2_ATOMIC_PLATFORM_FREE(object->storage); \
        object->storage = NULL; \
    } \
    type h2_atomic_##name##_load(const h2_atomic_##name##_t *object, h2_atomic_order_t order) { \
        (void)order; \
        h2_atomic_platform_lock_state_t state = h2_atomic_platform_lock(); \
        type result = object->storage->value; \
        h2_atomic_platform_unlock(state); \
        return result; \
    } \
    void h2_atomic_##name##_store(h2_atomic_##name##_t *object, type next, h2_atomic_order_t order) { \
        (void)order; \
        h2_atomic_platform_lock_state_t state = h2_atomic_platform_lock(); \
        object->storage->value = next; \
        h2_atomic_platform_unlock(state); \
    } \
    type h2_atomic_##name##_exchange(h2_atomic_##name##_t *object, type next, h2_atomic_order_t order) { \
        (void)order; \
        h2_atomic_platform_lock_state_t state = h2_atomic_platform_lock(); \
        type previous = object->storage->value; \
        object->storage->value = next; \
        h2_atomic_platform_unlock(state); \
        return previous; \
    } \
    bool h2_atomic_##name##_compare_exchange(h2_atomic_##name##_t *object, type *expected, type desired, h2_atomic_order_t success, h2_atomic_order_t failure) { \
        (void)success; (void)failure; \
        h2_atomic_platform_lock_state_t state = h2_atomic_platform_lock(); \
        bool matched = object->storage->value == *expected; \
        if (matched) object->storage->value = desired; \
        else *expected = object->storage->value; \
        h2_atomic_platform_unlock(state); \
        return matched; \
    } \
    type h2_atomic_##name##_fetch_add(h2_atomic_##name##_t *object, type amount, h2_atomic_order_t order) { \
        (void)order; \
        h2_atomic_platform_lock_state_t state = h2_atomic_platform_lock(); \
        type previous = object->storage->value; \
        object->storage->value = (type)((uint64_t)(type)previous + (uint64_t)(type)amount); \
        h2_atomic_platform_unlock(state); \
        return previous; \
    } \
    type h2_atomic_##name##_fetch_sub(h2_atomic_##name##_t *object, type amount, h2_atomic_order_t order) { \
        (void)order; \
        h2_atomic_platform_lock_state_t state = h2_atomic_platform_lock(); \
        type previous = object->storage->value; \
        object->storage->value = (type)((uint64_t)(type)previous - (uint64_t)(type)amount); \
        h2_atomic_platform_unlock(state); \
        return previous; \
    } \
    type h2_atomic_##name##_fetch_or(h2_atomic_##name##_t *object, type mask, h2_atomic_order_t order) { \
        (void)order; \
        h2_atomic_platform_lock_state_t state = h2_atomic_platform_lock(); \
        type previous = object->storage->value; \
        object->storage->value = (type)(previous | mask); \
        h2_atomic_platform_unlock(state); \
        return previous; \
    } \
    type h2_atomic_##name##_fetch_and(h2_atomic_##name##_t *object, type mask, h2_atomic_order_t order) { \
        (void)order; \
        h2_atomic_platform_lock_state_t state = h2_atomic_platform_lock(); \
        type previous = object->storage->value; \
        object->storage->value = (type)(previous & mask); \
        h2_atomic_platform_unlock(state); \
        return previous; \
    }

H2_ATOMIC_DEFINE_INTEGER(int, int)
H2_ATOMIC_DEFINE_INTEGER(uint, unsigned int)
H2_ATOMIC_DEFINE_INTEGER(u8, uint8_t)
H2_ATOMIC_DEFINE_INTEGER(u16, uint16_t)
H2_ATOMIC_DEFINE_INTEGER(u32, uint32_t)
H2_ATOMIC_DEFINE_INTEGER(size, size_t)
#undef H2_ATOMIC_DEFINE_INTEGER

#define H2_ATOMIC_DEFINE_SCALAR(name, type) \
    struct h2_atomic_##name##_storage { type value; }; \
    h2_atomic_result_t h2_atomic_##name##_init(h2_atomic_##name##_t *object, type initial) { \
        if (object == NULL) return H2_ATOMIC_INVALID_ARG; \
        if (object->storage != NULL) return H2_ATOMIC_INVALID_STATE; \
        object->storage = H2_ATOMIC_PLATFORM_ALLOC(sizeof(*object->storage)); \
        if (object->storage == NULL) return H2_ATOMIC_NO_MEMORY; \
        object->storage->value = initial; \
        return H2_ATOMIC_OK; \
    } \
    void h2_atomic_##name##_destroy(h2_atomic_##name##_t *object) { \
        if (object == NULL) return; \
        if (object->storage != NULL) H2_ATOMIC_PLATFORM_FREE(object->storage); \
        object->storage = NULL; \
    } \
    type h2_atomic_##name##_load(const h2_atomic_##name##_t *object, h2_atomic_order_t order) { \
        (void)order; \
        h2_atomic_platform_lock_state_t state = h2_atomic_platform_lock(); \
        type result = object->storage->value; \
        h2_atomic_platform_unlock(state); \
        return result; \
    } \
    void h2_atomic_##name##_store(h2_atomic_##name##_t *object, type next, h2_atomic_order_t order) { \
        (void)order; \
        h2_atomic_platform_lock_state_t state = h2_atomic_platform_lock(); \
        object->storage->value = next; \
        h2_atomic_platform_unlock(state); \
    } \
    type h2_atomic_##name##_exchange(h2_atomic_##name##_t *object, type next, h2_atomic_order_t order) { \
        (void)order; \
        h2_atomic_platform_lock_state_t state = h2_atomic_platform_lock(); \
        type result = object->storage->value; \
        object->storage->value = next; \
        h2_atomic_platform_unlock(state); \
        return result; \
    } \
    bool h2_atomic_##name##_compare_exchange(h2_atomic_##name##_t *object, type *expected, type desired, h2_atomic_order_t success, h2_atomic_order_t failure) { \
        (void)success; (void)failure; \
        h2_atomic_platform_lock_state_t state = h2_atomic_platform_lock(); \
        bool matched = object->storage->value == *expected; \
        if (matched) object->storage->value = desired; \
        else *expected = object->storage->value; \
        h2_atomic_platform_unlock(state); \
        return matched; \
    }
H2_ATOMIC_DEFINE_SCALAR(bool, bool)
H2_ATOMIC_DEFINE_SCALAR(ptr, void *)
#undef H2_ATOMIC_DEFINE_SCALAR

struct h2_atomic_flag_storage { bool value; };
static h2_atomic_flag_storage_t *h2_atomic_flag_storage(h2_atomic_flag_t *object) {
    h2_atomic_platform_lock_state_t state = h2_atomic_platform_lock();
    h2_atomic_flag_storage_t *storage = object->storage;
    h2_atomic_platform_unlock(state);
    if (storage != NULL) return storage;
    h2_atomic_flag_storage_t *candidate = H2_ATOMIC_PLATFORM_ALLOC(sizeof(*candidate));
    if (candidate == NULL) __builtin_trap();
    candidate->value = false;
    state = h2_atomic_platform_lock();
    if (object->storage == NULL) {
        object->storage = candidate;
        storage = candidate;
        candidate = NULL;
    } else {
        storage = object->storage;
    }
    h2_atomic_platform_unlock(state);
    if (candidate != NULL) H2_ATOMIC_PLATFORM_FREE(candidate);
    return storage;
}
h2_atomic_result_t h2_atomic_flag_init(h2_atomic_flag_t *object) {
    if (object == NULL) return H2_ATOMIC_INVALID_ARG;
    h2_atomic_platform_lock_state_t state = h2_atomic_platform_lock();
    bool initialized = object->storage != NULL;
    h2_atomic_platform_unlock(state);
    if (initialized) return H2_ATOMIC_INVALID_STATE;
    h2_atomic_flag_storage(object);
    return H2_ATOMIC_OK;
}
void h2_atomic_flag_destroy(h2_atomic_flag_t *object) {
    if (object == NULL) return;
    if (object->storage != NULL) H2_ATOMIC_PLATFORM_FREE(object->storage);
    object->storage = NULL;
}
bool h2_atomic_flag_test_and_set(h2_atomic_flag_t *object, h2_atomic_order_t order) {
    (void)order;
    h2_atomic_flag_storage_t *storage = h2_atomic_flag_storage(object);
    h2_atomic_platform_lock_state_t state = h2_atomic_platform_lock();
    bool previous = storage->value;
    storage->value = true;
    h2_atomic_platform_unlock(state);
    return previous;
}
void h2_atomic_flag_clear(h2_atomic_flag_t *object, h2_atomic_order_t order) {
    (void)order;
    h2_atomic_flag_storage_t *storage = h2_atomic_flag_storage(object);
    h2_atomic_platform_lock_state_t state = h2_atomic_platform_lock();
    storage->value = false;
    h2_atomic_platform_unlock(state);
}
