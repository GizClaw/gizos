#include "h2_libco.h"
#include "h2_libco_internal.h"

#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct test_allocator {
    size_t calls;
    size_t fail_call;
    size_t live;
    uint64_t now_ms;
} test_allocator_t;

typedef struct test_task {
    h2_libco_t *core;
    int id;
    int calls;
    int result;
    h2_libco_result_t operation_result;
    uintptr_t key;
    uint32_t timeout_ms;
    int *log;
    size_t *log_count;
} test_task_t;

static void *test_alloc(void *user, size_t size) {
    test_allocator_t *allocator = user;
    ++allocator->calls;
    if (allocator->fail_call != 0u &&
        allocator->calls == allocator->fail_call) {
        return NULL;
    }
    void *memory = malloc(size);
    if (memory != NULL) {
        ++allocator->live;
    }
    return memory;
}

static void test_free(void *user, void *memory) {
    test_allocator_t *allocator = user;
    assert(memory != NULL);
    assert(allocator->live > 0u);
    --allocator->live;
    free(memory);
}

static uint64_t test_now_ms(void *user) {
    return ((test_allocator_t *)user)->now_ms;
}

static h2_libco_t *test_core_create(test_allocator_t *allocator) {
    h2_libco_t *core = NULL;
    const h2_libco_config_t config = {
        .user = allocator,
        .alloc = test_alloc,
        .free = test_free,
        .now_ms = test_now_ms,
    };
    assert(h2_libco_create(&config, &core) == H2_LIBCO_OK);
    assert(core != NULL);
    return core;
}

static void test_schedule(h2_libco_t *core, size_t budget, size_t expected) {
    size_t resumed = SIZE_MAX;
    assert(h2_libco_schedule(core, budget, &resumed) == H2_LIBCO_OK);
    assert(resumed == expected);
}

static void assert_core_invariants(const h2_libco_t *core) {
    size_t task_count = 0u;
    size_t ready_count = 0u;
    const h2_libco_task_t *last_ready = NULL;
    for (const h2_libco_task_t *task = core->tasks; task != NULL;
         task = task->all_next) {
        assert(task->owner == core);
        assert((task->state == H2_LIBCO_TASK_READY) == task->queued);
        assert((task->state == H2_LIBCO_TASK_JOINED) ==
               (task->stack == NULL));
        ++task_count;
    }
    for (const h2_libco_task_t *task = core->ready_head; task != NULL;
         task = task->ready_next) {
        assert(task->owner == core && task->queued);
        assert(task->state == H2_LIBCO_TASK_READY);
        last_ready = task;
        ++ready_count;
        assert(ready_count <= task_count);
    }
    assert(last_ready == core->ready_tail);
    assert((core->ready_head == NULL) == (core->ready_tail == NULL));
}

static int return_entry(void *user) {
    test_task_t *task = user;
    ++task->calls;
    return task->result;
}

static int yielding_entry(void *user) {
    test_task_t *task = user;
    for (int step = 0; step < 2; ++step) {
        task->log[(*task->log_count)++] = task->id;
        ++task->calls;
        if (step == 0) {
            task->operation_result = h2_libco_yield(task->core);
            if (task->operation_result != H2_LIBCO_OK) {
                return task->operation_result;
            }
        }
    }
    return task->result;
}

static int waiting_entry(void *user) {
    test_task_t *task = user;
    ++task->calls;
    task->operation_result =
        h2_libco_wait(task->core, task->key, task->timeout_ms);
    ++task->calls;
    if (task->log != NULL) {
        task->log[(*task->log_count)++] = task->id;
    }
    return task->result;
}

static void join_and_destroy(h2_libco_t **core,
                             h2_libco_task_t *task,
                             int expected) {
    int result = 0;
    assert(h2_libco_task_join(*core, task, &result) == H2_LIBCO_OK);
    assert(result == expected);
    assert(h2_libco_destroy(core) == H2_LIBCO_OK);
}

