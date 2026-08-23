#include "h2_bk3633_platform_core.h"
#include "h2_libco.h"

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>

typedef struct test_env {
    h2_libco_t *core;
    uint64_t now_ms;
    size_t live_allocations;
    int fail_next_alloc;
    int calls;
    h2_pal_result_t wait_result;
    h2_pal_mem_api_t allocator;
    uintptr_t resume_order[3];
    size_t resume_count;
} test_env_t;

typedef struct waiter_state {
    test_env_t *env;
    uintptr_t wait_key;
    uint32_t timeout_ms;
    h2_pal_result_t wait_result;
} waiter_state_t;

static void *test_alloc(void *user, size_t size) {
    test_env_t *env = user;
    if (env->fail_next_alloc != 0) {
        env->fail_next_alloc = 0;
        return NULL;
    }
    void *memory = malloc(size);
    if (memory != NULL) {
        ++env->live_allocations;
    }
    return memory;
}

static void test_free(void *user, void *memory) {
    test_env_t *env = user;
    assert(env->live_allocations > 0u);
    --env->live_allocations;
    free(memory);
}

static uint64_t test_now_ms(void *user) {
    return ((test_env_t *)user)->now_ms;
}

static const h2_pal_mem_vtable_t s_mem_vtable = {
    .alloc = test_alloc,
    .free = test_free,
};

static h2_bk3633_platform_libco_result_t bind_executor(
    test_env_t *env, size_t capacity) {
    const h2_bk3633_platform_libco_config_t config = {
        .executor = env->core,
        .allocator = &env->allocator,
        .completion_capacity = capacity,
    };
    return h2_bk3633_platform_libco_bind(&config);
}

static int waiter_entry(void *user) {
    test_env_t *env = user;
    ++env->calls;
    env->wait_result = h2_bk3633_platform_libco_wait(
        0x3633u, H2_LIBCO_WAIT_FOREVER);
    ++env->calls;
    return 17;
}

static int ordered_waiter_entry(void *user) {
    waiter_state_t *state = user;
    state->wait_result = h2_bk3633_platform_libco_wait(
        state->wait_key, state->timeout_ms);
    if (state->env->resume_count < 3u) {
        state->env->resume_order[state->env->resume_count++] =
            state->wait_key;
    }
    return 23;
}

static void test_bind_validation(test_env_t *env) {
    h2_bk3633_platform_libco_config_t config = {
        .executor = env->core,
        .allocator = &env->allocator,
        .completion_capacity = 1u,
    };

    assert(h2_bk3633_platform_libco_bind(NULL) ==
           H2_BK3633_PLATFORM_LIBCO_ERR_INVALID_ARG);
    config.executor = NULL;
    assert(h2_bk3633_platform_libco_bind(&config) ==
           H2_BK3633_PLATFORM_LIBCO_ERR_INVALID_ARG);
    config.executor = env->core;
    config.completion_capacity = 0u;
    assert(h2_bk3633_platform_libco_bind(&config) ==
           H2_BK3633_PLATFORM_LIBCO_ERR_INVALID_ARG);
    config.completion_capacity = SIZE_MAX;
    assert(h2_bk3633_platform_libco_bind(&config) ==
           H2_BK3633_PLATFORM_LIBCO_ERR_INVALID_ARG);
    config.completion_capacity = 1u;
    env->fail_next_alloc = 1;
    assert(h2_bk3633_platform_libco_bind(&config) ==
           H2_BK3633_PLATFORM_LIBCO_ERR_NO_MEMORY);
    assert(bind_executor(env, 1u) == H2_BK3633_PLATFORM_LIBCO_OK);
    assert(bind_executor(env, 1u) ==
           H2_BK3633_PLATFORM_LIBCO_ERR_INVALID_STATE);
    h2_bk3633_platform_libco_unbind();
}

