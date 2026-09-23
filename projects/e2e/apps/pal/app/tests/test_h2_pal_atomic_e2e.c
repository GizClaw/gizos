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
    h2_pal_atomic_u32_t counter = H2_PAL_ATOMIC_U32_INIT(0);
    h2_pal_atomic_flag_t flag = H2_PAL_ATOMIC_FLAG_INIT;
    h2_pal_atomic_e2e_config_t config = {
        .atomic = h2_c11_pal_atomic_api(),
        .counter = &counter,
        .flag = &flag,
        .run_pair = test_run_pair,
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
    return 0;
}
