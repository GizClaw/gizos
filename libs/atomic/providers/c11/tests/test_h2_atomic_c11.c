#include "h2_atomic.h"

#include <assert.h>
#include <pthread.h>

H2_ATOMIC_DEFINE_STATIC(int, s_static_int, 7);
H2_ATOMIC_DEFINE_STATIC(uint, s_static_uint, 9u);
H2_ATOMIC_DEFINE_STATIC(u8, s_static_u8, UINT8_C(3));
H2_ATOMIC_DEFINE_STATIC(u16, s_static_u16, UINT16_C(5));
H2_ATOMIC_DEFINE_STATIC(u32, s_static_u32, UINT32_C(11));
H2_ATOMIC_DEFINE_STATIC(size, s_static_size, (size_t)13u);
H2_ATOMIC_DEFINE_STATIC(bool, s_static_bool, false);
H2_ATOMIC_DEFINE_STATIC(ptr, s_static_ptr, NULL);
H2_ATOMIC_DEFINE_STATIC(flag, s_static_flag, 0u);
H2_ATOMIC_DEFINE_STATIC(flag, s_other_static_flag, 0u);

static void *increment(void *argument) {
    h2_atomic_uint_t *count = argument;
    for (unsigned i = 0; i < 100000; ++i)
        h2_atomic_uint_fetch_add(count, 1u, H2_ATOMIC_RELAXED);
    return NULL;
}

typedef struct flag_counter {
    h2_atomic_flag_t guard;
    unsigned value;
} flag_counter_t;

static void *increment_with_flag(void *argument) {
    flag_counter_t *counter = argument;
    for (unsigned i = 0; i < 10000u; ++i) {
        while (h2_atomic_flag_test_and_set(&counter->guard, H2_ATOMIC_ACQUIRE)) {}
        counter->value++;
        h2_atomic_flag_clear(&counter->guard, H2_ATOMIC_RELEASE);
    }
    return NULL;
}