static void test_delivery_coalesces_until_resume(test_env_t *env) {
    h2_libco_task_t *task = NULL;
    size_t count = SIZE_MAX;
    int result = 0;

    assert(bind_executor(env, 1u) == H2_BK3633_PLATFORM_LIBCO_OK);
    assert(h2_libco_task_start(env->core, NULL, waiter_entry, env, &task) ==
           H2_LIBCO_OK);
    assert(h2_libco_schedule(env->core, 1u, &count) == H2_LIBCO_OK &&
           count == 1u && env->calls == 1);
    assert(h2_bk3633_platform_libco_record_completion(0x3633u) ==
           H2_BK3633_PLATFORM_LIBCO_OK);
    assert(h2_bk3633_platform_libco_record_completion(0x3633u) ==
           H2_BK3633_PLATFORM_LIBCO_OK);
    assert(h2_bk3633_platform_libco_dispatch_wakes(1u, &count) ==
               H2_BK3633_PLATFORM_LIBCO_OK &&
           count == 1u && env->calls == 1);

    /* The key remains delivered and coalesced until the task really resumes. */
    assert(h2_bk3633_platform_libco_record_completion(0x3633u) ==
           H2_BK3633_PLATFORM_LIBCO_OK);
    assert(h2_bk3633_platform_libco_record_completion(0x4444u) ==
           H2_BK3633_PLATFORM_LIBCO_ERR_FULL);
    assert(h2_bk3633_platform_libco_dispatch_wakes(0u, &count) ==
               H2_BK3633_PLATFORM_LIBCO_ERR_FULL &&
           count == 0u);
    assert(h2_bk3633_platform_libco_dispatch_wakes(0u, &count) ==
               H2_BK3633_PLATFORM_LIBCO_OK &&
           count == 0u);

    assert(h2_libco_schedule(env->core, 1u, &count) == H2_LIBCO_OK &&
           count == 1u && env->calls == 2);
    assert(env->wait_result == H2_PAL_OK);
    assert(h2_libco_task_join(env->core, task, &result) == H2_LIBCO_OK &&
           result == 17);

    /* Acknowledgement on resume releases the configured slot. */
    assert(h2_bk3633_platform_libco_record_completion(0x4444u) ==
           H2_BK3633_PLATFORM_LIBCO_OK);
    assert(h2_bk3633_platform_libco_dispatch_wakes(1u, &count) ==
               H2_BK3633_PLATFORM_LIBCO_OK &&
           count == 1u);
    h2_bk3633_platform_libco_unbind();
}

static void test_fifo_full_and_recovery(test_env_t *env) {
    static const uintptr_t record_order[3] = {3u, 1u, 2u};
    waiter_state_t waiters[3] = {0};
    h2_libco_task_t *tasks[3] = {0};
    size_t count = SIZE_MAX;
    int result = 0;

    assert(bind_executor(env, 3u) == H2_BK3633_PLATFORM_LIBCO_OK);
    env->resume_count = 0u;
    for (size_t index = 0u; index < 3u; ++index) {
        waiters[index] = (waiter_state_t){
            .env = env,
            .wait_key = index + 1u,
            .timeout_ms = H2_LIBCO_WAIT_FOREVER,
        };
        assert(h2_libco_task_start(env->core, NULL, ordered_waiter_entry,
                                   &waiters[index], &tasks[index]) ==
               H2_LIBCO_OK);
    }
    assert(h2_libco_schedule(env->core, 3u, &count) == H2_LIBCO_OK &&
           count == 3u);
    for (size_t index = 0u; index < 3u; ++index) {
        assert(h2_bk3633_platform_libco_record_completion(
                   record_order[index]) ==
               H2_BK3633_PLATFORM_LIBCO_OK);
    }
    assert(h2_bk3633_platform_libco_record_completion(4u) ==
           H2_BK3633_PLATFORM_LIBCO_ERR_FULL);
    assert(h2_bk3633_platform_libco_has_pending());
    assert(h2_bk3633_platform_libco_dispatch_wakes(2u, &count) ==
               H2_BK3633_PLATFORM_LIBCO_ERR_FULL &&
           count == 2u);
    assert(h2_libco_schedule(env->core, 3u, &count) == H2_LIBCO_OK &&
           count == 2u && env->resume_count == 2u &&
           env->resume_order[0] == 3u && env->resume_order[1] == 1u);

    /* The root-visible fault is one-shot and the rejected key can be retried. */
    assert(h2_bk3633_platform_libco_record_completion(4u) ==
           H2_BK3633_PLATFORM_LIBCO_OK);
    assert(h2_bk3633_platform_libco_dispatch_wakes(SIZE_MAX, &count) ==
               H2_BK3633_PLATFORM_LIBCO_OK &&
           count == 2u);
    assert(h2_libco_schedule(env->core, 3u, &count) == H2_LIBCO_OK &&
           count == 1u && env->resume_count == 3u &&
           env->resume_order[2] == 2u);
    assert(!h2_bk3633_platform_libco_has_pending());
    for (size_t index = 0u; index < 3u; ++index) {
        assert(waiters[index].wait_result == H2_PAL_OK);
        assert(h2_libco_task_join(env->core, tasks[index], &result) ==
                   H2_LIBCO_OK &&
               result == 23);
    }
    h2_bk3633_platform_libco_unbind();
}

