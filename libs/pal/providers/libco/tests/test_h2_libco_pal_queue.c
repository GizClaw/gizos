#include "h2_libco_test_support.h"

typedef struct queue_call {
    const h2_pal_queue_api_t *api;
    h2_pal_queue_t *queue;
    int value;
    uint32_t timeout_ms;
    h2_pal_result_t result;
} queue_call_t;

static int receive_task(void *user) {
    queue_call_t *call = user;
    call->result = (h2_pal_result_t)h2_pal_queue_recv(
        call->api, call->queue, &call->value, call->timeout_ms);
    return 0;
}

static int send_task(void *user) {
    queue_call_t *call = user;
    call->result = (h2_pal_result_t)h2_pal_queue_send(
        call->api, call->queue, &call->value, call->timeout_ms);
    return 0;
}

static h2_pal_queue_t *create_queue(
    const h2_pal_queue_api_t *api,
    const h2_pal_mem_api_t *mem,
    size_t item_count) {
    const h2_pal_queue_config_t config = {
        .item_size = sizeof(int),
        .item_count = item_count,
        .allocator = mem,
    };
    h2_pal_queue_t *queue = NULL;
    assert(h2_pal_queue_create(api, &config, &queue) == H2_PAL_OK);
    return queue;
}

static h2_libco_task_t *start_call(
    h2_libco_t *core,
    h2_libco_task_entry_fn_t entry,
    queue_call_t *call) {
    h2_libco_task_t *task = NULL;
    call->result = H2_PAL_ERR_INVALID_STATE;
    assert(h2_libco_task_start(core, NULL, entry, call, &task) ==
           H2_LIBCO_OK);
    return task;
}

static void join_call(h2_libco_t *core, h2_libco_task_t *task) {
    assert(h2_libco_task_join(core, task, NULL) == H2_LIBCO_OK);
}

static void test_fifo_latest_and_capacity(
    const h2_pal_queue_api_t *api,
    const h2_pal_mem_api_t *mem) {
    h2_pal_queue_t *queue = create_queue(api, mem, 2u);
    int value = 1;
    int out = 0;
    assert(h2_pal_queue_send(api, queue, &value, H2_PAL_QUEUE_NO_WAIT) ==
           H2_PAL_OK);
    value = 2;
    assert(h2_pal_queue_send(api, queue, &value, H2_PAL_QUEUE_NO_WAIT) ==
           H2_PAL_OK);
    value = 3;
    assert(h2_pal_queue_send(api, queue, &value, H2_PAL_QUEUE_NO_WAIT) ==
           H2_PAL_ERR_TIMEOUT);
    assert(h2_pal_queue_send_latest(api, queue, &value) == H2_PAL_OK);
    assert(h2_pal_queue_recv(api, queue, &out, H2_PAL_QUEUE_NO_WAIT) ==
           H2_PAL_OK && out == 2);
    assert(h2_pal_queue_recv(api, queue, &out, H2_PAL_QUEUE_NO_WAIT) ==
           H2_PAL_OK && out == 3);
    assert(h2_pal_queue_recv(api, queue, &out, H2_PAL_QUEUE_NO_WAIT) ==
           H2_PAL_ERR_TIMEOUT);
    h2_pal_queue_destroy(api, queue);
}

static void test_waiter_order_and_blocked_sender(
    h2_libco_t *core,
    const h2_pal_queue_api_t *api,
    const h2_pal_mem_api_t *mem) {
    h2_pal_queue_t *queue = create_queue(api, mem, 2u);
    queue_call_t receivers[2] = {
        {.api = api, .queue = queue,
         .timeout_ms = H2_PAL_QUEUE_WAIT_FOREVER},
        {.api = api, .queue = queue,
         .timeout_ms = H2_PAL_QUEUE_WAIT_FOREVER},
    };
    h2_libco_task_t *receiver_tasks[2] = {
        start_call(core, receive_task, &receivers[0]),
        start_call(core, receive_task, &receivers[1]),
    };
    h2_libco_test_schedule(core, 2u);
    int value = 11;
    assert(h2_pal_queue_send(api, queue, &value, H2_PAL_QUEUE_NO_WAIT) ==
           H2_PAL_OK);
    value = 12;
    assert(h2_pal_queue_send(api, queue, &value, H2_PAL_QUEUE_NO_WAIT) ==
           H2_PAL_OK);
    h2_libco_test_schedule(core, 2u);
    assert(receivers[0].result == H2_PAL_OK && receivers[0].value == 11);
    assert(receivers[1].result == H2_PAL_OK && receivers[1].value == 12);
    for (size_t index = 0u; index < 2u; ++index) {
        join_call(core, receiver_tasks[index]);
    }

    value = 21;
    assert(h2_pal_queue_send(api, queue, &value, H2_PAL_QUEUE_NO_WAIT) ==
           H2_PAL_OK);
    value = 22;
    assert(h2_pal_queue_send(api, queue, &value, H2_PAL_QUEUE_NO_WAIT) ==
           H2_PAL_OK);
    queue_call_t sender = {
        .api = api,
        .queue = queue,
        .value = 23,
        .timeout_ms = H2_PAL_QUEUE_WAIT_FOREVER,
    };
    h2_libco_task_t *sender_task = start_call(core, send_task, &sender);
    h2_libco_test_schedule(core, 1u);
    int out = 0;
    assert(h2_pal_queue_recv(api, queue, &out, H2_PAL_QUEUE_NO_WAIT) ==
           H2_PAL_OK && out == 21);
    h2_libco_test_schedule(core, 1u);
    assert(sender.result == H2_PAL_OK);
    join_call(core, sender_task);
    assert(h2_pal_queue_recv(api, queue, &out, H2_PAL_QUEUE_NO_WAIT) ==
           H2_PAL_OK && out == 22);
    assert(h2_pal_queue_recv(api, queue, &out, H2_PAL_QUEUE_NO_WAIT) ==
           H2_PAL_OK && out == 23);
    h2_pal_queue_destroy(api, queue);
}