static void test_create_start_return_and_failures(void) {
    test_allocator_t allocator = {0};
    h2_libco_t *core = NULL;
    h2_libco_t *already_set = (h2_libco_t *)(uintptr_t)1u;
    h2_libco_config_t config = {
        .user = &allocator,
        .alloc = test_alloc,
        .free = test_free,
        .now_ms = test_now_ms,
    };

    assert(h2_libco_create(NULL, &core) == H2_LIBCO_ERR_INVALID_ARG);
    assert(h2_libco_create(&config, NULL) == H2_LIBCO_ERR_INVALID_ARG);
    assert(h2_libco_create(&config, &already_set) == H2_LIBCO_ERR_INVALID_ARG);
    config.alloc = NULL;
    assert(h2_libco_create(&config, &core) == H2_LIBCO_ERR_INVALID_ARG);
    config.alloc = test_alloc;
    allocator.fail_call = 1u;
    assert(h2_libco_create(&config, &core) == H2_LIBCO_ERR_NO_MEMORY);
    assert(core == NULL && allocator.live == 0u);

    allocator.fail_call = 0u;
    core = test_core_create(&allocator);
    h2_libco_t *null_core = NULL;
    assert(h2_libco_destroy(NULL) == H2_LIBCO_ERR_INVALID_ARG);
    assert(h2_libco_destroy(&null_core) == H2_LIBCO_ERR_INVALID_ARG);
    assert(h2_libco_schedule(NULL, 1u, NULL) == H2_LIBCO_ERR_INVALID_STATE);
    assert(h2_libco_yield(core) == H2_LIBCO_ERR_INVALID_STATE);
    assert(h2_libco_wait(core, 1u, 1u) == H2_LIBCO_ERR_INVALID_ARG);
    assert(h2_libco_wake(NULL, 1u, 1u, NULL) == H2_LIBCO_ERR_INVALID_ARG);
    test_task_t state = {.core = core, .result = 42};
    h2_libco_task_t *task = NULL;
    assert(h2_libco_task_start(core, NULL, return_entry, &state, &task) ==
           H2_LIBCO_OK);
    assert(state.calls == 0);
    test_schedule(core, 0u, 0u);
    assert(state.calls == 0);
    test_schedule(core, 1u, 1u);
    assert(state.calls == 1);
    assert(h2_libco_destroy(&core) == H2_LIBCO_ERR_BUSY);
    int result = 0;
    assert(h2_libco_task_join(core, task, &result) == H2_LIBCO_OK);
    assert(result == 42);
    assert(h2_libco_task_join(core, task, NULL) ==
           H2_LIBCO_ERR_INVALID_STATE);
    assert(h2_libco_task_cancel(core, task) ==
           H2_LIBCO_ERR_INVALID_STATE);
    assert(h2_libco_destroy(&core) == H2_LIBCO_OK);
    assert(core == NULL && allocator.live == 0u);

    for (size_t failure_offset = 1u; failure_offset <= 2u;
         ++failure_offset) {
        memset(&allocator, 0, sizeof(allocator));
        core = test_core_create(&allocator);
        allocator.fail_call = allocator.calls + failure_offset;
        task = NULL;
        assert(h2_libco_task_start(core, NULL, return_entry, &state, &task) ==
               H2_LIBCO_ERR_NO_MEMORY);
        assert(task == NULL);
        allocator.fail_call = 0u;
        assert(h2_libco_destroy(&core) == H2_LIBCO_OK);
        assert(allocator.live == 0u);
    }

    memset(&allocator, 0, sizeof(allocator));
    core = test_core_create(&allocator);
    h2_libco_task_options_t too_small = {
        .stack_size = H2_LIBCO_MIN_STACK_SIZE - 1u,
    };
    h2_libco_task_options_t unaligned = {
        .stack_size = H2_LIBCO_MIN_STACK_SIZE + 1u,
    };
    h2_libco_task_options_t overflow = {.stack_size = SIZE_MAX};
    task = NULL;
    assert(h2_libco_task_start(core, &too_small, return_entry, &state, &task) ==
           H2_LIBCO_ERR_INVALID_ARG);
    assert(h2_libco_task_start(core, &unaligned, return_entry, &state, &task) ==
           H2_LIBCO_ERR_INVALID_ARG);
    assert(h2_libco_task_start(core, &overflow, return_entry, &state, &task) ==
           H2_LIBCO_ERR_INVALID_ARG);
    assert(h2_libco_task_start(core, NULL, NULL, &state, &task) ==
           H2_LIBCO_ERR_INVALID_ARG);
    assert(h2_libco_task_start(core, NULL, return_entry, &state, NULL) ==
           H2_LIBCO_ERR_INVALID_ARG);
    assert(h2_libco_destroy(&core) == H2_LIBCO_OK);
    assert(allocator.live == 0u);
}

static void test_fifo_yield_and_budget(void) {
    test_allocator_t allocator = {0};
    h2_libco_t *core = test_core_create(&allocator);
    int log[12] = {0};
    size_t log_count = 0u;
    test_task_t states[3] = {0};
    h2_libco_task_t *tasks[3] = {0};
    for (size_t index = 0u; index < 3u; ++index) {
        states[index].core = core;
        states[index].id = (int)index + 1;
        states[index].result = (int)index + 10;
        states[index].log = log;
        states[index].log_count = &log_count;
        assert(h2_libco_task_start(core, NULL, yielding_entry, &states[index],
                                   &tasks[index]) == H2_LIBCO_OK);
    }

    test_schedule(core, 2u, 2u);
    assert(log_count == 2u && log[0] == 1 && log[1] == 2);
    test_schedule(core, 3u, 3u);
    assert(log_count == 5u);
    assert(log[2] == 3 && log[3] == 1 && log[4] == 2);
    test_schedule(core, 3u, 1u);
    assert(log_count == 6u && log[5] == 3);
    for (size_t index = 0u; index < 3u; ++index) {
        int result = 0;
        assert(states[index].calls == 2);
        assert(h2_libco_task_join(core, tasks[index], &result) == H2_LIBCO_OK);
        assert(result == states[index].result);
    }
    assert(h2_libco_destroy(&core) == H2_LIBCO_OK);
    assert(allocator.live == 0u);
}

typedef struct start_child_state {
    h2_libco_t *core;
    h2_libco_task_t *child;
    test_task_t child_state;
    int parent_ran;
} start_child_state_t;

static int start_child_entry(void *user) {
    start_child_state_t *state = user;
    state->parent_ran = 1;
    assert(h2_libco_task_start(state->core, NULL, return_entry,
                               &state->child_state, &state->child) ==
           H2_LIBCO_OK);
    return 7;
}

static void test_new_task_deferred(void) {
    test_allocator_t allocator = {0};
    h2_libco_t *core = test_core_create(&allocator);
    start_child_state_t state = {.core = core};
    state.child_state.core = core;
    state.child_state.result = 8;
    h2_libco_task_t *parent = NULL;
    assert(h2_libco_task_start(core, NULL, start_child_entry, &state, &parent) ==
           H2_LIBCO_OK);
    test_schedule(core, H2_LIBCO_WAKE_ALL, 1u);
    assert(state.parent_ran == 1 && state.child_state.calls == 0);
    test_schedule(core, H2_LIBCO_WAKE_ALL, 1u);
    assert(state.child_state.calls == 1);
    int result;
    assert(h2_libco_task_join(core, parent, &result) == H2_LIBCO_OK &&
           result == 7);
    assert(h2_libco_task_join(core, state.child, &result) == H2_LIBCO_OK &&
           result == 8);
    assert(h2_libco_destroy(&core) == H2_LIBCO_OK);
    assert(allocator.live == 0u);
}

