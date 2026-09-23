#include "h2_pal_atomic_e2e.h"
#include "h2_c11_pal_atomic.h"

#include <assert.h>
#include <pthread.h>

typedef struct test_worker {
    void (*entry)(void *);
    void *argument;
} test_worker_t;

static void *test_worker_run(void *argument) {
    test_worker_t *worker = argument;
    worker->entry(worker->argument);
    return NULL;
}

static h2_pal_result_t test_run_pair(
    void *user, void (*entry)(void *), void *first, void *second) {
    (void)user;
    test_worker_t workers[2] = {
        { .entry = entry, .argument = first },
        { .entry = entry, .argument = second },
    };
    pthread_t threads[2];
    assert(pthread_create(&threads[0], NULL, test_worker_run, &workers[0]) == 0);
    assert(pthread_create(&threads[1], NULL, test_worker_run, &workers[1]) == 0);
    assert(pthread_join(threads[0], NULL) == 0);
    assert(pthread_join(threads[1], NULL) == 0);
    return H2_PAL_OK;
}

int main(void) {
    const h2_pal_atomic_api_t *api = h2_c11_pal_atomic_api();
    _Atomic uint32_t *counter = NULL;
    _Atomic int32_t *signed_counter = NULL;
    atomic_flag *flag = NULL;
    assert(h2_pal_atomic_alloc_u32(api, 0, &counter) == H2_PAL_OK);
    assert(h2_pal_atomic_alloc_i32(api, 0, &signed_counter) == H2_PAL_OK);
    assert(h2_pal_atomic_alloc_flag(api, &flag) == H2_PAL_OK);
    h2_pal_atomic_e2e_config_t config = {
        .counter = counter, .signed_counter = signed_counter,
        .flag = flag, .run_pair = test_run_pair,
    };
    for (int operation = H2_PAL_ATOMIC_E2E_FETCH_ADD;
         operation <= H2_PAL_ATOMIC_E2E_FLAG; ++operation) {
        uint32_t actual = 0;
        assert(h2_pal_atomic_e2e_run_case(&config,
            (h2_pal_atomic_e2e_case_t)operation, 20000, &actual) == H2_PAL_OK);
        assert(actual == 40000);
    }
    assert(h2_pal_atomic_e2e_run_case(&config, H2_PAL_ATOMIC_E2E_FETCH_ADD,
        0, NULL) == H2_PAL_ERR_INVALID_ARG);
    assert(h2_pal_atomic_free(api, counter) == H2_PAL_OK);
    assert(h2_pal_atomic_free(api, signed_counter) == H2_PAL_OK);
    assert(h2_pal_atomic_free(api, flag) == H2_PAL_OK);
    return 0;
}
