#include "h2_pal_e2e.h"
#include "h2_pal_pref_e2e.h"

#include <stdio.h>
#include <string.h>

#define H2_PAL_E2E_CONCURRENCY_PRODUCERS 3u
#define H2_PAL_E2E_CONCURRENCY_CONSUMERS 3u
#define H2_PAL_E2E_CONCURRENCY_ITEMS_PER_PRODUCER 32u
#define H2_PAL_E2E_CONCURRENCY_TOTAL_ITEMS                              \
  (H2_PAL_E2E_CONCURRENCY_PRODUCERS *                                  \
   H2_PAL_E2E_CONCURRENCY_ITEMS_PER_PRODUCER)
#define H2_PAL_E2E_CONCURRENCY_TASKS                                    \
  (H2_PAL_E2E_CONCURRENCY_PRODUCERS + H2_PAL_E2E_CONCURRENCY_CONSUMERS)
#define H2_PAL_E2E_CONCURRENCY_TIMEOUT_MS 2000u
#define H2_PAL_E2E_CONCURRENCY_SENTINEL UINT16_MAX
#define H2_PAL_E2E_TASK_JOIN_TIMEOUT_MS 2500u

typedef struct h2_pal_e2e_mqtt_events {
  h2_pal_mqtt_str_t topic;
  h2_pal_mqtt_bytes_t payload;
  int connected;
  int subscribe_ack;
  int publish_echo;
  int disconnected;
  h2_pal_result_t subscribe_result;
} h2_pal_e2e_mqtt_events_t;

typedef struct h2_pal_e2e_task_state {
  int ran;
} h2_pal_e2e_task_state_t;

typedef struct h2_pal_e2e_queue_state {
  h2_runtime_t *runtime;
  h2_pal_queue_t *queue;
  int value;
  h2_pal_result_t result;
} h2_pal_e2e_queue_state_t;

typedef struct h2_pal_e2e_condition_state {
  h2_runtime_t *runtime;
  h2_pal_mutex_t *mutex;
  h2_pal_cond_t *condition;
  int waiting;
  int signaled;
  h2_pal_result_t result;
} h2_pal_e2e_condition_state_t;

typedef struct h2_pal_e2e_concurrency_message {
  uint16_t producer;
  uint16_t sequence;
} h2_pal_e2e_concurrency_message_t;

typedef struct h2_pal_e2e_concurrency_state {
  h2_runtime_t *runtime;
  h2_pal_queue_t *queue;
  h2_pal_mutex_t *mutex;
  h2_pal_cond_t *ready_condition;
  h2_pal_cond_t *start_condition;
  size_t ready;
  int released;
  size_t produced;
  size_t consumed;
  uint8_t seen[H2_PAL_E2E_CONCURRENCY_TOTAL_ITEMS];
} h2_pal_e2e_concurrency_state_t;

typedef struct h2_pal_e2e_concurrency_worker {
  h2_pal_e2e_concurrency_state_t *state;
  size_t index;
  h2_pal_result_t result;
} h2_pal_e2e_concurrency_worker_t;

struct h2_pal_e2e_cleanup {
  h2_pal_e2e_concurrency_state_t state;
  h2_pal_e2e_concurrency_worker_t
      workers[H2_PAL_E2E_CONCURRENCY_TASKS];
  h2_pal_task_t *tasks[H2_PAL_E2E_CONCURRENCY_TASKS];
  size_t started;
  int queue_closed;
};

static void h2_pal_e2e_task_entry(void *user) {
  ((h2_pal_e2e_task_state_t *)user)->ran = 1;
}

static void h2_pal_e2e_queue_entry(void *user) {
  h2_pal_e2e_queue_state_t *state = user;
  state->result = (h2_pal_result_t)h2_pal_queue_recv(
      state->runtime->queue, state->queue, &state->value, 1000u);
}

static void h2_pal_e2e_condition_entry(void *user) {
  h2_pal_e2e_condition_state_t *state = user;
  state->result = h2_pal_mutex_lock(state->runtime->sync, state->mutex);
  if (state->result != H2_PAL_OK) return;
  state->waiting = 1;
  while (!state->signaled && state->result == H2_PAL_OK) {
    state->result = h2_pal_cond_wait(
        state->runtime->sync, state->condition, state->mutex, 1000u);
  }
  const h2_pal_result_t unlock =
      h2_pal_mutex_unlock(state->runtime->sync, state->mutex);
  if (state->result == H2_PAL_OK) state->result = unlock;
}

static h2_pal_result_t h2_pal_e2e_concurrency_barrier(
    h2_pal_e2e_concurrency_worker_t *worker) {
  h2_pal_e2e_concurrency_state_t *state = worker->state;
  h2_pal_result_t result =
      h2_pal_mutex_lock(state->runtime->sync, state->mutex);
  if (result != H2_PAL_OK) return result;
  ++state->ready;
  result = h2_pal_cond_signal(state->runtime->sync, state->ready_condition);
  while (result == H2_PAL_OK && !state->released) {
    result = h2_pal_cond_wait(
        state->runtime->sync, state->start_condition, state->mutex,
        H2_PAL_E2E_CONCURRENCY_TIMEOUT_MS);
  }
  const h2_pal_result_t unlock =
      h2_pal_mutex_unlock(state->runtime->sync, state->mutex);
  return result == H2_PAL_OK ? unlock : result;
}

static void h2_pal_e2e_concurrency_producer(void *user) {
  h2_pal_e2e_concurrency_worker_t *worker = user;
  h2_pal_e2e_concurrency_state_t *state = worker->state;
  worker->result = h2_pal_e2e_concurrency_barrier(worker);
  for (size_t sequence = 0u;
       worker->result == H2_PAL_OK &&
       sequence < H2_PAL_E2E_CONCURRENCY_ITEMS_PER_PRODUCER;
       ++sequence) {
    const h2_pal_e2e_concurrency_message_t message = {
        .producer = (uint16_t)worker->index,
        .sequence = (uint16_t)sequence,
    };
    worker->result = (h2_pal_result_t)h2_pal_queue_send(
        state->runtime->queue, state->queue, &message,
        H2_PAL_E2E_CONCURRENCY_TIMEOUT_MS);
    if (worker->result != H2_PAL_OK) break;
    worker->result =
        h2_pal_mutex_lock(state->runtime->sync, state->mutex);
    if (worker->result != H2_PAL_OK) break;
    ++state->produced;
    worker->result =
        h2_pal_mutex_unlock(state->runtime->sync, state->mutex);
  }
}

static void h2_pal_e2e_concurrency_consumer(void *user) {
  h2_pal_e2e_concurrency_worker_t *worker = user;
  h2_pal_e2e_concurrency_state_t *state = worker->state;
  worker->result = h2_pal_e2e_concurrency_barrier(worker);
  while (worker->result == H2_PAL_OK) {
    h2_pal_e2e_concurrency_message_t message = {0};
    worker->result = (h2_pal_result_t)h2_pal_queue_recv(
        state->runtime->queue, state->queue, &message,
        H2_PAL_E2E_CONCURRENCY_TIMEOUT_MS);
    if (worker->result != H2_PAL_OK ||
        message.producer == H2_PAL_E2E_CONCURRENCY_SENTINEL) {
      break;
    }
    if (message.producer >= H2_PAL_E2E_CONCURRENCY_PRODUCERS ||
        message.sequence >= H2_PAL_E2E_CONCURRENCY_ITEMS_PER_PRODUCER) {
      worker->result = H2_PAL_ERR_INVALID_STATE;
      break;
    }
    const size_t item =
        (size_t)message.producer *
            H2_PAL_E2E_CONCURRENCY_ITEMS_PER_PRODUCER +
        message.sequence;
    worker->result =
        h2_pal_mutex_lock(state->runtime->sync, state->mutex);
    if (worker->result != H2_PAL_OK) break;
    if (state->seen[item] != 0u) {
      worker->result = H2_PAL_ERR_INVALID_STATE;
    } else {
      state->seen[item] = 1u;
      ++state->consumed;
    }
    const h2_pal_result_t unlock =
        h2_pal_mutex_unlock(state->runtime->sync, state->mutex);
    if (worker->result == H2_PAL_OK) worker->result = unlock;
  }
}

