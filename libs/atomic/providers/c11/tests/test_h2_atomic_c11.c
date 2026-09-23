#include "h2_atomic.h"

#include <assert.h>
#include <pthread.h>

static void *increment(void *argument) {
    h2_atomic_uint_t *count = argument;
    for (unsigned i = 0; i < 100000; ++i)
        h2_atomic_uint_fetch_add(count, 1u, H2_ATOMIC_RELAXED);
    return NULL;
}

int main(void) {
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
    assert(h2_atomic_flag_init(&guard) == H2_ATOMIC_OK);
    assert(!h2_atomic_flag_test_and_set(&guard, H2_ATOMIC_ACQUIRE));
    h2_atomic_flag_clear(&guard, H2_ATOMIC_RELEASE);
    h2_atomic_flag_destroy(&guard);
    return 0;
}
