#include "h2/pal/os/h2_pal_atomic.h"
#include "h2/pal/h2_pal_unsupported.h"
#include "h2_c11_pal_atomic.h"
#include <assert.h>
#include <pthread.h>

#define CHECK(x) assert((x) == H2_PAL_OK)

typedef struct worker_arg { _Atomic uint32_t *count; } worker_arg_t;
static void *worker(void *argument) {
    worker_arg_t *arg = argument;
    for (unsigned i = 0; i < 100000; ++i) atomic_fetch_add(arg->count, 1);
    return NULL;
}

int main(void) {
    const h2_pal_atomic_api_t *api = h2_c11_pal_atomic_api();
    _Atomic uint32_t *u = NULL;
    _Atomic int32_t *i = NULL;
    _Atomic bool *b = NULL;
    _Atomic(void *) *p = NULL;
    atomic_flag *f = NULL;
    int marker = 0;
    CHECK(h2_pal_atomic_alloc_u32(api, 3, &u));
    CHECK(h2_pal_atomic_alloc_i32(api, -3, &i));
    CHECK(h2_pal_atomic_alloc_bool(api, true, &b));
    CHECK(h2_pal_atomic_alloc_ptr(api, &marker, &p));
    CHECK(h2_pal_atomic_alloc_flag(api, &f));
    assert(atomic_load(u) == 3 && atomic_load(i) == -3);
    assert(atomic_load(b) && atomic_load(p) == &marker);
    assert(!atomic_flag_test_and_set(f));
    atomic_flag_clear(f);
    assert(atomic_exchange(u, 0) == 3);
    uint32_t expected = 0;
    assert(atomic_compare_exchange_strong(u, &expected, 1));
    assert(atomic_fetch_sub(u, 1) == 1);
    assert(atomic_fetch_or(u, 3) == 0);
    assert(atomic_fetch_and(u, 1) == 3);
    atomic_store(u, 0);
    pthread_t threads[4];
    worker_arg_t arg = { .count = u };
    for (unsigned n = 0; n < 4; ++n) assert(pthread_create(&threads[n], NULL, worker, &arg) == 0);
    for (unsigned n = 0; n < 4; ++n) assert(pthread_join(threads[n], NULL) == 0);
    assert(atomic_load(u) == 400000);
    CHECK(h2_pal_atomic_free(api, u));
    CHECK(h2_pal_atomic_free(api, i));
    CHECK(h2_pal_atomic_free(api, b));
    CHECK(h2_pal_atomic_free(api, p));
    CHECK(h2_pal_atomic_free(api, f));
    CHECK(h2_pal_atomic_free(api, NULL));
    assert(h2_pal_atomic_alloc_u32(api, 0, NULL) == H2_PAL_ERR_INVALID_ARG);
    u = (_Atomic uint32_t *)&marker;
    assert(h2_pal_atomic_alloc_u32(NULL, 0, &u) == H2_PAL_ERR_UNSUPPORTED && u == NULL);
    void *raw = &marker;
    assert(h2_pal_atomic_alloc_raw(api, 4, 3, &raw) == H2_PAL_ERR_INVALID_ARG && raw == NULL);
    assert(h2_pal_atomic_free(NULL, NULL) == H2_PAL_ERR_UNSUPPORTED);
    void *aligned = NULL;
    CHECK(h2_pal_atomic_alloc_raw(api, sizeof(long double), _Alignof(long double), &aligned));
    assert(((uintptr_t)aligned % _Alignof(long double)) == 0);
    CHECK(h2_pal_atomic_free(api, aligned));
    u = (_Atomic uint32_t *)&marker;
    assert(h2_pal_atomic_alloc_u32(h2_pal_unsupported_atomic_api(), 0, &u) ==
           H2_PAL_ERR_UNSUPPORTED && u == NULL);
    return 0;
}
