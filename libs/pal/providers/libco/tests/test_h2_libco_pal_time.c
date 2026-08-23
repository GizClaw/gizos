#include "h2_libco_test_support.h"

typedef struct sleep_context {
    const h2_pal_time_api_t *time;
    h2_pal_result_t result;
    unsigned phase;
} sleep_context_t;

static int sleep_task(void *user) {
    sleep_context_t *context = user;
    context->phase = 1u;
    context->result = h2_pal_time_sleep_ms(context->time, 5u);
    context->phase = 2u;
    return 0;
}

static int yield_task(void *user) {
    sleep_context_t *context = user;
    context->phase = 1u;
    context->result = h2_pal_time_sleep_ms(context->time, 0u);
    context->phase = 2u;
    return 0;
}

int main(void) {
    h2_libco_test_env_t env = {.now_ms = 10u};
    h2_libco_t *core = h2_libco_test_create(&env);
    const h2_pal_time_api_t *time = h2_libco_time_api(core);
    uint64_t now = 0u;
    assert(h2_pal_time_get_monotonic_ms(time, &now) == H2_PAL_OK);
    assert(now == 10u);
    assert(h2_pal_time_sleep_ms(time, 1u) == H2_PAL_ERR_INVALID_STATE);
    assert(h2_pal_time_get_wall_ms(time, &now) == H2_PAL_ERR_UNSUPPORTED);

    sleep_context_t finite = {.time = time};
    h2_libco_task_t *finite_task = NULL;
    assert(h2_libco_task_start(core, NULL, sleep_task, &finite,
                               &finite_task) == H2_LIBCO_OK);
    h2_libco_test_schedule(core, 1u);
    assert(finite.phase == 1u);
    env.now_ms = 14u;
    h2_libco_test_schedule(core, 0u);
    env.now_ms = 15u;
    h2_libco_test_schedule(core, 1u);
    assert(finite.phase == 2u && finite.result == H2_PAL_OK);
    assert(h2_libco_task_join(core, finite_task, NULL) == H2_LIBCO_OK);

    sleep_context_t zero = {.time = time};
    h2_libco_task_t *zero_task = NULL;
    assert(h2_libco_task_start(core, NULL, yield_task, &zero, &zero_task) ==
           H2_LIBCO_OK);
    h2_libco_test_schedule(core, 1u);
    assert(zero.phase == 1u);
    h2_libco_test_schedule(core, 1u);
    assert(zero.phase == 2u && zero.result == H2_PAL_OK);
    assert(h2_libco_task_join(core, zero_task, NULL) == H2_LIBCO_OK);

    assert(h2_libco_destroy(&core) == H2_LIBCO_OK);
    assert(env.allocations == 0u);
    return 0;
}