static void test_wait_wake_fifo_and_timeout(void) {
    test_allocator_t allocator = {.now_ms = 100u};
    h2_libco_t *core = test_core_create(&allocator);
    int log[8] = {0};
    size_t log_count = 0u;
    test_task_t states[3] = {0};
    h2_libco_task_t *tasks[3] = {0};
    for (size_t index = 0u; index < 3u; ++index) {
        states[index].core = core;
        states[index].id = (int)index + 1;
        states[index].key = 0x55u;
        states[index].timeout_ms = H2_LIBCO_WAIT_FOREVER;
        states[index].log = log;
        states[index].log_count = &log_count;
        assert(h2_libco_task_start(core, NULL, waiting_entry, &states[index],
                                   &tasks[index]) == H2_LIBCO_OK);
    }
    test_schedule(core, H2_LIBCO_WAKE_ALL, 3u);
    size_t woken = SIZE_MAX;
    assert(h2_libco_wake(core, 0x55u, 0u, &woken) == H2_LIBCO_OK &&
           woken == 0u);
    assert(h2_libco_wake(core, 0x99u, H2_LIBCO_WAKE_ALL, &woken) ==
               H2_LIBCO_OK &&
           woken == 0u);
    assert(h2_libco_wake(core, 0x55u, 1u, &woken) == H2_LIBCO_OK &&
           woken == 1u && log_count == 0u);
    test_schedule(core, H2_LIBCO_WAKE_ALL, 1u);
    assert(log_count == 1u && log[0] == 1);
    assert(h2_libco_wake(core, 0x55u, H2_LIBCO_WAKE_ALL, &woken) ==
               H2_LIBCO_OK &&
           woken == 2u);
    test_schedule(core, H2_LIBCO_WAKE_ALL, 2u);
    assert(log_count == 3u && log[1] == 2 && log[2] == 3);
    for (size_t index = 0u; index < 3u; ++index) {
        assert(states[index].operation_result == H2_LIBCO_WOKEN);
        assert(h2_libco_task_join(core, tasks[index], NULL) == H2_LIBCO_OK);
    }

    test_task_t timeout = {
        .core = core,
        .key = 1u,
        .timeout_ms = 5u,
    };
    h2_libco_task_t *timeout_task = NULL;
    assert(h2_libco_task_start(core, NULL, waiting_entry, &timeout,
                               &timeout_task) == H2_LIBCO_OK);
    test_schedule(core, 1u, 1u);
    allocator.now_ms = 104u;
    test_schedule(core, 1u, 0u);
    allocator.now_ms = 105u;
    test_schedule(core, 1u, 1u);
    assert(timeout.operation_result == H2_LIBCO_ERR_TIMEOUT);
    assert(h2_libco_task_join(core, timeout_task, NULL) == H2_LIBCO_OK);

    test_task_t equal = {
        .core = core,
        .key = 2u,
        .timeout_ms = 5u,
    };
    h2_libco_task_t *equal_task = NULL;
    assert(h2_libco_task_start(core, NULL, waiting_entry, &equal,
                               &equal_task) == H2_LIBCO_OK);
    test_schedule(core, 1u, 1u);
    allocator.now_ms = 110u;
    assert(h2_libco_wake(core, 2u, 1u, &woken) == H2_LIBCO_OK && woken == 1u);
    test_schedule(core, 1u, 1u);
    assert(equal.operation_result == H2_LIBCO_WOKEN);
    assert(h2_libco_task_join(core, equal_task, NULL) == H2_LIBCO_OK);

    assert(h2_libco_wake(core, 3u, 1u, &woken) == H2_LIBCO_OK && woken == 0u);
    test_task_t edge = {
        .core = core,
        .key = 3u,
        .timeout_ms = 1u,
    };
    h2_libco_task_t *edge_task = NULL;
    assert(h2_libco_task_start(core, NULL, waiting_entry, &edge, &edge_task) ==
           H2_LIBCO_OK);
    test_schedule(core, 1u, 1u);
    assert(edge.calls == 1);
    allocator.now_ms = 111u;
    test_schedule(core, 1u, 1u);
    assert(edge.operation_result == H2_LIBCO_ERR_TIMEOUT);
    assert(h2_libco_task_join(core, edge_task, NULL) == H2_LIBCO_OK);

    test_task_t wrap_states[2] = {
        {.core = core,
         .id = 4,
         .key = 4u,
         .timeout_ms = H2_LIBCO_WAIT_FOREVER,
         .log = log,
         .log_count = &log_count},
        {.core = core,
         .id = 5,
         .key = 4u,
         .timeout_ms = H2_LIBCO_WAIT_FOREVER,
         .log = log,
         .log_count = &log_count},
    };
    h2_libco_task_t *wrap_tasks[2] = {0};
    core->next_wait_order = UINT64_MAX - 1u;
    for (size_t index = 0u; index < 2u; ++index) {
        assert(h2_libco_task_start(core, NULL, waiting_entry,
                                   &wrap_states[index], &wrap_tasks[index]) ==
               H2_LIBCO_OK);
    }
    test_schedule(core, 2u, 2u);
    assert(h2_libco_wake(core, 4u, H2_LIBCO_WAKE_ALL, &woken) ==
               H2_LIBCO_OK &&
           woken == 2u);
    test_schedule(core, 2u, 2u);
    assert(log_count == 5u && log[3] == 4 && log[4] == 5);
    for (size_t index = 0u; index < 2u; ++index) {
        assert(h2_libco_task_join(core, wrap_tasks[index], NULL) ==
               H2_LIBCO_OK);
    }

    test_task_t timeout_fifo_states[2] = {
        {.core = core,
         .id = 6,
         .key = 6u,
         .timeout_ms = 2u,
         .log = log,
         .log_count = &log_count},
        {.core = core,
         .id = 7,
         .key = 7u,
         .timeout_ms = 2u,
         .log = log,
         .log_count = &log_count},
    };
    h2_libco_task_t *timeout_fifo_tasks[2] = {0};
    for (size_t index = 0u; index < 2u; ++index) {
        assert(h2_libco_task_start(core, NULL, waiting_entry,
                                   &timeout_fifo_states[index],
                                   &timeout_fifo_tasks[index]) == H2_LIBCO_OK);
    }
    test_schedule(core, 2u, 2u);
    allocator.now_ms += 2u;
    test_schedule(core, 2u, 2u);
    assert(log_count == 7u && log[5] == 6 && log[6] == 7);
    for (size_t index = 0u; index < 2u; ++index) {
        assert(timeout_fifo_states[index].operation_result ==
               H2_LIBCO_ERR_TIMEOUT);
        assert(h2_libco_task_join(core, timeout_fifo_tasks[index], NULL) ==
               H2_LIBCO_OK);
    }

    allocator.now_ms = UINT64_MAX - 2u;
    test_task_t saturated = {.core = core, .key = 5u, .timeout_ms = 5u};
    h2_libco_task_t *saturated_task = NULL;
    assert(h2_libco_task_start(core, NULL, waiting_entry, &saturated,
                               &saturated_task) == H2_LIBCO_OK);
    test_schedule(core, 1u, 1u);
    assert(saturated_task->deadline_ms == UINT64_MAX);
    allocator.now_ms = UINT64_MAX - 1u;
    test_schedule(core, 1u, 0u);
    allocator.now_ms = UINT64_MAX;
    test_schedule(core, 1u, 1u);
    assert(saturated.operation_result == H2_LIBCO_ERR_TIMEOUT);
    assert(h2_libco_task_join(core, saturated_task, NULL) == H2_LIBCO_OK);

    assert(h2_libco_wake(core, 0u, 1u, NULL) == H2_LIBCO_ERR_INVALID_ARG);
    assert(h2_libco_destroy(&core) == H2_LIBCO_OK);
    assert(allocator.live == 0u);
}