static h2_pal_result_t h2_pal_e2e_concurrency_join(
    h2_runtime_t *runtime, h2_pal_task_t **task) {
  if (task == NULL || *task == NULL) return H2_PAL_OK;
  uint64_t now = 0u;
  h2_pal_result_t result =
      h2_pal_time_get_monotonic_ms(runtime->time, &now);
  if (result != H2_PAL_OK) return result;
  const uint64_t deadline = UINT64_MAX - now < H2_PAL_E2E_TASK_JOIN_TIMEOUT_MS
      ? UINT64_MAX : now + H2_PAL_E2E_TASK_JOIN_TIMEOUT_MS;
  for (;;) {
    const h2_pal_result_t join = h2_pal_task_join(runtime->task, *task);
    if (join == H2_PAL_OK) {
      *task = NULL;
      return H2_PAL_OK;
    }
    if (join != H2_PAL_ERR_BUSY) return join;
    result = h2_pal_time_get_monotonic_ms(runtime->time, &now);
    if (result != H2_PAL_OK) return result;
    if (now >= deadline) return H2_PAL_ERR_TIMEOUT;
    result = h2_pal_time_sleep_ms(runtime->time, 1u);
    if (result != H2_PAL_OK) return result;
  }
}

static void h2_pal_e2e_timer_callback(void *user, h2_pal_timer_t *timer) {
  (void)timer;
  ++*(int *)user;
}

static void h2_pal_e2e_record(h2_pal_e2e_result_t *result,
                              h2_pal_e2e_case_id_t case_id,
                              h2_pal_result_t case_result) {
  if (result->case_count < H2_PAL_E2E_MAX_CASES) {
    result->cases[result->case_count++] = (h2_pal_e2e_case_result_t){
        .case_id = case_id,
        .result = case_result,
    };
  }
  ++result->selected;
  if (case_result == H2_PAL_OK) {
    ++result->passed;
  } else {
    ++result->failed;
    if (result->result == H2_PAL_OK) {
      result->result = case_result;
    }
  }
}

static void h2_pal_e2e_record_cleanup(h2_pal_e2e_result_t *result,
                                      h2_pal_result_t cleanup) {
  if (cleanup != H2_PAL_OK && result->cleanup_result == H2_PAL_OK) {
    result->cleanup_result = cleanup;
  }
}

static h2_pal_result_t h2_pal_e2e_concurrency_release(
    h2_runtime_t *runtime, h2_pal_e2e_cleanup_t *run,
    h2_pal_e2e_result_t *e2e_result) {
  h2_pal_result_t result = H2_PAL_OK;
  if (run->state.queue != NULL) {
    if (!run->queue_closed) {
      const h2_pal_result_t cleanup = (h2_pal_result_t)h2_pal_queue_close(
          runtime->queue, run->state.queue);
      h2_pal_e2e_record_cleanup(e2e_result, cleanup);
      if (result == H2_PAL_OK) result = cleanup;
    }
    h2_pal_queue_destroy(runtime->queue, run->state.queue);
  }
  if (run->state.start_condition != NULL) {
    const h2_pal_result_t cleanup = h2_pal_cond_destroy(
        runtime->sync, run->state.start_condition);
    h2_pal_e2e_record_cleanup(e2e_result, cleanup);
    if (result == H2_PAL_OK) result = cleanup;
  }
  if (run->state.ready_condition != NULL) {
    const h2_pal_result_t cleanup = h2_pal_cond_destroy(
        runtime->sync, run->state.ready_condition);
    h2_pal_e2e_record_cleanup(e2e_result, cleanup);
    if (result == H2_PAL_OK) result = cleanup;
  }
  if (run->state.mutex != NULL) {
    const h2_pal_result_t cleanup = h2_pal_mutex_destroy(
        runtime->sync, run->state.mutex);
    h2_pal_e2e_record_cleanup(e2e_result, cleanup);
    if (result == H2_PAL_OK) result = cleanup;
  }
  h2_pal_mem_free(runtime->mem, run);
  return result;
}

static h2_pal_result_t h2_pal_e2e_core_time(h2_runtime_t *runtime) {
  uint64_t before = 0u;
  uint64_t after = 0u;
  h2_pal_result_t result =
      h2_pal_time_get_monotonic_ms(runtime->time, &before);
  if (result == H2_PAL_OK) {
    result = h2_pal_time_sleep_ms(runtime->time, 1u);
  }
  if (result == H2_PAL_OK) {
    result = h2_pal_time_get_monotonic_ms(runtime->time, &after);
  }
  return result == H2_PAL_OK && after >= before ? H2_PAL_OK
                                                : H2_PAL_ERR_INVALID_STATE;
}

static h2_pal_result_t h2_pal_e2e_core_timer(
    h2_runtime_t *runtime, h2_pal_e2e_result_t *e2e_result) {
  int calls = 0;
  h2_pal_timer_t *timer = NULL;
  const h2_pal_timer_config_t config = {
      .name = "pal-e2e",
      .period_ms = 1u,
      .flags = H2_PAL_TIMER_FLAG_AUTO_START,
      .cb = h2_pal_e2e_timer_callback,
      .cb_user = &calls,
  };
  h2_pal_result_t result = h2_pal_timer_create(runtime->timer, &config, &timer);
  if (result == H2_PAL_OK) {
    result = h2_pal_time_sleep_ms(runtime->time, 2u);
  }
  h2_pal_result_t cleanup = timer == NULL
      ? H2_PAL_OK : h2_pal_timer_destroy(runtime->timer, timer);
  h2_pal_e2e_record_cleanup(e2e_result, cleanup);
  if (result == H2_PAL_OK && cleanup != H2_PAL_OK) {
    result = cleanup;
  }
  return result == H2_PAL_OK && calls == 1 ? H2_PAL_OK
                                           : H2_PAL_ERR_INVALID_STATE;
}

static h2_pal_result_t h2_pal_e2e_core_task(h2_runtime_t *runtime) {
  h2_pal_e2e_task_state_t state = {0};
  h2_pal_task_t *task = NULL;
  h2_pal_result_t result = h2_pal_task_start(
      runtime->task, NULL, h2_pal_e2e_task_entry, &state, &task);
  if (result == H2_PAL_OK) {
    result = h2_pal_task_join(runtime->task, task);
  }
  return result == H2_PAL_OK && state.ran == 1 ? H2_PAL_OK
                                               : H2_PAL_ERR_INVALID_STATE;
}

static h2_pal_result_t h2_pal_e2e_core_queue(
    h2_runtime_t *runtime, h2_pal_e2e_result_t *e2e_result) {
  h2_pal_queue_t *queue = NULL;
  const h2_pal_queue_config_t config = {
      .name = "pal-e2e",
      .item_size = sizeof(int),
      .item_count = 1u,
      .allocator = runtime->mem,
  };
  h2_pal_result_t result = (h2_pal_result_t)h2_pal_queue_create(
      runtime->queue, &config, &queue);
  h2_pal_e2e_queue_state_t state = {
      .runtime = runtime,
      .queue = queue,
      .result = H2_PAL_ERR_INVALID_STATE,
  };
  h2_pal_task_t *task = NULL;
  if (result == H2_PAL_OK) {
    result = h2_pal_task_start(runtime->task, NULL, h2_pal_e2e_queue_entry,
                               &state, &task);
  }
  if (result == H2_PAL_OK) {
    result = h2_pal_time_sleep_ms(runtime->time, 1u);
  }
  const int sent = 42;
  if (result == H2_PAL_OK) {
    result = (h2_pal_result_t)h2_pal_queue_send(
        runtime->queue, queue, &sent, 1000u);
  }
  if (result == H2_PAL_OK) {
    result = h2_pal_task_join(runtime->task, task);
  }
  if (queue != NULL) {
    const h2_pal_result_t cleanup = (h2_pal_result_t)h2_pal_queue_close(
        runtime->queue, queue);
    h2_pal_e2e_record_cleanup(e2e_result, cleanup);
    if (result == H2_PAL_OK) result = cleanup;
    h2_pal_queue_destroy(runtime->queue, queue);
  }
  return result == H2_PAL_OK && state.result == H2_PAL_OK &&
                 state.value == sent
             ? H2_PAL_OK
             : H2_PAL_ERR_INVALID_STATE;
}

