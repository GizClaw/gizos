#include "h2/pal/os/h2_pal_atomic.h"
#include "h2/pal/h2_pal_unsupported.h"
#include "h2_c11_pal_atomic.h"

#include <assert.h>
#include <pthread.h>
#include <stdint.h>

#define CHECK(call) assert((call) == H2_PAL_OK)
#define ORDER H2_PAL_ATOMIC_SEQ_CST

static h2_pal_result_t fake_load(void *user, const h2_pal_atomic_u32_t *value,
                                  uint32_t *out_value, h2_pal_atomic_order_t order) {
    (void)value;
    (void)order;
    *out_value = *(uint32_t *)user;
    return H2_PAL_OK;
}

static void test_dispatch(void) {
    uint32_t state = 73;
    h2_pal_atomic_u32_t storage = H2_PAL_ATOMIC_U32_INIT(0);
    const h2_pal_atomic_vtable_t vtable = { .u32_load = fake_load };
    const h2_pal_atomic_api_t api = { .user = &state, .vtable = &vtable };
    uint32_t result = 0;
    CHECK(h2_pal_atomic_u32_load(&api, &storage, &result, ORDER));
    assert(result == 73);
    assert(h2_pal_atomic_u32_load(NULL, &storage, &result, ORDER) == H2_PAL_ERR_UNSUPPORTED);
    assert(h2_pal_atomic_u32_load(&api, &storage, NULL, ORDER) == H2_PAL_ERR_INVALID_ARG);
    assert(h2_pal_atomic_u32_load(&api, &storage, &result, H2_PAL_ATOMIC_RELEASE) == H2_PAL_ERR_INVALID_ARG);
    assert(h2_pal_atomic_u32_exchange(&api, &storage, 1, &result,
        (h2_pal_atomic_order_t)-1) == H2_PAL_ERR_INVALID_ARG);
    assert(h2_pal_atomic_u32_store(&api, &storage, 1, ORDER) == H2_PAL_ERR_UNSUPPORTED);
    bool exchanged = false;
    assert(h2_pal_atomic_u32_compare_exchange(&api, &storage, &result, 1,
        &exchanged, H2_PAL_ATOMIC_RELEASE, H2_PAL_ATOMIC_ACQUIRE) == H2_PAL_ERR_INVALID_ARG);
}