static int zero_timeout_entry(void *user) {
    test_task_t *task = user;
    task->operation_result =
        h2_libco_wait(task->core, 0u, H2_LIBCO_WAIT_FOREVER);
    assert(task->operation_result == H2_LIBCO_ERR_INVALID_ARG);
    task->operation_result = h2_libco_wait(task->core, task->key, 0u);
    return 0;
}

static void test_zero_timeout(void) {
    test_allocator_t allocator = {0};
    h2_libco_t *core = test_core_create(&allocator);
    test_task_t state = {.core = core, .key = 7u};
    h2_libco_task_t *task = NULL;
    assert(h2_libco_task_start(core, NULL, zero_timeout_entry, &state, &task) ==
           H2_LIBCO_OK);
    test_schedule(core, 1u, 1u);
    assert(state.operation_result == H2_LIBCO_ERR_TIMEOUT);
    join_and_destroy(&core, task, 0);
    assert(allocator.live == 0u);
}

static int cancel_ready_entry(void *user) {
    test_task_t *task = user;
    ++task->calls;
    task->operation_result = h2_libco_yield(task->core);
    ++task->calls;
    return task->operation_result;
}

static int self_cancel_entry(void *user) {
    test_task_t *task = user;
    task->operation_result = h2_libco_task_cancel(task->core,
                                                  task->core->running);
    ++task->calls;
    assert(task->operation_result == H2_LIBCO_OK);
    task->operation_result = h2_libco_yield(task->core);
    ++task->calls;
    return task->operation_result;
}

static void test_cancellation(void) {
    test_allocator_t allocator = {0};
    h2_libco_t *core = test_core_create(&allocator);
    test_task_t before = {.core = core};
    h2_libco_task_t *before_task = NULL;
    assert(h2_libco_task_start(core, NULL, return_entry, &before,
                               &before_task) == H2_LIBCO_OK);
    assert(h2_libco_task_cancel(core, before_task) == H2_LIBCO_OK);
    test_schedule(core, 1u, 1u);
    assert(before.calls == 0);
    int result = 0;
    assert(h2_libco_task_join(core, before_task, &result) == H2_LIBCO_OK);
    assert(result == H2_LIBCO_ERR_CANCELLED);

    test_task_t ready = {.core = core};
    h2_libco_task_t *ready_task = NULL;
    assert(h2_libco_task_start(core, NULL, cancel_ready_entry, &ready,
                               &ready_task) == H2_LIBCO_OK);
    test_schedule(core, 1u, 1u);
    assert(ready.calls == 1);
    assert(h2_libco_task_cancel(core, ready_task) == H2_LIBCO_OK);
    test_schedule(core, 1u, 1u);
    assert(ready.calls == 2 &&
           ready.operation_result == H2_LIBCO_ERR_CANCELLED);
    assert(h2_libco_task_join(core, ready_task, &result) == H2_LIBCO_OK &&
           result == H2_LIBCO_ERR_CANCELLED);

    test_task_t waiting = {
        .core = core,
        .key = 9u,
        .timeout_ms = H2_LIBCO_WAIT_FOREVER,
    };
    h2_libco_task_t *waiting_task = NULL;
    assert(h2_libco_task_start(core, NULL, waiting_entry, &waiting,
                               &waiting_task) == H2_LIBCO_OK);
    test_schedule(core, 1u, 1u);
    assert(h2_libco_task_cancel(core, waiting_task) == H2_LIBCO_OK);
    test_schedule(core, 1u, 1u);
    assert(waiting.operation_result == H2_LIBCO_ERR_CANCELLED);
    assert(h2_libco_task_join(core, waiting_task, NULL) == H2_LIBCO_OK);

    test_task_t self = {.core = core};
    h2_libco_task_t *self_task = NULL;
    assert(h2_libco_task_start(core, NULL, self_cancel_entry, &self,
                               &self_task) == H2_LIBCO_OK);
    test_schedule(core, 1u, 1u);
    assert(self.calls == 2);
    assert(h2_libco_task_join(core, self_task, &result) == H2_LIBCO_OK &&
           result == H2_LIBCO_ERR_CANCELLED);
    assert(h2_libco_destroy(&core) == H2_LIBCO_OK);
    assert(allocator.live == 0u);
}