static h2_pal_result_t h2_pal_e2e_core_mutex(
    h2_runtime_t *runtime, h2_pal_e2e_result_t *e2e_result) {
  h2_pal_mutex_t *mutex = NULL;
  const h2_pal_mutex_config_t config = {
      .name = "pal-e2e",
      .allocator = runtime->mem,
      .flags = H2_PAL_MUTEX_FLAG_NONE,
  };
  h2_pal_result_t result = h2_pal_mutex_create(runtime->sync, &config, &mutex);
  if (result == H2_PAL_OK) result = h2_pal_mutex_lock(runtime->sync, mutex);
  if (result == H2_PAL_OK) result = h2_pal_mutex_unlock(runtime->sync, mutex);
  if (mutex != NULL) {
    h2_pal_result_t cleanup = h2_pal_mutex_destroy(runtime->sync, mutex);
    h2_pal_e2e_record_cleanup(e2e_result, cleanup);
    if (result == H2_PAL_OK) result = cleanup;
  }
  return result;
}

static h2_pal_result_t h2_pal_e2e_core_semaphore(
    h2_runtime_t *runtime, h2_pal_e2e_result_t *e2e_result) {
  h2_pal_semaphore_t *semaphore = NULL;
  const h2_pal_semaphore_config_t config = {
      .name = "pal-e2e",
      .allocator = runtime->mem,
      .initial_count = 0u,
      .max_count = 1u,
  };
  h2_pal_result_t result = h2_pal_semaphore_create(
      runtime->sync, &config, &semaphore);
  if (result == H2_PAL_OK) result = h2_pal_semaphore_give(runtime->sync, semaphore);
  if (result == H2_PAL_OK) result = h2_pal_semaphore_take(runtime->sync, semaphore, 0u);
  if (semaphore != NULL) {
    h2_pal_result_t cleanup = h2_pal_semaphore_destroy(runtime->sync, semaphore);
    h2_pal_e2e_record_cleanup(e2e_result, cleanup);
    if (result == H2_PAL_OK) result = cleanup;
  }
  return result;
}

static h2_pal_result_t h2_pal_e2e_core_condition(
    h2_runtime_t *runtime, h2_pal_e2e_result_t *e2e_result) {
  h2_pal_mutex_t *mutex = NULL;
  h2_pal_cond_t *condition = NULL;
  h2_pal_task_t *task = NULL;
  const h2_pal_mutex_config_t mutex_config = {
      .name = "pal-e2e-cond-mutex",
      .allocator = runtime->mem,
      .flags = H2_PAL_MUTEX_FLAG_NONE,
  };
  const h2_pal_cond_config_t condition_config = {
      .name = "pal-e2e-cond",
      .allocator = runtime->mem,
  };
  h2_pal_result_t result =
      h2_pal_mutex_create(runtime->sync, &mutex_config, &mutex);
  if (result == H2_PAL_OK) {
    result = h2_pal_cond_create(runtime->sync, &condition_config, &condition);
  }
  h2_pal_e2e_condition_state_t state = {
      .runtime = runtime,
      .mutex = mutex,
      .condition = condition,
      .result = H2_PAL_ERR_INVALID_STATE,
  };
  if (result == H2_PAL_OK) {
    result = h2_pal_task_start(
        runtime->task, NULL, h2_pal_e2e_condition_entry, &state, &task);
  }
  if (result == H2_PAL_OK) {
    result = h2_pal_time_sleep_ms(runtime->time, 1u);
  }
  if (task != NULL) {
    h2_pal_result_t wake_result =
        h2_pal_mutex_lock(runtime->sync, mutex);
    if (wake_result == H2_PAL_OK) {
      state.signaled = 1;
      wake_result = h2_pal_cond_signal(runtime->sync, condition);
      const h2_pal_result_t unlock =
          h2_pal_mutex_unlock(runtime->sync, mutex);
      if (wake_result == H2_PAL_OK) wake_result = unlock;
    }
    if (result == H2_PAL_OK) result = wake_result;
    const h2_pal_result_t join = h2_pal_task_join(runtime->task, task);
    if (result == H2_PAL_OK) result = join;
    task = NULL;
  }
  if (condition != NULL) {
    const h2_pal_result_t cleanup =
        h2_pal_cond_destroy(runtime->sync, condition);
    h2_pal_e2e_record_cleanup(e2e_result, cleanup);
    if (result == H2_PAL_OK) result = cleanup;
  }
  if (mutex != NULL) {
    const h2_pal_result_t cleanup =
        h2_pal_mutex_destroy(runtime->sync, mutex);
    h2_pal_e2e_record_cleanup(e2e_result, cleanup);
    if (result == H2_PAL_OK) result = cleanup;
  }
  if (result != H2_PAL_OK) return result;
  return task == NULL && state.waiting && state.signaled &&
                 state.result == H2_PAL_OK
             ? H2_PAL_OK
             : H2_PAL_ERR_INVALID_STATE;
}