int main(void) {
    assert(h2_atomic_int_load(&s_static_int, H2_ATOMIC_RELAXED) == 7);
    assert(h2_atomic_uint_load(&s_static_uint, H2_ATOMIC_RELAXED) == 9u);
    assert(h2_atomic_u8_load(&s_static_u8, H2_ATOMIC_RELAXED) == 3u);
    assert(h2_atomic_u16_load(&s_static_u16, H2_ATOMIC_RELAXED) == 5u);
    assert(h2_atomic_u32_load(&s_static_u32, H2_ATOMIC_RELAXED) == 11u);
    assert(h2_atomic_size_load(&s_static_size, H2_ATOMIC_RELAXED) == 13u);
    assert(!h2_atomic_bool_load(&s_static_bool, H2_ATOMIC_RELAXED));
    assert(h2_atomic_ptr_load(&s_static_ptr, H2_ATOMIC_RELAXED) == NULL);
    assert(!h2_atomic_flag_test_and_set(&s_static_flag, H2_ATOMIC_ACQUIRE));
    assert(s_static_flag.storage != s_other_static_flag.storage);
    assert(!h2_atomic_flag_test_and_set(&s_other_static_flag, H2_ATOMIC_ACQUIRE));
    assert(h2_atomic_flag_test_and_set(&s_static_flag, H2_ATOMIC_ACQUIRE));
    h2_atomic_flag_clear(&s_static_flag, H2_ATOMIC_RELEASE);
    assert(h2_atomic_flag_init(&s_static_flag) == H2_ATOMIC_INVALID_STATE);
    h2_atomic_flag_destroy(&s_static_flag);
    assert(s_static_flag.storage == &s_static_flag_h2_storage);
    assert(!h2_atomic_flag_test_and_set(&s_static_flag, H2_ATOMIC_ACQUIRE));
    h2_atomic_int_destroy(&s_static_int);
    assert(s_static_int.storage == &s_static_int_h2_storage);
    h2_atomic_uint_t count = {0};
    assert(h2_atomic_uint_init(&count, 0u) == H2_ATOMIC_OK);
    pthread_t threads[2];
    for (unsigned i = 0; i < 2; ++i)
        assert(pthread_create(&threads[i], NULL, increment, &count) == 0);
    for (unsigned i = 0; i < 2; ++i)
        assert(pthread_join(threads[i], NULL) == 0);
    assert(h2_atomic_uint_load(&count, H2_ATOMIC_ACQUIRE) == 200000u);
    unsigned expected = 200000u;
    assert(h2_atomic_uint_compare_exchange(&count, &expected, 7u,
                                           H2_ATOMIC_ACQ_REL, H2_ATOMIC_ACQUIRE));
    assert(h2_atomic_uint_exchange(&count, 0u, H2_ATOMIC_SEQ_CST) == 7u);
    /* Invalid C11 order combinations must be normalized by the provider. */
    h2_atomic_uint_store(&count, 11u, H2_ATOMIC_ACQUIRE);
    assert(h2_atomic_uint_load(&count, H2_ATOMIC_RELEASE) == 11u);
    expected = 11u;
    assert(h2_atomic_uint_compare_exchange(&count, &expected, 12u,
                                           H2_ATOMIC_RELEASE, H2_ATOMIC_ACQUIRE));
    expected = 12u;
    assert(h2_atomic_uint_compare_exchange(&count, &expected, 13u,
                                           H2_ATOMIC_RELAXED, H2_ATOMIC_ACQ_REL));
    h2_atomic_uint_destroy(&count);
    assert(count.storage == NULL);

    h2_atomic_u16_t narrow = {0};
    assert(h2_atomic_u16_init(&narrow, UINT16_MAX) == H2_ATOMIC_OK);
    assert(h2_atomic_u16_fetch_add(&narrow, 1, H2_ATOMIC_RELAXED) == UINT16_MAX);
    assert(h2_atomic_u16_load(&narrow, H2_ATOMIC_RELAXED) == 0);
    h2_atomic_u16_destroy(&narrow);

    h2_atomic_u32_t word = {0};
    assert(h2_atomic_u32_init(&word, UINT32_MAX) == H2_ATOMIC_OK);
    assert(h2_atomic_u32_fetch_sub(&word, 1, H2_ATOMIC_RELAXED) == UINT32_MAX);
    h2_atomic_u32_destroy(&word);

    h2_atomic_bool_t ready = {0};
    assert(h2_atomic_bool_init(&ready, false) == H2_ATOMIC_OK);
    assert(!h2_atomic_bool_exchange(&ready, true, H2_ATOMIC_ACQ_REL));
    assert(h2_atomic_bool_load(&ready, H2_ATOMIC_ACQUIRE));
    h2_atomic_bool_destroy(&ready);

    h2_atomic_flag_t guard = {0};
    h2_atomic_flag_t other_guard = {0};
    assert(h2_atomic_flag_init(&guard) == H2_ATOMIC_OK);
    assert(h2_atomic_flag_init(&other_guard) == H2_ATOMIC_OK);
    assert(guard.storage != other_guard.storage);
    assert(h2_atomic_flag_init(&guard) == H2_ATOMIC_INVALID_STATE);
    assert(!h2_atomic_flag_test_and_set(&guard, H2_ATOMIC_ACQUIRE));
    assert(h2_atomic_flag_test_and_set(&guard, H2_ATOMIC_ACQUIRE));
    h2_atomic_flag_clear(&guard, H2_ATOMIC_RELEASE);
    assert(!h2_atomic_flag_test_and_set(&guard, H2_ATOMIC_ACQUIRE));
    assert(!h2_atomic_flag_test_and_set(&other_guard, H2_ATOMIC_ACQUIRE));
    h2_atomic_flag_destroy(&other_guard);
    h2_atomic_flag_destroy(&guard);
    assert(guard.storage == NULL);

    flag_counter_t guarded_counter = {0};
    assert(h2_atomic_flag_init(&guarded_counter.guard) == H2_ATOMIC_OK);
    for (unsigned i = 0; i < 2; ++i)
        assert(pthread_create(&threads[i], NULL, increment_with_flag,
                              &guarded_counter) == 0);
    for (unsigned i = 0; i < 2; ++i)
        assert(pthread_join(threads[i], NULL) == 0);
    assert(guarded_counter.value == 20000u);
    h2_atomic_flag_destroy(&guarded_counter.guard);
    return 0;
}