typedef struct join_state {
    h2_libco_t *core;
    h2_libco_task_t *self;
    h2_libco_task_t *target;
    h2_libco_result_t join_result;
    int target_result;
} join_state_t;

static int join_entry(void *user) {
    join_state_t *state = user;
    state->join_result = h2_libco_task_join(state->core, state->target,
                                            &state->target_result);
    return 12;
}

static int self_join_entry(void *user) {
    join_state_t *state = user;
    state->join_result =
        h2_libco_task_join(state->core, state->self, NULL);
    return 0;
}

static void test_join_and_instance_isolation(void) {
    test_allocator_t allocator_a = {0};
    test_allocator_t allocator_b = {0};
    h2_libco_t *core_a = test_core_create(&allocator_a);
    h2_libco_t *core_b = test_core_create(&allocator_b);
    int log[2] = {0};
    size_t log_count = 0u;
    test_task_t target_state = {
        .core = core_a,
        .id = 1,
        .result = 33,
        .log = log,
        .log_count = &log_count,
    };
    h2_libco_task_t *target = NULL;
    assert(h2_libco_task_start(core_a, NULL, yielding_entry, &target_state,
                               &target) == H2_LIBCO_OK);
    join_state_t join_state = {.core = core_a, .target = target};
    h2_libco_task_t *joiner = NULL;
    assert(h2_libco_task_start(core_a, NULL, join_entry, &join_state, &joiner) ==
           H2_LIBCO_OK);
    assert(h2_libco_task_join(core_a, target, NULL) == H2_LIBCO_ERR_BUSY);
    assert(h2_libco_task_join(core_b, target, NULL) ==
           H2_LIBCO_ERR_INVALID_ARG);
    assert(h2_libco_task_cancel(core_b, target) == H2_LIBCO_ERR_INVALID_ARG);
    test_schedule(core_a, 1u, 1u);
    test_schedule(core_a, 1u, 1u);
    assert(join_state.join_result == 0);
    test_schedule(core_a, 1u, 1u);
    test_schedule(core_a, 1u, 1u);
    assert(join_state.join_result == H2_LIBCO_OK &&
           join_state.target_result == 33);
    int result = 0;
    assert(h2_libco_task_join(core_a, joiner, &result) == H2_LIBCO_OK &&
           result == 12);
    assert(h2_libco_task_join(core_a, target, NULL) ==
           H2_LIBCO_ERR_INVALID_STATE);

    join_state_t self_state = {.core = core_a};
    h2_libco_task_t *self = NULL;
    self_state.self = self;
    assert(h2_libco_task_start(core_a, NULL, self_join_entry, &self_state,
                               &self) == H2_LIBCO_OK);
    self_state.self = self;
    test_schedule(core_a, 1u, 1u);
    assert(self_state.join_result == H2_LIBCO_ERR_INVALID_STATE);
    assert(h2_libco_task_join(core_a, self, NULL) == H2_LIBCO_OK);

    join_state_t cycle_a = {.core = core_a};
    join_state_t cycle_b = {.core = core_a};
    h2_libco_task_t *cycle_a_task = NULL;
    h2_libco_task_t *cycle_b_task = NULL;
    assert(h2_libco_task_start(core_a, NULL, join_entry, &cycle_a,
                               &cycle_a_task) == H2_LIBCO_OK);
    assert(h2_libco_task_start(core_a, NULL, join_entry, &cycle_b,
                               &cycle_b_task) == H2_LIBCO_OK);
    cycle_a.target = cycle_b_task;
    cycle_b.target = cycle_a_task;
    test_schedule(core_a, 1u, 1u);
    test_schedule(core_a, 1u, 1u);
    assert(cycle_b.join_result == H2_LIBCO_ERR_INVALID_STATE);
    test_schedule(core_a, 1u, 1u);
    assert(cycle_a.join_result == H2_LIBCO_OK);
    assert(h2_libco_task_join(core_a, cycle_a_task, NULL) == H2_LIBCO_OK);
    assert(h2_libco_task_join(core_a, cycle_b_task, NULL) ==
           H2_LIBCO_ERR_INVALID_STATE);

    assert(h2_libco_destroy(&core_a) == H2_LIBCO_OK);
    assert(h2_libco_destroy(&core_b) == H2_LIBCO_OK);
    assert(allocator_a.live == 0u && allocator_b.live == 0u);
}