static h2_pal_result_t h2_pal_e2e_core_concurrency(
    h2_runtime_t *runtime, h2_pal_e2e_result_t *e2e_result) {
  h2_pal_queue_t *queue = NULL;
  h2_pal_mutex_t *mutex = NULL;
  h2_pal_cond_t *ready_condition = NULL;
  h2_pal_cond_t *start_condition = NULL;
  h2_pal_e2e_cleanup_t *run =
      h2_pal_mem_alloc(runtime->mem, sizeof(*run));
  if (run == NULL) return H2_PAL_ERR_NO_MEMORY;
  memset(run, 0, sizeof(*run));
  h2_pal_task_t **tasks = run->tasks;
  h2_pal_e2e_concurrency_worker_t *workers = run->workers;
  const h2_pal_queue_config_t queue_config = {
      .name = "pal-e2e-concurrency",
      .item_size = sizeof(h2_pal_e2e_concurrency_message_t),
      .item_count = 8u,
      .allocator = runtime->mem,
  };
  const h2_pal_mutex_config_t mutex_config = {
      .name = "pal-e2e-concurrency",
      .allocator = runtime->mem,
      .flags = H2_PAL_MUTEX_FLAG_NONE,
  };
  const h2_pal_cond_config_t ready_condition_config = {
      .name = "pal-e2e-concurrency-ready",
      .allocator = runtime->mem,
  };
  const h2_pal_cond_config_t start_condition_config = {
      .name = "pal-e2e-concurrency-start",
      .allocator = runtime->mem,
  };
  h2_pal_result_t result = (h2_pal_result_t)h2_pal_queue_create(
      runtime->queue, &queue_config, &queue);
  if (result == H2_PAL_OK) {
    result = h2_pal_mutex_create(runtime->sync, &mutex_config, &mutex);
  }
  if (result == H2_PAL_OK) {
    result = h2_pal_cond_create(
        runtime->sync, &ready_condition_config, &ready_condition);
  }
  if (result == H2_PAL_OK) {
    result = h2_pal_cond_create(
        runtime->sync, &start_condition_config, &start_condition);
  }
  run->state = (h2_pal_e2e_concurrency_state_t){
      .runtime = runtime,
      .queue = queue,
      .mutex = mutex,
      .ready_condition = ready_condition,
      .start_condition = start_condition,
  };
  h2_pal_e2e_concurrency_state_t *state = &run->state;
  size_t started = 0u;
  while (result == H2_PAL_OK && started < H2_PAL_E2E_CONCURRENCY_TASKS) {
    workers[started] = (h2_pal_e2e_concurrency_worker_t){
        .state = state,
        .index = started < H2_PAL_E2E_CONCURRENCY_CONSUMERS
            ? started
            : started - H2_PAL_E2E_CONCURRENCY_CONSUMERS,
        .result = H2_PAL_ERR_INVALID_STATE,
    };
    const h2_pal_task_entry_t entry =
        started < H2_PAL_E2E_CONCURRENCY_CONSUMERS
            ? h2_pal_e2e_concurrency_consumer
            : h2_pal_e2e_concurrency_producer;
    result = h2_pal_task_start(
        runtime->task, NULL, entry, &workers[started], &tasks[started]);
    if (result == H2_PAL_OK) ++started;
    run->started = started;
  }

  if (started > 0u) {
    h2_pal_result_t release_result =
        h2_pal_mutex_lock(runtime->sync, mutex);
    if (release_result == H2_PAL_OK) {
      while (release_result == H2_PAL_OK && state->ready < started) {
        release_result = h2_pal_cond_wait(
            runtime->sync, ready_condition, mutex,
            H2_PAL_E2E_CONCURRENCY_TIMEOUT_MS);
      }
      state->released = 1;
      const h2_pal_result_t broadcast =
          h2_pal_cond_broadcast(runtime->sync, start_condition);
      if (release_result == H2_PAL_OK) release_result = broadcast;
      const h2_pal_result_t unlock =
          h2_pal_mutex_unlock(runtime->sync, mutex);
      if (release_result == H2_PAL_OK) release_result = unlock;
    }
    if (result == H2_PAL_OK) result = release_result;
  }

  for (size_t index = H2_PAL_E2E_CONCURRENCY_CONSUMERS;
       index < started; ++index) {
    const h2_pal_result_t join = h2_pal_e2e_concurrency_join(
        runtime, &tasks[index]);
    if (result == H2_PAL_OK) result = join;
  }

  h2_pal_result_t sentinel_result = H2_PAL_OK;
  const h2_pal_e2e_concurrency_message_t sentinel = {
      .producer = H2_PAL_E2E_CONCURRENCY_SENTINEL,
  };
  const size_t consumers_started =
      started < H2_PAL_E2E_CONCURRENCY_CONSUMERS
          ? started
          : H2_PAL_E2E_CONCURRENCY_CONSUMERS;
  for (size_t index = 0u;
       sentinel_result == H2_PAL_OK && index < consumers_started; ++index) {
    sentinel_result = (h2_pal_result_t)h2_pal_queue_send(
        runtime->queue, queue, &sentinel,
        H2_PAL_E2E_CONCURRENCY_TIMEOUT_MS);
  }
  if (sentinel_result != H2_PAL_OK && queue != NULL) {
    const h2_pal_result_t close =
        (h2_pal_result_t)h2_pal_queue_close(runtime->queue, queue);
    h2_pal_e2e_record_cleanup(e2e_result, close);
    run->queue_closed = close == H2_PAL_OK;
  }
  if (result == H2_PAL_OK) result = sentinel_result;

  for (size_t index = 0u; index < consumers_started; ++index) {
    const h2_pal_result_t join = h2_pal_e2e_concurrency_join(
        runtime, &tasks[index]);
    if (result == H2_PAL_OK) result = join;
  }
  for (size_t index = 0u; index < started; ++index) {
    if (result == H2_PAL_OK && workers[index].result != H2_PAL_OK) {
      result = workers[index].result;
    }
    if (result == H2_PAL_OK && tasks[index] != NULL) {
      result = H2_PAL_ERR_INVALID_STATE;
    }
  }
  for (size_t index = 0u; index < started; ++index) {
    if (tasks[index] != NULL) {
      const h2_pal_result_t retained = result == H2_PAL_OK
          ? H2_PAL_ERR_BUSY : result;
      h2_pal_e2e_record_cleanup(e2e_result, retained);
      e2e_result->retained_cleanup = run;
      return retained;
    }
  }
  if (result == H2_PAL_OK &&
      (state->ready != H2_PAL_E2E_CONCURRENCY_TASKS ||
       state->produced != H2_PAL_E2E_CONCURRENCY_TOTAL_ITEMS ||
       state->consumed != H2_PAL_E2E_CONCURRENCY_TOTAL_ITEMS)) {
    result = H2_PAL_ERR_INVALID_STATE;
  }
  for (size_t index = 0u;
       result == H2_PAL_OK && index < H2_PAL_E2E_CONCURRENCY_TOTAL_ITEMS;
       ++index) {
    if (state->seen[index] != 1u) result = H2_PAL_ERR_INVALID_STATE;
  }

  const h2_pal_result_t cleanup =
      h2_pal_e2e_concurrency_release(runtime, run, e2e_result);
  if (result == H2_PAL_OK) result = cleanup;
  return result;
}

static h2_pal_result_t h2_pal_e2e_core_unsupported(h2_runtime_t *runtime) {
  return (h2_pal_result_t)h2_pal_fs_mkdir(runtime->fs, "/unsupported") ==
                 H2_PAL_ERR_UNSUPPORTED
             ? H2_PAL_OK
             : H2_PAL_ERR_INVALID_STATE;
}

static void h2_pal_e2e_run_core(h2_runtime_t *runtime,
                                h2_pal_e2e_result_t *result) {
  h2_pal_e2e_record(result, H2_PAL_E2E_CASE_TIME,
                    h2_pal_e2e_core_time(runtime));
  h2_pal_e2e_record(result, H2_PAL_E2E_CASE_TIMER,
                    h2_pal_e2e_core_timer(runtime, result));
  h2_pal_e2e_record(result, H2_PAL_E2E_CASE_TASK,
                    h2_pal_e2e_core_task(runtime));
  h2_pal_e2e_record(result, H2_PAL_E2E_CASE_QUEUE,
                    h2_pal_e2e_core_queue(runtime, result));
  h2_pal_e2e_record(result, H2_PAL_E2E_CASE_MUTEX,
                    h2_pal_e2e_core_mutex(runtime, result));
  h2_pal_e2e_record(result, H2_PAL_E2E_CASE_SEMAPHORE,
                    h2_pal_e2e_core_semaphore(runtime, result));
  h2_pal_e2e_record(result, H2_PAL_E2E_CASE_CONDITION,
                    h2_pal_e2e_core_condition(runtime, result));
  h2_pal_e2e_record(result, H2_PAL_E2E_CASE_CONCURRENCY,
                    h2_pal_e2e_core_concurrency(runtime, result));
  h2_pal_e2e_record(result, H2_PAL_E2E_CASE_UNSUPPORTED,
                    h2_pal_e2e_core_unsupported(runtime));
}

static h2_pal_result_t h2_pal_e2e_host_memory(h2_runtime_t *runtime) {
  uint8_t *memory = h2_pal_mem_alloc(runtime->mem, 17u);
  if (memory == NULL) return H2_PAL_ERR_NO_MEMORY;
  memset(memory, 0x5a, 17u);
  uint8_t *resized = h2_pal_mem_realloc(runtime->mem, memory, 33u);
  if (resized == NULL) {
    h2_pal_mem_free(runtime->mem, memory);
    return H2_PAL_ERR_NO_MEMORY;
  }
  memory = resized;
  for (size_t index = 0u; index < 17u; ++index) {
    if (memory[index] != 0x5au) {
      h2_pal_mem_free(runtime->mem, memory);
      return H2_PAL_ERR_INVALID_STATE;
    }
  }
  h2_pal_mem_free(runtime->mem, memory);
  return H2_PAL_OK;
}

static h2_pal_result_t h2_pal_e2e_host_filesystem(
    h2_runtime_t *runtime) {
  static const char payload[] = "h2-pal-host-e2e";
  h2_pal_result_t result = (h2_pal_result_t)h2_pal_fs_mkdir(
      runtime->fs, "/data/pal-host-e2e");
  h2_pal_fs_file_t *file = NULL;
  if (result == H2_PAL_OK) {
    result = (h2_pal_result_t)h2_pal_fs_open(
        runtime->fs, "/data/pal-host-e2e/value",
        H2_PAL_FS_OPEN_WRITE_TRUNCATE, &file);
  }
  size_t transferred = 0u;
  if (result == H2_PAL_OK) {
    result = (h2_pal_result_t)h2_pal_fs_write(
        runtime->fs, file, payload, sizeof(payload), &transferred);
  }
  if (result == H2_PAL_OK && transferred != sizeof(payload)) {
    result = H2_PAL_ERR_INVALID_STATE;
  }
  if (file != NULL) {
    h2_pal_result_t cleanup = (h2_pal_result_t)h2_pal_fs_close(
        runtime->fs, file);
    if (result == H2_PAL_OK) result = cleanup;
    file = NULL;
  }
  if (result == H2_PAL_OK) {
    result = (h2_pal_result_t)h2_pal_fs_open(
        runtime->fs, "/data/pal-host-e2e/value", H2_PAL_FS_OPEN_READ,
        &file);
  }
  char buffer[sizeof(payload)] = {0};
  transferred = 0u;
  if (result == H2_PAL_OK) {
    result = (h2_pal_result_t)h2_pal_fs_read(
        runtime->fs, file, buffer, sizeof(buffer), &transferred);
  }
  if (result == H2_PAL_OK &&
      (transferred != sizeof(buffer) ||
       memcmp(buffer, payload, sizeof(payload)) != 0)) {
    result = H2_PAL_ERR_INVALID_STATE;
  }
  if (file != NULL) {
    h2_pal_result_t cleanup = (h2_pal_result_t)h2_pal_fs_close(
        runtime->fs, file);
    if (result == H2_PAL_OK) result = cleanup;
  }
  h2_pal_result_t cleanup = (h2_pal_result_t)h2_pal_fs_remove(
      runtime->fs, "/data/pal-host-e2e/value");
  if (result == H2_PAL_OK) result = cleanup;
  cleanup = (h2_pal_result_t)h2_pal_fs_remove(
      runtime->fs, "/data/pal-host-e2e");
  if (result == H2_PAL_OK) result = cleanup;
  h2_pal_fs_stat_t stat_value;
  if (result == H2_PAL_OK &&
      h2_pal_fs_stat(runtime->fs, "/data/../escape", &stat_value) !=
          H2_PAL_ERR_INVALID_ARG) {
    result = H2_PAL_ERR_INVALID_STATE;
  }
  return result;
}