static void test_timeout_cancel_and_rebind(test_env_t *env) {
    waiter_state_t timeout_waiter = {
        .env = env,
        .wait_key = 0x100u,
        .timeout_ms = 5u,
    };
    waiter_state_t cancelled_waiter = {
        .env = env,
        .wait_key = 0x200u,
        .timeout_ms = H2_LIBCO_WAIT_FOREVER,
    };
    h2_libco_task_t *timeout_task = NULL;
    h2_libco_task_t *cancelled_task = NULL;
    size_t count = SIZE_MAX;
    int result = 0;

    assert(bind_executor(env, 1u) == H2_BK3633_PLATFORM_LIBCO_OK);
    assert(h2_libco_task_start(env->core, NULL, ordered_waiter_entry,
                               &timeout_waiter, &timeout_task) == H2_LIBCO_OK);
    assert(h2_libco_schedule(env->core, 1u, &count) == H2_LIBCO_OK &&
           count == 1u);
    assert(h2_bk3633_platform_libco_record_completion(
               timeout_waiter.wait_key) == H2_BK3633_PLATFORM_LIBCO_OK);
    env->now_ms += 5u;
    assert(h2_libco_schedule(env->core, 1u, &count) == H2_LIBCO_OK &&
           count == 1u && timeout_waiter.wait_result == H2_PAL_ERR_TIMEOUT);
    assert(h2_libco_task_join(env->core, timeout_task, &result) ==
               H2_LIBCO_OK &&
           result == 23);
    assert(h2_bk3633_platform_libco_dispatch_wakes(1u, &count) ==
               H2_BK3633_PLATFORM_LIBCO_OK &&
           count == 1u);

    assert(h2_libco_task_start(env->core, NULL, ordered_waiter_entry,
                               &cancelled_waiter, &cancelled_task) ==
           H2_LIBCO_OK);
    assert(h2_libco_schedule(env->core, 1u, &count) == H2_LIBCO_OK &&
           count == 1u);
    assert(h2_bk3633_platform_libco_record_completion(
               cancelled_waiter.wait_key) == H2_BK3633_PLATFORM_LIBCO_OK);
    assert(h2_libco_task_cancel(env->core, cancelled_task) == H2_LIBCO_OK);
    assert(h2_libco_schedule(env->core, 1u, &count) == H2_LIBCO_OK &&
           count == 1u && cancelled_waiter.wait_result == H2_PAL_EXIT);
    assert(h2_libco_task_join(env->core, cancelled_task, &result) ==
               H2_LIBCO_OK &&
           result == 23);
    assert(h2_bk3633_platform_libco_dispatch_wakes(1u, &count) ==
               H2_BK3633_PLATFORM_LIBCO_OK &&
           count == 1u);

    assert(h2_bk3633_platform_libco_record_completion(0x300u) ==
           H2_BK3633_PLATFORM_LIBCO_OK);
    h2_bk3633_platform_libco_unbind();
    assert(bind_executor(env, 1u) == H2_BK3633_PLATFORM_LIBCO_OK);
    h2_bk3633_platform_libco_unbind();
}

int main(void) {
    test_env_t env = {0};
    const h2_libco_config_t config = {
        .user = &env,
        .alloc = test_alloc,
        .free = test_free,
        .now_ms = test_now_ms,
    };
    env.allocator = (h2_pal_mem_api_t){
        .user = &env,
        .vtable = &s_mem_vtable,
    };

    assert(h2_bk3633_platform_libco_record_completion(1u) ==
           H2_BK3633_PLATFORM_LIBCO_ERR_INVALID_STATE);
    assert(h2_libco_create(&config, &env.core) == H2_LIBCO_OK);
    test_bind_validation(&env);
    test_delivery_coalesces_until_resume(&env);
    test_fifo_full_and_recovery(&env);
    test_timeout_cancel_and_rebind(&env);
    assert(h2_bk3633_platform_libco_dispatch_wakes(1u, NULL) ==
           H2_BK3633_PLATFORM_LIBCO_ERR_INVALID_STATE);
    assert(h2_libco_destroy(&env.core) == H2_LIBCO_OK);
    assert(env.live_allocations == 0u);
    return 0;
}