static void test_instance_wait_isolation(void) {
    test_allocator_t allocator_a = {.now_ms = 10u};
    test_allocator_t allocator_b = {.now_ms = 100u};
    h2_libco_t *core_a = test_core_create(&allocator_a);
    h2_libco_t *core_b = test_core_create(&allocator_b);
    test_task_t state_a = {
        .core = core_a,
        .key = 77u,
        .timeout_ms = 5u,
    };
    test_task_t state_b = {
        .core = core_b,
        .key = 77u,
        .timeout_ms = 7u,
    };
    h2_libco_task_t *task_a = NULL;
    h2_libco_task_t *task_b = NULL;
    assert(h2_libco_task_start(core_a, NULL, waiting_entry, &state_a,
                               &task_a) == H2_LIBCO_OK);
    assert(h2_libco_task_start(core_b, NULL, waiting_entry, &state_b,
                               &task_b) == H2_LIBCO_OK);
    test_schedule(core_a, 1u, 1u);
    test_schedule(core_b, 1u, 1u);

    size_t woken = 0u;
    assert(h2_libco_wake(core_a, 77u, H2_LIBCO_WAKE_ALL, &woken) ==
               H2_LIBCO_OK &&
           woken == 1u);
    test_schedule(core_a, 1u, 1u);
    test_schedule(core_b, 1u, 0u);
    assert(state_a.operation_result == H2_LIBCO_WOKEN && state_b.calls == 1);

    allocator_b.now_ms = 106u;
    test_schedule(core_b, 1u, 0u);
    allocator_b.now_ms = 107u;
    test_schedule(core_b, 1u, 1u);
    assert(state_b.operation_result == H2_LIBCO_ERR_TIMEOUT);
    assert(h2_libco_task_join(core_a, task_a, NULL) == H2_LIBCO_OK);
    assert(h2_libco_task_join(core_b, task_b, NULL) == H2_LIBCO_OK);
    assert(h2_libco_destroy(&core_a) == H2_LIBCO_OK);
    assert(h2_libco_destroy(&core_b) == H2_LIBCO_OK);
    assert(allocator_a.live == 0u && allocator_b.live == 0u);
}

typedef struct destroy_from_task_state {
    h2_libco_t *core;
    h2_libco_result_t result;
} destroy_from_task_state_t;

static int destroy_from_task_entry(void *user) {
    destroy_from_task_state_t *state = user;
    h2_libco_t *local = state->core;
    state->result = h2_libco_destroy(&local);
    assert(local == state->core);
    return 0;
}

static void test_destroy_busy_states(void) {
    test_allocator_t allocator = {0};
    h2_libco_t *core = test_core_create(&allocator);
    test_task_t waiting = {
        .core = core,
        .key = 88u,
        .timeout_ms = H2_LIBCO_WAIT_FOREVER,
    };
    h2_libco_task_t *waiting_task = NULL;
    assert(h2_libco_task_start(core, NULL, waiting_entry, &waiting,
                               &waiting_task) == H2_LIBCO_OK);
    assert(h2_libco_destroy(&core) == H2_LIBCO_ERR_BUSY);
    test_schedule(core, 1u, 1u);
    assert(h2_libco_destroy(&core) == H2_LIBCO_ERR_BUSY);

    destroy_from_task_state_t active = {.core = core};
    h2_libco_task_t *active_task = NULL;
    assert(h2_libco_task_start(core, NULL, destroy_from_task_entry, &active,
                               &active_task) == H2_LIBCO_OK);
    test_schedule(core, 1u, 1u);
    assert(active.result == H2_LIBCO_ERR_INVALID_STATE);
    assert(h2_libco_destroy(&core) == H2_LIBCO_ERR_BUSY);

    assert(h2_libco_task_cancel(core, waiting_task) == H2_LIBCO_OK);
    test_schedule(core, 1u, 1u);
    assert(h2_libco_task_join(core, waiting_task, NULL) == H2_LIBCO_OK);
    assert(h2_libco_task_join(core, active_task, NULL) == H2_LIBCO_OK);
    assert(h2_libco_destroy(&core) == H2_LIBCO_OK);
    assert(allocator.live == 0u);
}

typedef struct wake_deferred_state {
    h2_libco_t *core;
    uintptr_t key;
    size_t woken;
    h2_libco_result_t nested_result;
} wake_deferred_state_t;

static int wake_deferred_entry(void *user) {
    wake_deferred_state_t *state = user;
    state->nested_result = h2_libco_schedule(state->core, 1u, NULL);
    assert(h2_libco_wake(state->core, state->key, H2_LIBCO_WAKE_ALL,
                         &state->woken) == H2_LIBCO_OK);
    return 0;
}

static void test_wake_and_reentrant_schedule_deferred(void) {
    test_allocator_t allocator = {0};
    h2_libco_t *core = test_core_create(&allocator);
    test_task_t waiter = {
        .core = core,
        .key = 41u,
        .timeout_ms = H2_LIBCO_WAIT_FOREVER,
    };
    h2_libco_task_t *waiter_task = NULL;
    assert(h2_libco_task_start(core, NULL, waiting_entry, &waiter,
                               &waiter_task) == H2_LIBCO_OK);
    test_schedule(core, 1u, 1u);
    wake_deferred_state_t waker = {.core = core, .key = 41u};
    h2_libco_task_t *waker_task = NULL;
    assert(h2_libco_task_start(core, NULL, wake_deferred_entry, &waker,
                               &waker_task) == H2_LIBCO_OK);
    test_schedule(core, H2_LIBCO_WAKE_ALL, 1u);
    assert(waker.woken == 1u && waiter.calls == 1);
    assert(waker.nested_result == H2_LIBCO_ERR_INVALID_STATE);
    test_schedule(core, H2_LIBCO_WAKE_ALL, 1u);
    assert(waiter.calls == 2 && waiter.operation_result == H2_LIBCO_WOKEN);
    assert(h2_libco_task_join(core, waker_task, NULL) == H2_LIBCO_OK);
    assert(h2_libco_task_join(core, waiter_task, NULL) == H2_LIBCO_OK);
    assert(h2_libco_destroy(&core) == H2_LIBCO_OK);
    assert(allocator.live == 0u);
}

typedef struct hook_state {
    test_allocator_t allocator;
    h2_libco_t *core;
    uintptr_t wake_key;
    h2_libco_result_t poll_result;
    h2_libco_result_t nested_schedule_result;
    h2_libco_result_t nested_start_result;
    h2_libco_result_t idle_schedule_result;
    h2_libco_result_t idle_wake_result;
    size_t poll_calls;
    size_t idle_calls;
    size_t woken;
    int idle_has_deadline;
    uint64_t idle_deadline_ms;
} hook_state_t;