static h2_pal_result_t h2_pal_e2e_host_resolve_sync(
    h2_runtime_t *runtime) {
  h2_pal_net_addr_t address;
  return (h2_pal_result_t)h2_pal_net_resolve_addr(
      runtime->net, "localhost", &address);
}

static h2_pal_result_t h2_pal_e2e_host_resolve_async(
    h2_runtime_t *runtime, uint32_t timeout_ms) {
  h2_pal_net_resolver_t *resolver = NULL;
  h2_pal_result_t result = h2_pal_net_resolve_start(
      runtime->net, "localhost", &resolver);
  h2_pal_net_addr_t address;
  if (result == H2_PAL_OK) {
    result = h2_pal_net_resolve_poll(
        runtime->net, resolver, &address, timeout_ms);
  }
  h2_pal_net_resolve_close(runtime->net, resolver);
  return result;
}

static h2_pal_result_t h2_pal_e2e_host_udp(
    h2_runtime_t *runtime, h2_pal_net_family_t family,
    uint32_t timeout_ms) {
  h2_pal_net_socket_t socket_token = -1;
  h2_pal_net_addr_t bound;
  memset(&bound, 0, sizeof(bound));
  h2_pal_result_t result = (h2_pal_result_t)h2_pal_net_udp_open_bound(
      runtime->net, family, 0u, NULL, &socket_token, &bound);
  h2_pal_net_addr_t destination;
  memset(&destination, 0, sizeof(destination));
  destination.family = family;
  destination.port = bound.port;
  if (family == H2_PAL_NET_FAMILY_IPV4) {
    destination.ip[0] = 127u;
    destination.ip[3] = 1u;
  } else {
    destination.ip[15] = 1u;
  }
  static const uint8_t payload[] = "h2-pal-udp";
  if (result == H2_PAL_OK) {
    int sent = h2_pal_net_udp_sendto(runtime->net, socket_token,
                                     &destination, payload, sizeof(payload));
    result = sent == (int)sizeof(payload) ? H2_PAL_OK
                                           : H2_PAL_ERR_INVALID_STATE;
  }
  uint8_t buffer[sizeof(payload)] = {0};
  h2_pal_net_addr_t peer;
  if (result == H2_PAL_OK) {
    int received = h2_pal_net_udp_recvfrom(
        runtime->net, socket_token, &peer, buffer, sizeof(buffer), timeout_ms);
    result = received == (int)sizeof(buffer) &&
                     memcmp(buffer, payload, sizeof(payload)) == 0
                 ? H2_PAL_OK
                 : received < 0 ? (h2_pal_result_t)received
                                : H2_PAL_ERR_INVALID_STATE;
  }
  h2_pal_net_close(runtime->net, socket_token);
  return result;
}

static h2_pal_result_t h2_pal_e2e_host_remaining(
    const h2_runtime_t *runtime, uint64_t deadline_ms,
    uint32_t *out_remaining_ms) {
  uint64_t now_ms = 0u;
  h2_pal_result_t result =
      h2_pal_time_get_monotonic_ms(runtime->time, &now_ms);
  if (result != H2_PAL_OK) return result;
  if (h2_pal_time_deadline_expired(now_ms, deadline_ms)) {
    return H2_PAL_ERR_TIMEOUT;
  }
  const uint64_t remaining_ms = deadline_ms - now_ms;
  *out_remaining_ms = remaining_ms > UINT32_MAX
                          ? UINT32_MAX
                          : (uint32_t)remaining_ms;
  return H2_PAL_OK;
}

static h2_pal_result_t h2_pal_e2e_host_send_all(
    h2_runtime_t *runtime, h2_pal_net_socket_t socket_token,
    const uint8_t *data, size_t data_len, uint64_t deadline_ms) {
  size_t offset = 0u;
  while (offset < data_len) {
    uint32_t remaining_ms = 0u;
    h2_pal_result_t result = h2_pal_e2e_host_remaining(
        runtime, deadline_ms, &remaining_ms);
    if (result != H2_PAL_OK) return result;
    const int sent = h2_pal_net_tcp_send_timeout(
        runtime->net, socket_token, data + offset, data_len - offset,
        remaining_ms);
    if (sent <= 0) {
      return sent < 0 ? (h2_pal_result_t)sent : H2_PAL_ERR_INVALID_STATE;
    }
    offset += (size_t)sent;
  }
  return H2_PAL_OK;
}

static h2_pal_result_t h2_pal_e2e_host_receive_all(
    h2_runtime_t *runtime, h2_pal_net_socket_t socket_token, uint8_t *data,
    size_t data_len, uint64_t deadline_ms) {
  size_t offset = 0u;
  while (offset < data_len) {
    uint32_t remaining_ms = 0u;
    h2_pal_result_t result = h2_pal_e2e_host_remaining(
        runtime, deadline_ms, &remaining_ms);
    if (result != H2_PAL_OK) return result;
    const int received = h2_pal_net_tcp_recv(
        runtime->net, socket_token, data + offset, data_len - offset,
        remaining_ms);
    if (received <= 0) {
      return received < 0 ? (h2_pal_result_t)received
                          : H2_PAL_ERR_INVALID_STATE;
    }
    offset += (size_t)received;
  }
  return H2_PAL_OK;
}

static h2_pal_result_t h2_pal_e2e_host_tcp(
    h2_runtime_t *runtime, uint16_t port, uint32_t timeout_ms) {
  if (port == 0u) return H2_PAL_ERR_INVALID_ARG;
  uint64_t now_ms = 0u;
  h2_pal_result_t result =
      h2_pal_time_get_monotonic_ms(runtime->time, &now_ms);
  const uint64_t deadline_ms =
      h2_pal_time_deadline_ms(now_ms, timeout_ms);
  h2_pal_net_socket_t socket_token = -1;
  if (result == H2_PAL_OK) {
    result = (h2_pal_result_t)h2_pal_net_tcp_open_bound(
        runtime->net, H2_PAL_NET_FAMILY_IPV4, NULL, &socket_token);
  }
  h2_pal_net_addr_t destination = {
      .family = H2_PAL_NET_FAMILY_IPV4,
      .port = port,
      .ip = {127u, 0u, 0u, 1u},
  };
  if (result == H2_PAL_OK) {
    uint32_t remaining_ms = 0u;
    result = h2_pal_e2e_host_remaining(
        runtime, deadline_ms, &remaining_ms);
    if (result == H2_PAL_OK) {
      result = h2_pal_net_tcp_connect(
          runtime->net, socket_token, &destination, remaining_ms);
    }
  }
  static const uint8_t payload[] = "h2-pal-tcp";
  if (result == H2_PAL_OK) {
    result = h2_pal_e2e_host_send_all(
        runtime, socket_token, payload, sizeof(payload), deadline_ms);
  }
  uint8_t buffer[sizeof(payload)] = {0};
  if (result == H2_PAL_OK) {
    result = h2_pal_e2e_host_receive_all(
        runtime, socket_token, buffer, sizeof(buffer), deadline_ms);
    if (result == H2_PAL_OK &&
        memcmp(buffer, payload, sizeof(payload)) != 0) {
      result = H2_PAL_ERR_INVALID_STATE;
    }
  }
  h2_pal_net_close(runtime->net, socket_token);
  return result;
}

