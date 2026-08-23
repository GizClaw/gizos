#include "h2_libco_test_support.h"

typedef struct sync_context {
    h2_libco_t *core;
    const h2_pal_sync_api_t *api;
    h2_pal_mutex_t *mutex;
    h2_pal_semaphore_t *semaphore;
    h2_pal_cond_t *cond;
    uint32_t timeout_ms;
    h2_pal_result_t result;
    bool woke_with_mutex;
    bool finished;
} sync_context_t;

static int semaphore_task(void *user) {
    sync_context_t *context = user;
    context->result = h2_pal_semaphore_take(
        context->api, context->semaphore,
        context->timeout_ms == 0u ? H2_PAL_SYNC_WAIT_FOREVER
                                  : context->timeout_ms);
    return 0;
}

static int condition_task(void *user) {
    sync_context_t *context = user;
    assert(h2_pal_mutex_lock(context->api, context->mutex) == H2_PAL_OK);
    context->result = h2_pal_cond_wait(
        context->api, context->cond, context->mutex,
        context->timeout_ms == 0u ? H2_PAL_SYNC_WAIT_FOREVER
                                  : context->timeout_ms);
    context->woke_with_mutex =
        h2_pal_mutex_try_lock(context->api, context->mutex) == H2_PAL_ERR_BUSY;
    assert(h2_pal_mutex_unlock(context->api, context->mutex) == H2_PAL_OK);
    context->finished = true;
    return 0;
}

static int mutex_holder_task(void *user) {
    sync_context_t *context = user;
    assert(h2_pal_mutex_lock(context->api, context->mutex) == H2_PAL_OK);
    assert(h2_libco_wait(context->core, (uintptr_t)context,
                         H2_LIBCO_WAIT_FOREVER) == H2_LIBCO_WOKEN);
    assert(h2_pal_mutex_unlock(context->api, context->mutex) == H2_PAL_OK);
    return 0;
}

static int recursive_mutex_task(void *user) {
    sync_context_t *context = user;
    assert(h2_pal_mutex_lock(context->api, context->mutex) == H2_PAL_OK);
    assert(h2_pal_mutex_lock(context->api, context->mutex) == H2_PAL_OK);
    assert(h2_pal_mutex_unlock(context->api, context->mutex) == H2_PAL_OK);
    assert(h2_pal_mutex_unlock(context->api, context->mutex) == H2_PAL_OK);
    context->finished = true;
    return 0;
}