static h2_libco_result_t test_poll_external(void *user, h2_libco_t *core) {
    hook_state_t *state = user;
    h2_libco_task_t *unexpected_task = NULL;
    assert(core == state->core);
    ++state->poll_calls;
    state->nested_schedule_result = h2_libco_schedule(core, 1u, NULL);
    state->nested_start_result = h2_libco_task_start(
        core, NULL, return_entry, NULL, &unexpected_task);
    assert(unexpected_task == NULL);
    state->woken = 0u;
    if (state->wake_key != 0u) {
        assert(h2_libco_wake(core, state->wake_key, H2_LIBCO_WAKE_ALL,
                             &state->woken) == H2_LIBCO_OK);
    }
    return state->poll_result;
}

static void test_idle(void *user, int has_deadline, uint64_t deadline_ms) {
    hook_state_t *state = user;
    ++state->idle_calls;
    state->idle_has_deadline = has_deadline;
    state->idle_deadline_ms = deadline_ms;
    state->idle_schedule_result = h2_libco_schedule(state->core, 1u, NULL);
    state->idle_wake_result = h2_libco_wake(
        state->core, 91u, H2_LIBCO_WAKE_ALL, NULL);
}

static void test_external_poll_idle_and_reentrancy(void) {
    hook_state_t state = {0};
    h2_libco_t *core = NULL;
    h2_libco_config_t config = {
        .user = &state,
        .alloc = test_alloc,
        .free = test_free,
        .now_ms = test_now_ms,
        .poll_external = test_poll_external,
        .idle = test_idle,
    };
    state.poll_result = H2_LIBCO_OK;
    assert(h2_libco_create(&config, &core) == H2_LIBCO_OK);
    state.core = core;

    test_task_t waiter = {
        .core = core,
        .key = 91u,
        .timeout_ms = H2_LIBCO_WAIT_FOREVER,
    };
    h2_libco_task_t *waiter_task = NULL;
    assert(h2_libco_task_start(core, NULL, waiting_entry, &waiter,
                               &waiter_task) == H2_LIBCO_OK);
    test_schedule(core, 1u, 1u);
    assert(state.poll_calls == 1u && state.idle_calls == 0u);
    assert(state.nested_schedule_result == H2_LIBCO_ERR_INVALID_STATE);
    assert(state.nested_start_result == H2_LIBCO_ERR_INVALID_ARG);

    state.wake_key = 91u;
    test_schedule(core, 1u, 1u);
    assert(state.woken == 1u && waiter.operation_result == H2_LIBCO_WOKEN);
    assert(h2_libco_task_join(core, waiter_task, NULL) == H2_LIBCO_OK);

    state.wake_key = 0u;
    test_schedule(core, 1u, 0u);
    assert(state.idle_calls == 1u && state.idle_has_deadline == 0);
    assert(state.idle_schedule_result == H2_LIBCO_ERR_INVALID_STATE);
    assert(state.idle_wake_result == H2_LIBCO_ERR_INVALID_ARG);

    test_task_t timed = {
        .core = core,
        .key = 92u,
        .timeout_ms = 25u,
    };
    h2_libco_task_t *timed_task = NULL;
    state.allocator.now_ms = 100u;
    assert(h2_libco_task_start(core, NULL, waiting_entry, &timed,
                               &timed_task) == H2_LIBCO_OK);
    test_schedule(core, 1u, 1u);
    test_schedule(core, 1u, 0u);
    assert(state.idle_calls == 2u && state.idle_has_deadline == 1);
    assert(state.idle_deadline_ms == 125u);
    state.allocator.now_ms = 125u;
    test_schedule(core, 1u, 1u);
    assert(timed.operation_result == H2_LIBCO_ERR_TIMEOUT);
    assert(h2_libco_task_join(core, timed_task, NULL) == H2_LIBCO_OK);

    state.poll_result = H2_LIBCO_ERR_EXTERNAL;
    size_t resumed = SIZE_MAX;
    assert(h2_libco_schedule(core, 1u, &resumed) ==
           H2_LIBCO_ERR_EXTERNAL);
    assert(resumed == 0u && state.idle_calls == 2u);
    state.poll_result = H2_LIBCO_OK;
    assert(h2_libco_destroy(&core) == H2_LIBCO_OK);
    assert(state.allocator.live == 0u);
}

typedef struct wrong_executor_state {
    h2_libco_t *core;
    h2_libco_result_t result;
} wrong_executor_state_t;

static void *wrong_executor_thread(void *user) {
    wrong_executor_state_t *state = user;
    state->result = h2_libco_schedule(state->core, 1u, NULL);
    return NULL;
}

static void test_wrong_executor_rejected(void) {
    test_allocator_t allocator = {0};
    h2_libco_t *core = test_core_create(&allocator);
    wrong_executor_state_t state = {.core = core, .result = H2_LIBCO_OK};
    pthread_t thread;
    assert(pthread_create(&thread, NULL, wrong_executor_thread, &state) == 0);
    void *thread_result = (void *)(uintptr_t)1u;
    assert(pthread_join(thread, &thread_result) == 0);
    assert(thread_result == NULL);
    assert(state.result == H2_LIBCO_ERR_INVALID_STATE);
    assert(h2_libco_destroy(&core) == H2_LIBCO_OK);
    assert(allocator.live == 0u);
}

typedef struct stress_state {
    h2_libco_t *core;
    uint32_t switches;
    uint32_t checksum;
    h2_libco_result_t result;
} stress_state_t;

static int stress_entry(void *user) {
    stress_state_t *state = user;
    uint32_t local = UINT32_C(0x12345678);
    for (uint32_t index = 0u; index < 10000u; ++index) {
        local = local * UINT32_C(1664525) + UINT32_C(1013904223);
        ++state->switches;
        state->result = h2_libco_yield(state->core);
        if (state->result != H2_LIBCO_OK) {
            return state->result;
        }
    }
    state->checksum = local;
    return 99;
}