static h2_pal_result_t h2_pal_e2e_host_tls(
    h2_runtime_t *runtime, uint16_t port, const uint8_t *root_ca_pem,
    size_t root_ca_pem_len, uint32_t timeout_ms, int expect_verify_failure) {
  if (runtime->net == NULL || port == 0u || root_ca_pem == NULL ||
      root_ca_pem_len == 0u) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  uint64_t now_ms = 0u;
  h2_pal_result_t result =
      h2_pal_time_get_monotonic_ms(runtime->time, &now_ms);
  const uint64_t deadline_ms =
      h2_pal_time_deadline_ms(now_ms, timeout_ms);
  h2_pal_net_socket_t socket_token = -1;
  if (result == H2_PAL_OK) {
    result = (h2_pal_result_t)h2_pal_net_tcp_open_bound(
        runtime->net, H2_PAL_NET_FAMILY_IPV4, NULL, &socket_token);
  }
  h2_pal_net_addr_t destination = {
      .family = H2_PAL_NET_FAMILY_IPV4,
      .port = port,
      .ip = {127u, 0u, 0u, 1u},
  };
  if (result == H2_PAL_OK) {
    uint32_t remaining_ms = 0u;
    result = h2_pal_e2e_host_remaining(
        runtime, deadline_ms, &remaining_ms);
    if (result == H2_PAL_OK) {
      result = h2_pal_net_tcp_connect(
          runtime->net, socket_token, &destination, remaining_ms);
    }
  }
  h2_pal_net_tls_config_t tls = {
      .server_name = "localhost",
      .root_ca_pem = root_ca_pem,
      .root_ca_pem_len = root_ca_pem_len,
      .verify = H2_PAL_NET_TLS_VERIFY_REQUIRED,
  };
  h2_pal_net_socket_t wrapped_token = -1;
  if (result == H2_PAL_OK) {
    uint32_t remaining_ms = 0u;
    result = h2_pal_e2e_host_remaining(
        runtime, deadline_ms, &remaining_ms);
    if (result == H2_PAL_OK) {
      result = h2_pal_net_tls_wrap(
          runtime->net, socket_token, &tls, remaining_ms, &wrapped_token);
    }
  }
  if (expect_verify_failure) {
    h2_pal_net_close(runtime->net, socket_token);
    return result == H2_PAL_ERR_TLS_VERIFY
               ? H2_PAL_OK
               : result == H2_PAL_OK ? H2_PAL_ERR_INVALID_STATE : result;
  }
  if (result == H2_PAL_OK && wrapped_token != socket_token) {
    result = H2_PAL_ERR_INVALID_STATE;
  }
  static const uint8_t request[] = "ping";
  if (result == H2_PAL_OK) {
    result = h2_pal_e2e_host_send_all(
        runtime, socket_token, request, sizeof(request), deadline_ms);
  }
  static const uint8_t response[] = "ok";
  uint8_t buffer[sizeof(response)] = {0};
  if (result == H2_PAL_OK) {
    result = h2_pal_e2e_host_receive_all(
        runtime, socket_token, buffer, sizeof(buffer), deadline_ms);
    if (result == H2_PAL_OK &&
        memcmp(buffer, response, sizeof(response)) != 0) {
      result = H2_PAL_ERR_INVALID_STATE;
    }
  }
  h2_pal_net_close(runtime->net, socket_token);
  return result;
}

static h2_pal_result_t h2_pal_e2e_host_https(
    h2_runtime_t *runtime, uint16_t port, uint32_t timeout_ms) {
  if (runtime->http == NULL || port == 0u) return H2_PAL_ERR_INVALID_ARG;
  char url[128];
  int url_len = snprintf(url, sizeof(url),
                         "https://localhost:%u/pal-host-e2e", port);
  if (url_len <= 0 || (size_t)url_len >= sizeof(url)) {
    return H2_PAL_ERR_NO_SPACE;
  }
  uint8_t body[16] = {0};
  h2_pal_http_request_t request = {
      .method = H2_PAL_HTTP_GET,
      .url = {.data = url, .len = (size_t)url_len},
      .timeout_ms = (int)timeout_ms,
      .response_buf = body,
      .response_buf_cap = sizeof(body),
  };
  h2_pal_http_response_t response;
  h2_pal_http_response_reset(&response);
  h2_pal_result_t result = (h2_pal_result_t)h2_pal_http_request(
      runtime->http, &request, &response);
  if (result == H2_PAL_OK &&
      (response.status_code != 200 || response.body_len != 8u ||
       memcmp(response.body, "host-e2e", 8u) != 0)) {
    result = H2_PAL_ERR_INVALID_STATE;
  }
  h2_pal_http_response_free(runtime->http, &response);
  return result;
}

static h2_pal_result_t h2_pal_e2e_run_mqtt(
    h2_runtime_t *runtime, const h2_pal_e2e_config_t *config,
    h2_pal_e2e_result_t *out_result);

static int h2_pal_e2e_host_netif_callback(
    void *user, const h2_pal_netif_ref_t *ref,
    const h2_pal_netif_status_t *status) {
  size_t *count = user;
  if (ref != NULL && status != NULL) ++*count;
  return 0;
}

static h2_pal_result_t h2_pal_e2e_host_netif(h2_runtime_t *runtime) {
  size_t count = 0u;
  h2_pal_result_t result = h2_pal_netif_list(
      runtime->netif, NULL, h2_pal_e2e_host_netif_callback, &count);
  return result != H2_PAL_OK
             ? result
             : count != 0u ? H2_PAL_OK : H2_PAL_ERR_INVALID_STATE;
}

static int h2_pal_e2e_host_event_handler(
    void *user, const h2_pal_system_event_t *event) {
  int *calls = user;
  if (event != NULL &&
      event->type == H2_PAL_SYSTEM_EVENT_TYPE_NETIF_DEFAULT_CHANGED) {
    ++*calls;
  }
  return 0;
}

static h2_pal_result_t h2_pal_e2e_host_system_event(
    h2_runtime_t *runtime) {
  int calls = 0;
  h2_pal_system_event_subscription_t *subscription = NULL;
  h2_pal_result_t result = (h2_pal_result_t)h2_pal_system_event_subscribe(
      runtime->system_event,
      H2_PAL_SYSTEM_EVENT_TYPE_NETIF_DEFAULT_CHANGED,
      h2_pal_e2e_host_event_handler, &calls, &subscription);
  h2_pal_system_event_t event = {
      .type = H2_PAL_SYSTEM_EVENT_TYPE_NETIF_DEFAULT_CHANGED,
  };
  if (result == H2_PAL_OK) {
    result = (h2_pal_result_t)h2_pal_system_event_post(
        runtime->system_event, &event, 0u);
  }
  h2_pal_system_event_unsubscribe(runtime->system_event, subscription);
  return result == H2_PAL_OK && calls == 1 ? H2_PAL_OK
                                           : H2_PAL_ERR_INVALID_STATE;
}