static void test_all(const h2_pal_atomic_api_t *api) {
    h2_pal_atomic_u32_t u = H2_PAL_ATOMIC_U32_INIT(3);
    h2_pal_atomic_i32_t i = H2_PAL_ATOMIC_I32_INIT(-3);
    h2_pal_atomic_bool_t b = H2_PAL_ATOMIC_BOOL_INIT(false);
    h2_pal_atomic_ptr_t p = H2_PAL_ATOMIC_PTR_INIT(NULL);
    h2_pal_atomic_flag_t f = H2_PAL_ATOMIC_FLAG_INIT;
    uint32_t uout = 0, uexpected = 0;
    int iout = 0, iexpected = 0;
    bool bout = false, bexpected = false, changed = false;
    int first = 1, second = 2;
    void *pout = NULL, *pexpected = NULL;

    CHECK(h2_pal_atomic_u32_load(api, &u, &uout, H2_PAL_ATOMIC_RELAXED)); assert(uout == 3);
    CHECK(h2_pal_atomic_u32_store(api, &u, 5, H2_PAL_ATOMIC_RELEASE));
    CHECK(h2_pal_atomic_u32_exchange(api, &u, 7, &uout, ORDER)); assert(uout == 5);
    uexpected = 6;
    CHECK(h2_pal_atomic_u32_compare_exchange(api, &u, &uexpected, 9, &changed, ORDER, H2_PAL_ATOMIC_ACQUIRE));
    assert(!changed && uexpected == 7);
    CHECK(h2_pal_atomic_u32_compare_exchange(api, &u, &uexpected, 9, &changed, ORDER, H2_PAL_ATOMIC_ACQUIRE));
    assert(changed);
    CHECK(h2_pal_atomic_u32_fetch_add(api, &u, 3, &uout, ORDER)); assert(uout == 9);
    CHECK(h2_pal_atomic_u32_fetch_sub(api, &u, 2, &uout, ORDER)); assert(uout == 12);
    CHECK(h2_pal_atomic_u32_fetch_or(api, &u, 4, &uout, ORDER)); assert(uout == 10);
    CHECK(h2_pal_atomic_u32_fetch_and(api, &u, 6, &uout, ORDER)); assert(uout == 14);
    CHECK(h2_pal_atomic_u32_load(api, &u, &uout, H2_PAL_ATOMIC_ACQUIRE)); assert(uout == 6);

    CHECK(h2_pal_atomic_i32_load(api, &i, &iout, ORDER)); assert(iout == -3);
    CHECK(h2_pal_atomic_i32_store(api, &i, -5, ORDER));
    CHECK(h2_pal_atomic_i32_exchange(api, &i, 7, &iout, ORDER)); assert(iout == -5);
    iexpected = 6;
    CHECK(h2_pal_atomic_i32_compare_exchange(api, &i, &iexpected, 9, &changed, ORDER, H2_PAL_ATOMIC_RELAXED));
    assert(!changed && iexpected == 7);
    CHECK(h2_pal_atomic_i32_compare_exchange(api, &i, &iexpected, 9, &changed, ORDER, H2_PAL_ATOMIC_RELAXED));
    assert(changed);
    CHECK(h2_pal_atomic_i32_fetch_add(api, &i, 3, &iout, ORDER)); assert(iout == 9);
    CHECK(h2_pal_atomic_i32_fetch_sub(api, &i, 2, &iout, ORDER)); assert(iout == 12);
    CHECK(h2_pal_atomic_i32_fetch_or(api, &i, 4, &iout, ORDER)); assert(iout == 10);
    CHECK(h2_pal_atomic_i32_fetch_and(api, &i, 6, &iout, ORDER)); assert(iout == 14);
    CHECK(h2_pal_atomic_i32_load(api, &i, &iout, ORDER)); assert(iout == 6);

    CHECK(h2_pal_atomic_bool_load(api, &b, &bout, ORDER)); assert(!bout);
    CHECK(h2_pal_atomic_bool_store(api, &b, true, ORDER));
    CHECK(h2_pal_atomic_bool_exchange(api, &b, false, &bout, ORDER)); assert(bout);
    bexpected = true;
    CHECK(h2_pal_atomic_bool_compare_exchange(api, &b, &bexpected, true, &changed, ORDER, H2_PAL_ATOMIC_RELAXED));
    assert(!changed && !bexpected);
    CHECK(h2_pal_atomic_bool_compare_exchange(api, &b, &bexpected, true, &changed, ORDER, H2_PAL_ATOMIC_RELAXED));
    assert(changed);

    CHECK(h2_pal_atomic_ptr_load(api, &p, &pout, ORDER)); assert(pout == NULL);
    CHECK(h2_pal_atomic_ptr_store(api, &p, &first, ORDER));
    CHECK(h2_pal_atomic_ptr_exchange(api, &p, &second, &pout, ORDER)); assert(pout == &first);
    pexpected = &first;
    CHECK(h2_pal_atomic_ptr_compare_exchange(api, &p, &pexpected, &first, &changed, ORDER, H2_PAL_ATOMIC_RELAXED));
    assert(!changed && pexpected == &second);
    CHECK(h2_pal_atomic_ptr_compare_exchange(api, &p, &pexpected, &first, &changed, ORDER, H2_PAL_ATOMIC_RELAXED));
    assert(changed);

    CHECK(h2_pal_atomic_flag_test_and_set(api, &f, &bout, ORDER)); assert(!bout);
    CHECK(h2_pal_atomic_flag_test_and_set(api, &f, &bout, ORDER)); assert(bout);
    CHECK(h2_pal_atomic_flag_clear(api, &f, ORDER));
    CHECK(h2_pal_atomic_flag_test_and_set(api, &f, &bout, ORDER)); assert(!bout);
}

typedef struct worker_args { const h2_pal_atomic_api_t *api; h2_pal_atomic_u32_t *count; } worker_args_t;
static void *worker(void *arg) {
    worker_args_t *args = arg;
    for (unsigned n = 0; n < 10000; ++n) {
        uint32_t old = 0;
        CHECK(h2_pal_atomic_u32_fetch_add(args->api, args->count, 1, &old, H2_PAL_ATOMIC_RELAXED));
    }
    return NULL;
}
static void test_threads(const h2_pal_atomic_api_t *api) {
    h2_pal_atomic_u32_t count = H2_PAL_ATOMIC_U32_INIT(0);
    worker_args_t args = { api, &count };
    pthread_t threads[4];
    for (unsigned n = 0; n < 4; ++n) assert(pthread_create(&threads[n], NULL, worker, &args) == 0);
    for (unsigned n = 0; n < 4; ++n) assert(pthread_join(threads[n], NULL) == 0);
    uint32_t result = 0;
    CHECK(h2_pal_atomic_u32_load(api, &count, &result, ORDER));
    assert(result == 40000);
}
int main(void) {
    const h2_pal_atomic_api_t *api = h2_c11_pal_atomic_api();
    test_dispatch();
    test_all(api);
    test_threads(api);
    api = h2_pal_unsupported_atomic_api();
    test_all(api);
    test_threads(api);
    return 0;
}