static void test_real_switch_stress_and_guard(void) {
    test_allocator_t allocator = {0};
    h2_libco_t *core = test_core_create(&allocator);
    stress_state_t stress = {.core = core};
    h2_libco_task_options_t options = {.stack_size = 16u * 1024u};
    h2_libco_task_t *task = NULL;
    assert(h2_libco_task_start(core, &options, stress_entry, &stress, &task) ==
           H2_LIBCO_OK);
    for (size_t turn = 0u; turn < 10001u; ++turn) {
        test_schedule(core, 1u, 1u);
    }
    assert(stress.switches == 10000u);
    assert(stress.result == H2_LIBCO_OK);
    assert(stress.checksum == UINT32_C(0xf0d18bc8));
    int result = 0;
    assert(h2_libco_task_join(core, task, &result) == H2_LIBCO_OK &&
           result == 99);

    test_task_t guard_state = {.core = core};
    h2_libco_task_t *guard_task = NULL;
    assert(h2_libco_task_start(core, NULL, return_entry, &guard_state,
                               &guard_task) == H2_LIBCO_OK);
    uint8_t saved = guard_task->stack[-1];
    guard_task->stack[-1] ^= 0xffu;
    assert(h2_libco_schedule(core, 1u, NULL) ==
           H2_LIBCO_ERR_STACK_CORRUPT);
    guard_task->stack[-1] = saved;
    core->faulted = false;
    test_schedule(core, 1u, 1u);
    assert(h2_libco_task_join(core, guard_task, NULL) == H2_LIBCO_OK);
    assert(h2_libco_destroy(&core) == H2_LIBCO_OK);
    assert(allocator.live == 0u);
}

typedef struct storm_task {
    h2_libco_t *core;
    uint32_t seed;
    uint32_t operations;
} storm_task_t;

static uint32_t storm_next(uint32_t *seed) {
    *seed = *seed * UINT32_C(1664525) + UINT32_C(1013904223);
    return *seed;
}

static int storm_entry(void *user) {
    storm_task_t *task = user;
    for (size_t index = 0u; index < 200u; ++index) {
        uint32_t value = storm_next(&task->seed);
        h2_libco_result_t result;
        if ((value & 1u) == 0u) {
            result = h2_libco_yield(task->core);
        } else {
            result = h2_libco_wait(task->core, 1u + value % 4u,
                                   1u + (value >> 8u) % 7u);
        }
        ++task->operations;
        if (result == H2_LIBCO_ERR_CANCELLED) {
            return H2_LIBCO_ERR_CANCELLED;
        }
        assert(result == H2_LIBCO_OK || result == H2_LIBCO_WOKEN ||
               result == H2_LIBCO_ERR_TIMEOUT);
    }
    return 123;
}

static void test_fixed_seed_wake_timeout_cancel_storm(void) {
    test_allocator_t allocator = {0};
    h2_libco_t *core = test_core_create(&allocator);
    storm_task_t states[8] = {0};
    h2_libco_task_t *tasks[8] = {0};
    uint32_t root_seed = UINT32_C(0x7503633);
    for (size_t index = 0u; index < 8u; ++index) {
        states[index].core = core;
        states[index].seed = UINT32_C(0x9e3779b9) ^ (uint32_t)index;
        assert(h2_libco_task_start(core, NULL, storm_entry, &states[index],
                                   &tasks[index]) == H2_LIBCO_OK);
    }
    for (size_t turn = 0u; turn < 3000u; ++turn) {
        uint32_t value = storm_next(&root_seed);
        allocator.now_ms += (value >> 4u) & 1u;
        size_t max_waiters = (value & 3u) == 0u
                                 ? H2_LIBCO_WAKE_ALL
                                 : (size_t)((value >> 2u) & 1u);
        assert(h2_libco_wake(core, 1u + (value >> 8u) % 4u, max_waiters,
                             NULL) == H2_LIBCO_OK);
        assert(h2_libco_schedule(core, (value >> 16u) % 5u, NULL) ==
               H2_LIBCO_OK);
        assert_core_invariants(core);
    }
    for (size_t index = 0u; index < 8u; ++index) {
        h2_libco_result_t cancel_result =
            h2_libco_task_cancel(core, tasks[index]);
        assert(cancel_result == H2_LIBCO_OK ||
               cancel_result == H2_LIBCO_ERR_INVALID_STATE);
    }
    size_t resumed = 0u;
    size_t drain_turns = 0u;
    do {
        assert(h2_libco_schedule(core, H2_LIBCO_WAKE_ALL, &resumed) ==
               H2_LIBCO_OK);
        ++drain_turns;
    } while (resumed != 0u && drain_turns < 16u);
    assert(resumed == 0u);
    for (size_t index = 0u; index < 8u; ++index) {
        int result = 0;
        assert(h2_libco_task_join(core, tasks[index], &result) == H2_LIBCO_OK);
        assert(result == 123 || result == H2_LIBCO_ERR_CANCELLED);
        assert(states[index].operations > 0u);
    }
    assert(h2_libco_destroy(&core) == H2_LIBCO_OK);
    assert(allocator.live == 0u);
}

int main(void) {
    test_create_start_return_and_failures();
    test_fifo_yield_and_budget();
    test_new_task_deferred();
    test_wait_wake_fifo_and_timeout();
    test_zero_timeout();
    test_cancellation();
    test_join_and_instance_isolation();
    test_instance_wait_isolation();
    test_destroy_busy_states();
    test_wake_and_reentrant_schedule_deferred();
    test_external_poll_idle_and_reentrancy();
    test_wrong_executor_rejected();
    test_real_switch_stress_and_guard();
    test_fixed_seed_wake_timeout_cancel_storm();
    puts("h2_libco tests passed");
    return 0;
}
