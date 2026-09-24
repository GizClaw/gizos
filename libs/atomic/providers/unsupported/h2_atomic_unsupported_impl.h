/* Include in exactly one platform source when atomic support is unavailable.
 * Initialization reports the limitation; using an uninitialized value traps
 * instead of returning a plausible but incorrect result. */
#include "h2_atomic.h"

#define H2_ATOMIC_UNAVAILABLE() __builtin_trap()

#define H2_ATOMIC_UNSUPPORTED_INTEGER(name, type) \
    h2_atomic_result_t h2_atomic_##name##_init(h2_atomic_##name##_t *object, type initial) { \
        (void)initial; \
        if (object != NULL) object->storage = NULL; \
        return H2_ATOMIC_UNSUPPORTED; \
    } \
    void h2_atomic_##name##_destroy(h2_atomic_##name##_t *object) { \
        if (object != NULL) object->storage = NULL; \
    } \
    type h2_atomic_##name##_load(const h2_atomic_##name##_t *object, h2_atomic_order_t order) { \
        (void)object; (void)order; H2_ATOMIC_UNAVAILABLE(); \
    } \
    void h2_atomic_##name##_store(h2_atomic_##name##_t *object, type next, h2_atomic_order_t order) { \
        (void)object; (void)next; (void)order; H2_ATOMIC_UNAVAILABLE(); \
    } \
    type h2_atomic_##name##_exchange(h2_atomic_##name##_t *object, type next, h2_atomic_order_t order) { \
        (void)object; (void)next; (void)order; H2_ATOMIC_UNAVAILABLE(); \
    } \
    bool h2_atomic_##name##_compare_exchange(h2_atomic_##name##_t *object, type *expected, type desired, h2_atomic_order_t success, h2_atomic_order_t failure) { \
        (void)object; (void)expected; (void)desired; (void)success; (void)failure; H2_ATOMIC_UNAVAILABLE(); \
    } \
    type h2_atomic_##name##_fetch_add(h2_atomic_##name##_t *object, type amount, h2_atomic_order_t order) { \
        (void)object; (void)amount; (void)order; H2_ATOMIC_UNAVAILABLE(); \
    } \
    type h2_atomic_##name##_fetch_sub(h2_atomic_##name##_t *object, type amount, h2_atomic_order_t order) { \
        (void)object; (void)amount; (void)order; H2_ATOMIC_UNAVAILABLE(); \
    } \
    type h2_atomic_##name##_fetch_or(h2_atomic_##name##_t *object, type mask, h2_atomic_order_t order) { \
        (void)object; (void)mask; (void)order; H2_ATOMIC_UNAVAILABLE(); \
    } \
    type h2_atomic_##name##_fetch_and(h2_atomic_##name##_t *object, type mask, h2_atomic_order_t order) { \
        (void)object; (void)mask; (void)order; H2_ATOMIC_UNAVAILABLE(); \
    }

H2_ATOMIC_UNSUPPORTED_INTEGER(int, int)
H2_ATOMIC_UNSUPPORTED_INTEGER(uint, unsigned int)
H2_ATOMIC_UNSUPPORTED_INTEGER(u8, uint8_t)
H2_ATOMIC_UNSUPPORTED_INTEGER(u16, uint16_t)
H2_ATOMIC_UNSUPPORTED_INTEGER(u32, uint32_t)
H2_ATOMIC_UNSUPPORTED_INTEGER(size, size_t)
#undef H2_ATOMIC_UNSUPPORTED_INTEGER

#define H2_ATOMIC_UNSUPPORTED_SCALAR(name, type) \
    h2_atomic_result_t h2_atomic_##name##_init(h2_atomic_##name##_t *object, type initial) { \
        (void)initial; \
        if (object != NULL) object->storage = NULL; \
        return H2_ATOMIC_UNSUPPORTED; \
    } \
    void h2_atomic_##name##_destroy(h2_atomic_##name##_t *object) { \
        if (object != NULL) object->storage = NULL; \
    } \
    type h2_atomic_##name##_load(const h2_atomic_##name##_t *object, h2_atomic_order_t order) { \
        (void)object; (void)order; H2_ATOMIC_UNAVAILABLE(); \
    } \
    void h2_atomic_##name##_store(h2_atomic_##name##_t *object, type next, h2_atomic_order_t order) { \
        (void)object; (void)next; (void)order; H2_ATOMIC_UNAVAILABLE(); \
    } \
    type h2_atomic_##name##_exchange(h2_atomic_##name##_t *object, type next, h2_atomic_order_t order) { \
        (void)object; (void)next; (void)order; H2_ATOMIC_UNAVAILABLE(); \
    } \
    bool h2_atomic_##name##_compare_exchange(h2_atomic_##name##_t *object, type *expected, type desired, h2_atomic_order_t success, h2_atomic_order_t failure) { \
        (void)object; (void)expected; (void)desired; (void)success; (void)failure; H2_ATOMIC_UNAVAILABLE(); \
    }

H2_ATOMIC_UNSUPPORTED_SCALAR(bool, bool)
H2_ATOMIC_UNSUPPORTED_SCALAR(ptr, void *)
#undef H2_ATOMIC_UNSUPPORTED_SCALAR

h2_atomic_result_t h2_atomic_flag_init(h2_atomic_flag_t *object) {
    if (object != NULL) object->storage = NULL;
    return H2_ATOMIC_UNSUPPORTED;
}
void h2_atomic_flag_destroy(h2_atomic_flag_t *object) {
    if (object != NULL) object->storage = NULL;
}
bool h2_atomic_flag_test_and_set(h2_atomic_flag_t *object, h2_atomic_order_t order) {
    (void)object; (void)order; H2_ATOMIC_UNAVAILABLE();
}
void h2_atomic_flag_clear(h2_atomic_flag_t *object, h2_atomic_order_t order) {
    (void)object; (void)order; H2_ATOMIC_UNAVAILABLE();
}
#undef H2_ATOMIC_UNAVAILABLE