static void h2_pal_e2e_run_host(h2_runtime_t *runtime,
                                const h2_pal_e2e_config_t *config,
                                h2_pal_e2e_result_t *result) {
  uint32_t timeout_ms = config->host.timeout_ms == 0u
                            ? 2000u : config->host.timeout_ms;
  h2_pal_e2e_record(result, H2_PAL_E2E_CASE_HOST_MEMORY,
                    h2_pal_e2e_host_memory(runtime));
  h2_pal_e2e_record(result, H2_PAL_E2E_CASE_TIME,
                    h2_pal_e2e_core_time(runtime));
  h2_pal_e2e_record(result, H2_PAL_E2E_CASE_TASK,
                    h2_pal_e2e_core_task(runtime));
  h2_pal_e2e_record(result, H2_PAL_E2E_CASE_QUEUE,
                    h2_pal_e2e_core_queue(runtime, result));
  h2_pal_e2e_record(result, H2_PAL_E2E_CASE_MUTEX,
                    h2_pal_e2e_core_mutex(runtime, result));
  h2_pal_e2e_record(result, H2_PAL_E2E_CASE_SEMAPHORE,
                    h2_pal_e2e_core_semaphore(runtime, result));
  h2_pal_e2e_record(result, H2_PAL_E2E_CASE_CONDITION,
                    h2_pal_e2e_core_condition(runtime, result));
  h2_pal_e2e_record(result, H2_PAL_E2E_CASE_HOST_FILESYSTEM,
                    h2_pal_e2e_host_filesystem(runtime));
  h2_pal_e2e_record(result, H2_PAL_E2E_CASE_HOST_RESOLVE_SYNC,
                    h2_pal_e2e_host_resolve_sync(runtime));
  h2_pal_e2e_record(result, H2_PAL_E2E_CASE_HOST_RESOLVE_ASYNC,
                    h2_pal_e2e_host_resolve_async(runtime, timeout_ms));
  h2_pal_e2e_record(result, H2_PAL_E2E_CASE_HOST_UDP_IPV4,
                    h2_pal_e2e_host_udp(runtime, H2_PAL_NET_FAMILY_IPV4,
                                        timeout_ms));
  h2_pal_e2e_record(result, H2_PAL_E2E_CASE_HOST_UDP_IPV6,
                    h2_pal_e2e_host_udp(runtime, H2_PAL_NET_FAMILY_IPV6,
                                        timeout_ms));
  h2_pal_e2e_record(result, H2_PAL_E2E_CASE_HOST_TCP,
                    h2_pal_e2e_host_tcp(runtime,
                                        config->host.tcp_echo_port,
                                        timeout_ms));
  h2_pal_e2e_record(
      result, H2_PAL_E2E_CASE_HOST_TLS,
      h2_pal_e2e_host_tls(runtime, config->host.tls_echo_port,
                          config->host.root_ca_pem,
                          config->host.root_ca_pem_len, timeout_ms, 0));
  h2_pal_e2e_record(
      result, H2_PAL_E2E_CASE_HOST_TLS_WRONG_CA,
      h2_pal_e2e_host_tls(runtime, config->host.tls_wrong_ca_port,
                          config->host.wrong_ca_pem,
                          config->host.wrong_ca_pem_len, timeout_ms, 1));
  h2_pal_e2e_record(result, H2_PAL_E2E_CASE_HOST_HTTPS,
                    h2_pal_e2e_host_https(runtime, config->host.https_port,
                                          timeout_ms));
  h2_pal_e2e_record(result, H2_PAL_E2E_CASE_HOST_MQTT,
                    h2_pal_e2e_run_mqtt(runtime, config, result));
  h2_pal_e2e_record(result, H2_PAL_E2E_CASE_HOST_NETIF,
                    h2_pal_e2e_host_netif(runtime));
  h2_pal_e2e_record(result, H2_PAL_E2E_CASE_HOST_SYSTEM_EVENT,
                    h2_pal_e2e_host_system_event(runtime));
}

static int h2_pal_e2e_mqtt_str_valid(h2_pal_mqtt_str_t value) {
  return value.data != NULL && value.len > 0u;
}

static int h2_pal_e2e_mqtt_bytes_valid(h2_pal_mqtt_bytes_t value) {
  return value.data != NULL && value.len > 0u;
}

static void h2_pal_e2e_on_mqtt_event(void *user,
                                     h2_pal_mqtt_client_t *client,
                                     const h2_pal_mqtt_event_t *event) {
  (void)client;
  h2_pal_e2e_mqtt_events_t *events = (h2_pal_e2e_mqtt_events_t *)user;
  if (events == NULL || event == NULL) {
    return;
  }

  switch (event->type) {
    case H2_PAL_MQTT_EVENT_CONNECTED:
      events->connected++;
      break;
    case H2_PAL_MQTT_EVENT_SUBSCRIBE_ACK:
      events->subscribe_ack++;
      events->subscribe_result = event->data.subscribe_ack.result;
      if (event->data.subscribe_ack.result == H2_PAL_OK &&
          (event->data.subscribe_ack.result_count != 1u ||
           event->data.subscribe_ack.results == NULL)) {
        events->subscribe_result = H2_PAL_ERR_INVALID_STATE;
      } else if (event->data.subscribe_ack.result_count == 1u &&
                 event->data.subscribe_ack.results != NULL &&
                 event->data.subscribe_ack.results[0] ==
                     H2_PAL_MQTT_SUBACK_FAILURE) {
        events->subscribe_result = H2_PAL_ERR_IO;
      }
      break;
    case H2_PAL_MQTT_EVENT_PUBLISH_RECEIVED: {
      const h2_pal_mqtt_publish_received_event_t *publish =
          &event->data.publish_received;
      if (publish->topic.data != NULL && publish->payload.data != NULL &&
          publish->topic.len == events->topic.len &&
          memcmp(publish->topic.data, events->topic.data, events->topic.len) ==
              0 &&
          publish->payload.len == events->payload.len &&
          memcmp(publish->payload.data, events->payload.data,
                 events->payload.len) == 0) {
        events->publish_echo++;
      }
      break;
    }
    case H2_PAL_MQTT_EVENT_DISCONNECTED:
      events->disconnected++;
      break;
    default:
      break;
  }
}

static h2_pal_result_t h2_pal_e2e_now(const h2_runtime_t *runtime,
                                      uint64_t *out_now_ms) {
  return h2_pal_time_get_monotonic_ms(runtime->time, out_now_ms);
}

static h2_pal_result_t h2_pal_e2e_process_until(
    const h2_runtime_t *runtime,
    h2_pal_mqtt_client_t *client,
    const int *counter,
    uint64_t deadline_ms) {
  while (*counter == 0) {
    uint64_t now_ms = 0u;
    h2_pal_result_t result = h2_pal_e2e_now(runtime, &now_ms);
    if (result != H2_PAL_OK) {
      return result;
    }
    if (h2_pal_time_deadline_expired(now_ms, deadline_ms)) {
      return H2_PAL_ERR_TIMEOUT;
    }
    uint64_t remaining_ms = deadline_ms - now_ms;
    uint32_t slice_ms =
        remaining_ms > 250u ? 250u : (uint32_t)remaining_ms;
    result = h2_pal_mqtt_process(runtime->mqtt, client, slice_ms);
    if (result != H2_PAL_OK && result != H2_PAL_ERR_TIMEOUT) {
      return result;
    }
  }
  return H2_PAL_OK;
}

static h2_pal_result_t h2_pal_e2e_deadline(const h2_runtime_t *runtime,
                                           uint32_t timeout_ms,
                                           uint64_t *out_deadline_ms) {
  uint64_t now_ms = 0u;
  h2_pal_result_t result = h2_pal_e2e_now(runtime, &now_ms);
  if (result != H2_PAL_OK) {
    return result;
  }
  *out_deadline_ms = h2_pal_time_deadline_ms(now_ms, timeout_ms);
  return H2_PAL_OK;
}

static int h2_pal_e2e_mqtt_config_valid(const h2_runtime_t *runtime,
                                        const h2_pal_e2e_config_t *config) {
  if (runtime == NULL || runtime->mqtt == NULL || runtime->time == NULL ||
      config == NULL) {
    return 0;
  }
  const h2_pal_e2e_mqtt_config_t *mqtt = &config->mqtt;
  return mqtt->host != NULL && mqtt->host[0] != '\0' && mqtt->port != 0u &&
         (mqtt->transport == H2_PAL_MQTT_TRANSPORT_TCP ||
          mqtt->transport == H2_PAL_MQTT_TRANSPORT_TLS) &&
         h2_pal_e2e_mqtt_str_valid(mqtt->client_id) &&
         h2_pal_e2e_mqtt_str_valid(mqtt->topic) &&
         h2_pal_e2e_mqtt_bytes_valid(mqtt->payload) &&
         mqtt->network_buffer != NULL && mqtt->network_buffer_len > 0u;
}