static void test_timeout_cancel_close_reset_and_teardown(
    h2_libco_test_env_t *env,
    h2_libco_t *core,
    const h2_pal_queue_api_t *api,
    const h2_pal_mem_api_t *mem) {
    h2_pal_queue_t *queue = create_queue(api, mem, 1u);
    queue_call_t timeout = {
        .api = api, .queue = queue, .timeout_ms = 5u,
    };
    h2_libco_task_t *timeout_task = start_call(core, receive_task, &timeout);
    h2_libco_test_schedule(core, 1u);
    env->now_ms = 4u;
    h2_libco_test_schedule(core, 0u);
    env->now_ms = 5u;
    h2_libco_test_schedule(core, 1u);
    assert(timeout.result == H2_PAL_ERR_TIMEOUT);
    join_call(core, timeout_task);

    queue_call_t cancelled = {
        .api = api,
        .queue = queue,
        .timeout_ms = H2_PAL_QUEUE_WAIT_FOREVER,
    };
    h2_libco_task_t *cancelled_task =
        start_call(core, receive_task, &cancelled);
    h2_libco_test_schedule(core, 1u);
    size_t allocations_while_waiting = env->allocations;
    h2_pal_queue_destroy(api, queue);
    assert(env->allocations == allocations_while_waiting);
    assert(h2_libco_task_cancel(core, cancelled_task) == H2_LIBCO_OK);
    h2_libco_test_schedule(core, 1u);
    assert(cancelled.result == H2_PAL_EXIT);
    join_call(core, cancelled_task);

    queue_call_t closed[2] = {
        {.api = api, .queue = queue,
         .timeout_ms = H2_PAL_QUEUE_WAIT_FOREVER},
        {.api = api, .queue = queue,
         .timeout_ms = H2_PAL_QUEUE_WAIT_FOREVER},
    };
    h2_libco_task_t *closed_tasks[2] = {
        start_call(core, receive_task, &closed[0]),
        start_call(core, receive_task, &closed[1]),
    };
    h2_libco_test_schedule(core, 2u);
    assert(h2_pal_queue_close(api, queue) == H2_PAL_OK);
    assert(h2_pal_queue_reset(api, queue) == H2_PAL_OK);
    h2_libco_test_schedule(core, 2u);
    for (size_t index = 0u; index < 2u; ++index) {
        assert(closed[index].result == H2_PAL_ERR_CLOSED);
        join_call(core, closed_tasks[index]);
    }

    int value = 31;
    int out = 0;
    assert(h2_pal_queue_send(api, queue, &value, H2_PAL_QUEUE_NO_WAIT) ==
           H2_PAL_OK);
    assert(h2_pal_queue_recv(api, queue, &out, H2_PAL_QUEUE_NO_WAIT) ==
           H2_PAL_OK && out == 31);
    assert(h2_pal_queue_close(api, queue) == H2_PAL_OK);
    assert(h2_pal_queue_send(api, queue, &value, H2_PAL_QUEUE_NO_WAIT) ==
           H2_PAL_ERR_CLOSED);
    h2_pal_queue_destroy(api, queue);
}

int main(void) {
    h2_libco_test_env_t env = {0};
    h2_libco_t *core = h2_libco_test_create(&env);
    const h2_pal_queue_api_t *api = h2_libco_queue_api(core);
    const h2_pal_mem_api_t *mem = h2_libco_test_mem(&env);

    test_fifo_latest_and_capacity(api, mem);
    test_waiter_order_and_blocked_sender(core, api, mem);
    test_timeout_cancel_close_reset_and_teardown(&env, core, api, mem);

    assert(h2_libco_destroy(&core) == H2_LIBCO_OK);
    assert(env.allocations == 0u);
    return 0;
}