int main(void) {
    h2_libco_test_env_t env = {0};
    h2_libco_t *core = h2_libco_test_create(&env);
    const h2_pal_sync_api_t *api = h2_libco_sync_api(core);
    const h2_pal_mem_api_t *mem = h2_libco_test_mem(&env);
    h2_pal_mutex_t *mutex = NULL;
    h2_pal_semaphore_t *semaphore = NULL;
    h2_pal_cond_t *cond = NULL;
    assert(h2_pal_mutex_create(
               api, &(h2_pal_mutex_config_t){.allocator = mem}, &mutex) ==
           H2_PAL_OK);
    assert(h2_pal_semaphore_create(
               api,
               &(h2_pal_semaphore_config_t){
                   .allocator = mem, .initial_count = 0u, .max_count = 1u},
               &semaphore) == H2_PAL_OK);
    assert(h2_pal_cond_create(
               api, &(h2_pal_cond_config_t){.allocator = mem}, &cond) ==
           H2_PAL_OK);
    assert(h2_pal_mutex_lock(api, mutex) == H2_PAL_ERR_INVALID_STATE);

    sync_context_t semaphore_context = {
        .api = api, .semaphore = semaphore,
    };
    h2_libco_task_t *semaphore_waiter = NULL;
    assert(h2_libco_task_start(core, NULL, semaphore_task,
                               &semaphore_context, &semaphore_waiter) ==
           H2_LIBCO_OK);
    h2_libco_test_schedule(core, 1u);
    assert(h2_pal_semaphore_destroy(api, semaphore) == H2_PAL_ERR_BUSY);
    assert(h2_pal_semaphore_give(api, semaphore) == H2_PAL_OK);
    h2_libco_test_schedule(core, 1u);
    assert(semaphore_context.result == H2_PAL_OK);
    assert(h2_libco_task_join(core, semaphore_waiter, NULL) == H2_LIBCO_OK);

    h2_pal_semaphore_t *multi_semaphore = NULL;
    assert(h2_pal_semaphore_create(
               api,
               &(h2_pal_semaphore_config_t){
                   .allocator = mem, .initial_count = 0u, .max_count = 2u},
               &multi_semaphore) == H2_PAL_OK);
    sync_context_t multi_contexts[2] = {
        {.api = api, .semaphore = multi_semaphore,
         .result = H2_PAL_ERR_INVALID_STATE},
        {.api = api, .semaphore = multi_semaphore,
         .result = H2_PAL_ERR_INVALID_STATE},
    };
    h2_libco_task_t *multi_waiters[2] = {NULL, NULL};
    for (size_t i = 0u; i < 2u; ++i) {
        assert(h2_libco_task_start(core, NULL, semaphore_task,
                                   &multi_contexts[i], &multi_waiters[i]) ==
               H2_LIBCO_OK);
    }
    h2_libco_test_schedule(core, 2u);
    assert(h2_pal_semaphore_give(api, multi_semaphore) == H2_PAL_OK);
    assert(h2_pal_semaphore_give(api, multi_semaphore) == H2_PAL_OK);
    assert(h2_pal_semaphore_destroy(api, multi_semaphore) ==
           H2_PAL_ERR_BUSY);
    assert(h2_pal_semaphore_give(api, multi_semaphore) == H2_PAL_ERR_FULL);
    h2_libco_test_schedule(core, 2u);
    for (size_t i = 0u; i < 2u; ++i) {
        assert(multi_contexts[i].result == H2_PAL_OK);
        assert(h2_libco_task_join(core, multi_waiters[i], NULL) ==
               H2_LIBCO_OK);
    }
    assert(h2_pal_semaphore_destroy(api, multi_semaphore) == H2_PAL_OK);

    h2_pal_semaphore_t *cancel_semaphore = NULL;
    assert(h2_pal_semaphore_create(
               api,
               &(h2_pal_semaphore_config_t){
                   .allocator = mem, .initial_count = 0u, .max_count = 1u},
               &cancel_semaphore) == H2_PAL_OK);
    sync_context_t cancel_contexts[2] = {
        {.api = api, .semaphore = cancel_semaphore,
         .result = H2_PAL_ERR_INVALID_STATE},
        {.api = api, .semaphore = cancel_semaphore,
         .result = H2_PAL_ERR_INVALID_STATE},
    };
    h2_libco_task_t *cancel_waiters[2] = {NULL, NULL};
    for (size_t i = 0u; i < 2u; ++i) {
        assert(h2_libco_task_start(core, NULL, semaphore_task,
                                   &cancel_contexts[i], &cancel_waiters[i]) ==
               H2_LIBCO_OK);
    }
    h2_libco_test_schedule(core, 2u);
    assert(h2_pal_semaphore_give(api, cancel_semaphore) == H2_PAL_OK);
    assert(h2_libco_task_cancel(core, cancel_waiters[0]) == H2_LIBCO_OK);
    h2_libco_test_schedule(core, 1u);
    h2_libco_test_schedule(core, 1u);
    assert(cancel_contexts[0].result == H2_PAL_EXIT);
    assert(cancel_contexts[1].result == H2_PAL_OK);
    for (size_t i = 0u; i < 2u; ++i) {
        assert(h2_libco_task_join(core, cancel_waiters[i], NULL) ==
               H2_LIBCO_OK);
    }
    assert(h2_pal_semaphore_destroy(api, cancel_semaphore) == H2_PAL_OK);

    h2_pal_semaphore_t *timeout_semaphore = NULL;
    assert(h2_pal_semaphore_create(
               api,
               &(h2_pal_semaphore_config_t){
                   .allocator = mem, .initial_count = 0u, .max_count = 1u},
               &timeout_semaphore) == H2_PAL_OK);
    sync_context_t semaphore_timeout = {
        .api = api,
        .semaphore = timeout_semaphore,
        .timeout_ms = 5u,
        .result = H2_PAL_ERR_INVALID_STATE,
    };
    h2_libco_task_t *semaphore_timeout_task = NULL;
    assert(h2_libco_task_start(core, NULL, semaphore_task,
                               &semaphore_timeout,
                               &semaphore_timeout_task) == H2_LIBCO_OK);
    h2_libco_test_schedule(core, 1u);
    env.now_ms = 4u;
    h2_libco_test_schedule(core, 0u);
    env.now_ms = 5u;
    h2_libco_test_schedule(core, 1u);
    assert(semaphore_timeout.result == H2_PAL_ERR_TIMEOUT);
    assert(h2_libco_task_join(core, semaphore_timeout_task, NULL) ==
           H2_LIBCO_OK);
    assert(h2_pal_semaphore_destroy(api, timeout_semaphore) == H2_PAL_OK);

    sync_context_t condition_context = {
        .api = api, .mutex = mutex, .cond = cond,
    };
    h2_libco_task_t *condition_waiter = NULL;
    assert(h2_libco_task_start(core, NULL, condition_task,
                               &condition_context, &condition_waiter) ==
           H2_LIBCO_OK);
    h2_libco_test_schedule(core, 1u);
    assert(h2_pal_cond_destroy(api, cond) == H2_PAL_ERR_BUSY);
    assert(h2_pal_cond_signal(api, cond) == H2_PAL_OK);
    h2_libco_test_schedule(core, 1u);
    assert(condition_context.result == H2_PAL_OK);
    assert(condition_context.woke_with_mutex);
    assert(h2_libco_task_join(core, condition_waiter, NULL) == H2_LIBCO_OK);

    sync_context_t broadcast_contexts[2] = {
        {.api = api, .mutex = mutex, .cond = cond,
         .result = H2_PAL_ERR_INVALID_STATE},
        {.api = api, .mutex = mutex, .cond = cond,
         .result = H2_PAL_ERR_INVALID_STATE},
    };
    h2_libco_task_t *broadcast_waiters[2] = {NULL, NULL};
    for (size_t index = 0u; index < 2u; ++index) {
        assert(h2_libco_task_start(core, NULL, condition_task,
                                   &broadcast_contexts[index],
                                   &broadcast_waiters[index]) ==
               H2_LIBCO_OK);
    }
    h2_libco_test_schedule(core, 2u);
    assert(h2_pal_cond_broadcast(api, cond) == H2_PAL_OK);
    h2_libco_test_schedule(core, 2u);
    for (size_t index = 0u; index < 2u; ++index) {
        assert(broadcast_contexts[index].result == H2_PAL_OK);
        assert(broadcast_contexts[index].woke_with_mutex);
        assert(h2_libco_task_join(core, broadcast_waiters[index], NULL) ==
               H2_LIBCO_OK);
    }

    sync_context_t condition_timeout = {
        .api = api,
        .mutex = mutex,
        .cond = cond,
        .timeout_ms = 5u,
        .result = H2_PAL_ERR_INVALID_STATE,
    };
    h2_libco_task_t *condition_timeout_task = NULL;
    assert(h2_libco_task_start(core, NULL, condition_task,
                               &condition_timeout,
                               &condition_timeout_task) == H2_LIBCO_OK);
    h2_libco_test_schedule(core, 1u);
    env.now_ms = 9u;
    h2_libco_test_schedule(core, 0u);
    env.now_ms = 10u;
    h2_libco_test_schedule(core, 1u);
    assert(condition_timeout.result == H2_PAL_ERR_TIMEOUT);
    assert(condition_timeout.woke_with_mutex);
    assert(h2_libco_task_join(core, condition_timeout_task, NULL) ==
           H2_LIBCO_OK);

    sync_context_t cancelled_condition = {
        .core = core,
        .api = api,
        .mutex = mutex,
        .cond = cond,
        .result = H2_PAL_ERR_INVALID_STATE,
    };
    sync_context_t mutex_holder = {
        .core = core,
        .api = api,
        .mutex = mutex,
    };
    h2_libco_task_t *cancelled_waiter = NULL;
    h2_libco_task_t *holder = NULL;
    assert(h2_libco_task_start(core, NULL, condition_task,
                               &cancelled_condition, &cancelled_waiter) ==
           H2_LIBCO_OK);
    h2_libco_test_schedule(core, 1u);
    assert(h2_libco_task_start(core, NULL, mutex_holder_task,
                               &mutex_holder, &holder) == H2_LIBCO_OK);
    h2_libco_test_schedule(core, 1u);
    assert(h2_pal_mutex_destroy(api, mutex) == H2_PAL_ERR_BUSY);
    assert(h2_libco_task_cancel(core, cancelled_waiter) == H2_LIBCO_OK);
    h2_libco_test_schedule(core, 1u);
    assert(!cancelled_condition.finished);
    assert(h2_libco_task_cancel(core, cancelled_waiter) == H2_LIBCO_OK);
    h2_libco_test_schedule(core, 0u);
    assert(!cancelled_condition.finished);
    assert(h2_libco_wake(core, (uintptr_t)&mutex_holder,
                         1u, NULL) == H2_LIBCO_OK);
    h2_libco_test_schedule(core, 1u);
    h2_libco_test_schedule(core, 1u);
    assert(cancelled_condition.result == H2_PAL_EXIT);
    assert(cancelled_condition.woke_with_mutex);
    assert(cancelled_condition.finished);
    assert(h2_libco_task_join(core, holder, NULL) == H2_LIBCO_OK);
    assert(h2_libco_task_join(core, cancelled_waiter, NULL) == H2_LIBCO_OK);

    h2_pal_mutex_t *recursive_mutex = NULL;
    assert(h2_pal_mutex_create(
               api,
               &(h2_pal_mutex_config_t){
                   .allocator = mem, .flags = H2_PAL_MUTEX_FLAG_RECURSIVE},
               &recursive_mutex) == H2_PAL_OK);
    sync_context_t recursive_context = {
        .api = api, .mutex = recursive_mutex,
    };
    h2_libco_task_t *recursive_task = NULL;
    assert(h2_libco_task_start(core, NULL, recursive_mutex_task,
                               &recursive_context, &recursive_task) ==
           H2_LIBCO_OK);
    h2_libco_test_schedule(core, 1u);
    assert(recursive_context.finished);
    assert(h2_libco_task_join(core, recursive_task, NULL) == H2_LIBCO_OK);
    assert(h2_pal_mutex_destroy(api, recursive_mutex) == H2_PAL_OK);

    assert(h2_pal_cond_destroy(api, cond) == H2_PAL_OK);
    assert(h2_pal_semaphore_destroy(api, semaphore) == H2_PAL_OK);
    assert(h2_pal_mutex_destroy(api, mutex) == H2_PAL_OK);
    assert(h2_libco_destroy(&core) == H2_LIBCO_OK);
    assert(env.allocations == 0u);
    return 0;
}