static h2_pal_result_t h2_pal_e2e_run_mqtt(h2_runtime_t *runtime,
                                           const h2_pal_e2e_config_t *config,
                                           h2_pal_e2e_result_t *out_result) {
  out_result->stage = H2_PAL_E2E_STAGE_PREFLIGHT;
  if (!h2_pal_e2e_mqtt_config_valid(runtime, config)) {
    return H2_PAL_ERR_INVALID_ARG;
  }

  const h2_pal_e2e_mqtt_config_t *mqtt = &config->mqtt;
  const uint32_t timeout_ms = mqtt->timeout_ms == 0u
                                  ? H2_PAL_E2E_MQTT_DEFAULT_TIMEOUT_MS
                                  : mqtt->timeout_ms;
  h2_pal_e2e_mqtt_events_t events = {
      .topic = mqtt->topic,
      .payload = mqtt->payload,
      .subscribe_result = H2_PAL_ERR_INVALID_STATE,
  };
  h2_pal_net_tls_config_t tls = {
      .server_name = mqtt->host,
      .verify = H2_PAL_NET_TLS_VERIFY_REQUIRED,
  };
  h2_pal_mqtt_client_config_t client_config = {
      .endpoint = {
          .host = {.data = mqtt->host, .len = strlen(mqtt->host)},
          .port = mqtt->port,
      },
      .client_id = mqtt->client_id,
      .transport = mqtt->transport,
      .tls = mqtt->transport == H2_PAL_MQTT_TRANSPORT_TLS ? &tls : NULL,
      .keepalive_sec = 30u,
      .connect_timeout_ms = timeout_ms,
      .operation_timeout_ms = timeout_ms,
      .clean_session = 1,
      .network_buffer = mqtt->network_buffer,
      .network_buffer_len = mqtt->network_buffer_len,
      .on_event = h2_pal_e2e_on_mqtt_event,
      .event_user = &events,
  };

  h2_pal_mqtt_client_t *client = NULL;
  h2_pal_result_t result = H2_PAL_OK;
  out_result->stage = H2_PAL_E2E_STAGE_OPEN;
  result = h2_pal_mqtt_open(runtime->mqtt, &client_config, &client);
  if (result != H2_PAL_OK) {
    goto cleanup;
  }

  out_result->stage = H2_PAL_E2E_STAGE_CONNECT;
  result = h2_pal_mqtt_connect(runtime->mqtt, client);
  if (result != H2_PAL_OK) {
    goto cleanup;
  }
  if (events.connected != 1) {
    result = H2_PAL_ERR_INVALID_STATE;
    goto cleanup;
  }

  h2_pal_mqtt_subscribe_item_t item = {
      .filter = {.data = mqtt->topic.data, .len = mqtt->topic.len},
      .qos = H2_PAL_MQTT_QOS0,
  };
  h2_pal_mqtt_subscribe_request_t subscribe = {
      .items = &item,
      .item_count = 1u,
      .timeout_ms = timeout_ms,
  };
  out_result->stage = H2_PAL_E2E_STAGE_SUBSCRIBE;
  result = h2_pal_mqtt_subscribe(runtime->mqtt, client, &subscribe, NULL);
  uint64_t deadline_ms = 0u;
  if (result == H2_PAL_OK) {
    result = h2_pal_e2e_deadline(runtime, timeout_ms, &deadline_ms);
  }
  if (result == H2_PAL_OK) {
    result = h2_pal_e2e_process_until(runtime, client,
                                      &events.subscribe_ack, deadline_ms);
  }
  if (result != H2_PAL_OK) {
    goto cleanup;
  }
  if (events.subscribe_result != H2_PAL_OK) {
    result = events.subscribe_result;
    goto cleanup;
  }

  h2_pal_mqtt_publish_t publish = {
      .topic = mqtt->topic,
      .payload = mqtt->payload,
      .qos = H2_PAL_MQTT_QOS0,
      .timeout_ms = timeout_ms,
  };
  out_result->stage = H2_PAL_E2E_STAGE_PUBLISH;
  result = h2_pal_mqtt_publish(runtime->mqtt, client, &publish, NULL);
  if (result == H2_PAL_OK) {
    result = h2_pal_e2e_deadline(runtime, timeout_ms, &deadline_ms);
  }
  if (result == H2_PAL_OK) {
    result = h2_pal_e2e_process_until(runtime, client, &events.publish_echo,
                                      deadline_ms);
  }
  if (result != H2_PAL_OK) {
    goto cleanup;
  }

  out_result->stage = H2_PAL_E2E_STAGE_DISCONNECT;
  result = h2_pal_mqtt_disconnect(runtime->mqtt, client, timeout_ms);

cleanup:
  h2_pal_mqtt_close(runtime->mqtt, client);
  out_result->connected_events = events.connected;
  out_result->subscribe_ack_events = events.subscribe_ack;
  out_result->publish_echo_events = events.publish_echo;
  out_result->disconnected_events = events.disconnected;
  if (result == H2_PAL_OK) {
    out_result->stage = H2_PAL_E2E_STAGE_COMPLETE;
  }
  return result;
}

h2_pal_result_t h2_pal_e2e_run(h2_runtime_t *runtime,
                               const h2_pal_e2e_config_t *config,
                               h2_pal_e2e_result_t *out_result) {
  if (out_result == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  memset(out_result, 0, sizeof(*out_result));
  out_result->stage = H2_PAL_E2E_STAGE_PREFLIGHT;
  out_result->result = H2_PAL_OK;
  out_result->cleanup_result = H2_PAL_OK;
  if (runtime == NULL || config == NULL || config->suite_mask == 0u ||
      (config->suite_mask & ~(H2_PAL_E2E_SUITE_CORE |
                              H2_PAL_E2E_SUITE_MQTT |
                              H2_PAL_E2E_SUITE_PREF |
                              H2_PAL_E2E_SUITE_HOST)) != 0u ||
      ((config->suite_mask & H2_PAL_E2E_SUITE_PREF) != 0u &&
       config->suite_mask != H2_PAL_E2E_SUITE_PREF)) {
    out_result->result = H2_PAL_ERR_INVALID_ARG;
    out_result->failed = 1u;
    out_result->complete = 1;
    return out_result->result;
  }
  if (config->suite_mask == H2_PAL_E2E_SUITE_PREF) {
    return h2_pal_pref_e2e_run(runtime, out_result);
  }
  if ((config->suite_mask & H2_PAL_E2E_SUITE_CORE) != 0u) {
    h2_pal_e2e_run_core(runtime, out_result);
  }
  if ((config->suite_mask & H2_PAL_E2E_SUITE_HOST) != 0u) {
    h2_pal_e2e_run_host(runtime, config, out_result);
  }
  if ((config->suite_mask & H2_PAL_E2E_SUITE_MQTT) != 0u) {
    const h2_pal_result_t mqtt_result =
        h2_pal_e2e_run_mqtt(runtime, config, out_result);
    h2_pal_e2e_record(out_result, H2_PAL_E2E_CASE_MQTT, mqtt_result);
  }
  out_result->complete = 1;
  if (out_result->failed == 0u) {
    out_result->stage = H2_PAL_E2E_STAGE_COMPLETE;
    out_result->result = H2_PAL_OK;
  }
  return out_result->result;
}

h2_pal_result_t h2_pal_e2e_cleanup(h2_runtime_t *runtime,
                                   h2_pal_e2e_result_t *result) {
  if (runtime == NULL || result == NULL) return H2_PAL_ERR_INVALID_ARG;
  h2_pal_e2e_cleanup_t *run = result->retained_cleanup;
  if (run == NULL) return H2_PAL_OK;

  h2_pal_result_t cleanup = H2_PAL_OK;
  for (size_t index = 0u; index < run->started; ++index) {
    if (run->tasks[index] == NULL) continue;
    const h2_pal_result_t join =
        h2_pal_task_join(runtime->task, run->tasks[index]);
    if (join == H2_PAL_OK) {
      run->tasks[index] = NULL;
    } else if (cleanup == H2_PAL_OK) {
      cleanup = join;
    }
  }
  for (size_t index = 0u; index < run->started; ++index) {
    if (run->tasks[index] != NULL) {
      h2_pal_e2e_record_cleanup(result, cleanup);
      return cleanup == H2_PAL_OK ? H2_PAL_ERR_BUSY : cleanup;
    }
  }

  result->retained_cleanup = NULL;
  cleanup = h2_pal_e2e_concurrency_release(runtime, run, result);
  h2_pal_e2e_record_cleanup(result, cleanup);
  return cleanup;
}
