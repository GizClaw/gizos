#include "h2_pal_core_baseline.h"
#include "h2_pal_core_e2e_task_names.h"

#include <string.h>

#define H2_PAL_CORE_E2E_CONCURRENCY_PRODUCERS 3u
#define H2_PAL_CORE_E2E_CONCURRENCY_CONSUMERS 3u
#define H2_PAL_CORE_E2E_CONCURRENCY_ITEMS_PER_PRODUCER 32u
#define H2_PAL_CORE_E2E_CONCURRENCY_TOTAL_ITEMS                                \
  (H2_PAL_CORE_E2E_CONCURRENCY_PRODUCERS *                                     \
   H2_PAL_CORE_E2E_CONCURRENCY_ITEMS_PER_PRODUCER)
#define H2_PAL_CORE_E2E_CONCURRENCY_TASKS                                      \
  (H2_PAL_CORE_E2E_CONCURRENCY_PRODUCERS +                                     \
   H2_PAL_CORE_E2E_CONCURRENCY_CONSUMERS)
#define H2_PAL_CORE_E2E_CONCURRENCY_TIMEOUT_MS 2000u
#define H2_PAL_CORE_E2E_CONCURRENCY_SENTINEL UINT16_MAX
#define H2_PAL_CORE_E2E_TASK_JOIN_TIMEOUT_MS 2500u

typedef struct h2_pal_core_e2e_task_state {
  int ran;
} h2_pal_core_e2e_task_state_t;

typedef struct h2_pal_core_e2e_queue_state {
  h2_runtime_t *runtime;
  h2_pal_queue_t *queue;
  int value;
  h2_pal_result_t result;
} h2_pal_core_e2e_queue_state_t;

typedef struct h2_pal_core_e2e_condition_state {
  h2_runtime_t *runtime;
  h2_pal_mutex_t *mutex;
  h2_pal_cond_t *condition;
  int waiting;
  int signaled;
  h2_pal_result_t result;
} h2_pal_core_e2e_condition_state_t;

typedef struct h2_pal_core_e2e_concurrency_message {
  uint16_t producer;
  uint16_t sequence;
} h2_pal_core_e2e_concurrency_message_t;

typedef struct h2_pal_core_e2e_concurrency_state {
  h2_runtime_t *runtime;
  h2_pal_queue_t *queue;
  h2_pal_mutex_t *mutex;
  h2_pal_cond_t *ready_condition;
  h2_pal_cond_t *start_condition;
  size_t ready;
  int released;
  size_t produced;
  size_t consumed;
  uint8_t seen[H2_PAL_CORE_E2E_CONCURRENCY_TOTAL_ITEMS];
} h2_pal_core_e2e_concurrency_state_t;

typedef struct h2_pal_core_e2e_concurrency_worker {
  h2_pal_core_e2e_concurrency_state_t *state;
  size_t index;
  h2_pal_result_t result;
} h2_pal_core_e2e_concurrency_worker_t;

struct h2_pal_core_cleanup {
  h2_pal_timer_t *timer;
  int timer_calls;
  /* Given by the timer callback so the case waits for the expiry instead of
   * assuming it lands within a fixed sleep on a slow or loaded host. */
  const h2_pal_sync_api_t *timer_sync;
  h2_pal_semaphore_t *timer_fired;
  h2_pal_core_e2e_task_state_t task_worker;
  h2_pal_core_e2e_condition_state_t condition_worker;
  h2_pal_core_e2e_queue_state_t queue_worker;
  h2_pal_core_e2e_concurrency_state_t state;
  h2_pal_core_e2e_concurrency_worker_t
      workers[H2_PAL_CORE_E2E_CONCURRENCY_TASKS];
  h2_pal_task_t *tasks[H2_PAL_CORE_E2E_CONCURRENCY_TASKS];
  size_t started;
  int queue_closed;
};

static void h2_pal_core_e2e_task_entry(void *user) {
  ((h2_pal_core_e2e_task_state_t *)user)->ran = 1;
}

static void h2_pal_core_e2e_queue_entry(void *user) {
  h2_pal_core_e2e_queue_state_t *state = user;
  state->result = (h2_pal_result_t)h2_pal_queue_recv(
      state->runtime->queue, state->queue, &state->value, 1000u);
}

static void h2_pal_core_e2e_condition_entry(void *user) {
  h2_pal_core_e2e_condition_state_t *state = user;
  state->result = h2_pal_mutex_lock(state->runtime->sync, state->mutex);
  if (state->result != H2_PAL_OK)
    return;
  state->waiting = 1;
  while (!state->signaled && state->result == H2_PAL_OK) {
    state->result = h2_pal_cond_wait(state->runtime->sync, state->condition,
                                     state->mutex, 1000u);
  }
  const h2_pal_result_t unlock =
      h2_pal_mutex_unlock(state->runtime->sync, state->mutex);
  if (state->result == H2_PAL_OK)
    state->result = unlock;
}

static h2_pal_result_t h2_pal_core_e2e_concurrency_barrier(
    h2_pal_core_e2e_concurrency_worker_t *worker) {
  h2_pal_core_e2e_concurrency_state_t *state = worker->state;
  h2_pal_result_t result =
      h2_pal_mutex_lock(state->runtime->sync, state->mutex);
  if (result != H2_PAL_OK)
    return result;
  ++state->ready;
  result = h2_pal_cond_signal(state->runtime->sync, state->ready_condition);
  while (result == H2_PAL_OK && !state->released) {
    result =
        h2_pal_cond_wait(state->runtime->sync, state->start_condition,
                         state->mutex, H2_PAL_CORE_E2E_CONCURRENCY_TIMEOUT_MS);
  }
  const h2_pal_result_t unlock =
      h2_pal_mutex_unlock(state->runtime->sync, state->mutex);
  return result == H2_PAL_OK ? unlock : result;
}

static void h2_pal_core_e2e_concurrency_producer(void *user) {
  h2_pal_core_e2e_concurrency_worker_t *worker = user;
  h2_pal_core_e2e_concurrency_state_t *state = worker->state;
  worker->result = h2_pal_core_e2e_concurrency_barrier(worker);
  for (size_t sequence = 0u;
       worker->result == H2_PAL_OK &&
       sequence < H2_PAL_CORE_E2E_CONCURRENCY_ITEMS_PER_PRODUCER;
       ++sequence) {
    const h2_pal_core_e2e_concurrency_message_t message = {
        .producer = (uint16_t)worker->index,
        .sequence = (uint16_t)sequence,
    };
    worker->result = (h2_pal_result_t)h2_pal_queue_send(
        state->runtime->queue, state->queue, &message,
        H2_PAL_CORE_E2E_CONCURRENCY_TIMEOUT_MS);
    if (worker->result != H2_PAL_OK)
      break;
    worker->result = h2_pal_mutex_lock(state->runtime->sync, state->mutex);
    if (worker->result != H2_PAL_OK)
      break;
    ++state->produced;
    worker->result = h2_pal_mutex_unlock(state->runtime->sync, state->mutex);
  }
}

static void h2_pal_core_e2e_concurrency_consumer(void *user) {
  h2_pal_core_e2e_concurrency_worker_t *worker = user;
  h2_pal_core_e2e_concurrency_state_t *state = worker->state;
  worker->result = h2_pal_core_e2e_concurrency_barrier(worker);
  while (worker->result == H2_PAL_OK) {
    h2_pal_core_e2e_concurrency_message_t message = {0};
    worker->result = (h2_pal_result_t)h2_pal_queue_recv(
        state->runtime->queue, state->queue, &message,
        H2_PAL_CORE_E2E_CONCURRENCY_TIMEOUT_MS);
    if (worker->result != H2_PAL_OK ||
        message.producer == H2_PAL_CORE_E2E_CONCURRENCY_SENTINEL) {
      break;
    }
    if (message.producer >= H2_PAL_CORE_E2E_CONCURRENCY_PRODUCERS ||
        message.sequence >= H2_PAL_CORE_E2E_CONCURRENCY_ITEMS_PER_PRODUCER) {
      worker->result = H2_PAL_ERR_INVALID_STATE;
      break;
    }
    const size_t item = (size_t)message.producer *
                            H2_PAL_CORE_E2E_CONCURRENCY_ITEMS_PER_PRODUCER +
                        message.sequence;
    worker->result = h2_pal_mutex_lock(state->runtime->sync, state->mutex);
    if (worker->result != H2_PAL_OK)
      break;
    if (state->seen[item] != 0u) {
      worker->result = H2_PAL_ERR_INVALID_STATE;
    } else {
      state->seen[item] = 1u;
      ++state->consumed;
    }
    const h2_pal_result_t unlock =
        h2_pal_mutex_unlock(state->runtime->sync, state->mutex);
    if (worker->result == H2_PAL_OK)
      worker->result = unlock;
  }
}

static h2_pal_result_t h2_pal_core_e2e_concurrency_join(h2_runtime_t *runtime,
                                                        h2_pal_task_t **task) {
  if (task == NULL || *task == NULL)
    return H2_PAL_OK;
  uint64_t now = 0u;
  h2_pal_result_t result = h2_pal_time_get_monotonic_ms(runtime->time, &now);
  if (result != H2_PAL_OK)
    return result;
  const uint64_t deadline =
      UINT64_MAX - now < H2_PAL_CORE_E2E_TASK_JOIN_TIMEOUT_MS
          ? UINT64_MAX
          : now + H2_PAL_CORE_E2E_TASK_JOIN_TIMEOUT_MS;
  for (uint32_t attempts = 0u; attempts <= H2_PAL_CORE_E2E_TASK_JOIN_TIMEOUT_MS;
       ++attempts) {
    const h2_pal_result_t join = h2_pal_task_join(runtime->task, *task);
    if (join == H2_PAL_OK) {
      *task = NULL;
      return H2_PAL_OK;
    }
    if (join != H2_PAL_ERR_BUSY)
      return join;
    result = h2_pal_time_get_monotonic_ms(runtime->time, &now);
    if (result != H2_PAL_OK)
      return result;
    if (now >= deadline)
      return H2_PAL_ERR_TIMEOUT;
    result = h2_pal_time_sleep_ms(runtime->time, 1u);
    if (result != H2_PAL_OK)
      return result;
  }
  return H2_PAL_ERR_TIMEOUT;
}

static void h2_pal_core_e2e_timer_callback(void *user, h2_pal_timer_t *timer) {
  (void)timer;
  h2_pal_core_cleanup_t *run = user;
  ++run->timer_calls;
  (void)h2_pal_semaphore_give(run->timer_sync, run->timer_fired);
}

static void
h2_pal_core_e2e_record_cleanup(h2_pal_core_baseline_result_t *result,
                               h2_pal_result_t cleanup) {
  if (cleanup != H2_PAL_OK && result->cleanup_result == H2_PAL_OK) {
    result->cleanup_result = cleanup;
  }
}

static h2_pal_result_t
h2_pal_core_e2e_concurrency_release(h2_runtime_t *runtime,
                                    h2_pal_core_cleanup_t *run,
                                    h2_pal_core_baseline_result_t *e2e_result) {
  h2_pal_result_t result = H2_PAL_OK;
  if (run->state.queue != NULL) {
    if (!run->queue_closed) {
      result =
          (h2_pal_result_t)h2_pal_queue_close(runtime->queue, run->state.queue);
      if (result != H2_PAL_OK)
        goto retained;
      run->queue_closed = 1;
    }
    h2_pal_queue_destroy(runtime->queue, run->state.queue);
    run->state.queue = NULL;
  }
  if (run->state.start_condition != NULL) {
    result = h2_pal_cond_destroy(runtime->sync, run->state.start_condition);
    if (result != H2_PAL_OK)
      goto retained;
    run->state.start_condition = NULL;
  }
  if (run->state.ready_condition != NULL) {
    result = h2_pal_cond_destroy(runtime->sync, run->state.ready_condition);
    if (result != H2_PAL_OK)
      goto retained;
    run->state.ready_condition = NULL;
  }
  if (run->state.mutex != NULL) {
    result = h2_pal_mutex_destroy(runtime->sync, run->state.mutex);
    if (result != H2_PAL_OK)
      goto retained;
    run->state.mutex = NULL;
  }
  h2_pal_mem_free(runtime->mem, run);
  return H2_PAL_OK;
retained:
  h2_pal_core_e2e_record_cleanup(e2e_result, result);
  e2e_result->retained_cleanup = run;
  return result;
}

h2_pal_result_t h2_pal_core_baseline_time(h2_runtime_t *runtime) {
  uint64_t before = 0u;
  uint64_t after = 0u;
  h2_pal_result_t result = h2_pal_time_get_monotonic_ms(runtime->time, &before);
  if (result == H2_PAL_OK) {
    result = h2_pal_time_sleep_ms(runtime->time, 1u);
  }
  if (result == H2_PAL_OK) {
    result = h2_pal_time_get_monotonic_ms(runtime->time, &after);
  }
  return result == H2_PAL_OK && after >= before ? H2_PAL_OK
                                                : H2_PAL_ERR_INVALID_STATE;
}

h2_pal_result_t
h2_pal_core_baseline_timer(h2_runtime_t *runtime,
                           h2_pal_core_baseline_result_t *e2e_result) {
  h2_pal_core_cleanup_t *run = h2_pal_mem_alloc(runtime->mem, sizeof(*run));
  if (run == NULL)
    return H2_PAL_ERR_NO_MEMORY;
  memset(run, 0, sizeof(*run));
  run->timer_sync = runtime->sync;
  const h2_pal_semaphore_config_t fired = {
      .name = "pal-e2e-timer",
      .allocator = runtime->mem,
      .initial_count = 0u,
      .max_count = 2u,
  };
  const h2_pal_timer_config_t config = {
      .name = "pal-e2e",
      .period_ms = 1u,
      .flags = H2_PAL_TIMER_FLAG_AUTO_START,
      .cb = h2_pal_core_e2e_timer_callback,
      .cb_user = run,
  };
  h2_pal_result_t result =
      h2_pal_semaphore_create(runtime->sync, &fired, &run->timer_fired);
  if (result == H2_PAL_OK)
    result = h2_pal_timer_create(runtime->timer, &config, &run->timer);
  /* A one-shot expiry must arrive within a generous bound; a short settle
   * afterwards gives a spurious second expiry the chance to show. */
  if (result == H2_PAL_OK)
    result = h2_pal_semaphore_take(runtime->sync, run->timer_fired, 1000u);
  if (result == H2_PAL_OK)
    result = h2_pal_time_sleep_ms(runtime->time, 5u);
  h2_pal_result_t cleanup =
      run->timer == NULL ? H2_PAL_OK
                         : h2_pal_timer_destroy(runtime->timer, run->timer);
  h2_pal_core_e2e_record_cleanup(e2e_result, cleanup);
  if (cleanup != H2_PAL_OK) {
    /* The callback may still run and give the semaphore: keep both. */
    e2e_result->retained_cleanup = run;
    return result == H2_PAL_OK ? cleanup : result;
  }
  run->timer = NULL;
  if (run->timer_fired != NULL) {
    cleanup = h2_pal_semaphore_destroy(runtime->sync, run->timer_fired);
    h2_pal_core_e2e_record_cleanup(e2e_result, cleanup);
    if (cleanup != H2_PAL_OK) {
      /* Keep the handle so h2_pal_core_baseline_cleanup can retry it. */
      e2e_result->retained_cleanup = run;
      return result == H2_PAL_OK ? cleanup : result;
    }
    run->timer_fired = NULL;
  }
  const int calls = run->timer_calls;
  h2_pal_mem_free(runtime->mem, run);
  if (result != H2_PAL_OK)
    return result;
  return result == H2_PAL_OK && calls == 1 ? H2_PAL_OK
                                           : H2_PAL_ERR_INVALID_STATE;
}

h2_pal_result_t
h2_pal_core_baseline_task(h2_runtime_t *runtime,
                          h2_pal_core_baseline_result_t *e2e_result) {
  h2_pal_core_cleanup_t *run = h2_pal_mem_alloc(runtime->mem, sizeof(*run));
  if (run == NULL)
    return H2_PAL_ERR_NO_MEMORY;
  memset(run, 0, sizeof(*run));
  const h2_pal_task_options_t options = {
      .name = h2_pal_core_e2e_core_task_name,
  };
  h2_pal_result_t result =
      h2_pal_task_start(runtime->task, &options, h2_pal_core_e2e_task_entry,
                        &run->task_worker, &run->tasks[0]);
  if (result == H2_PAL_OK) {
    run->started = 1u;
    result = h2_pal_core_e2e_concurrency_join(runtime, &run->tasks[0]);
    if (run->tasks[0] != NULL) {
      h2_pal_core_e2e_record_cleanup(e2e_result, result);
      e2e_result->retained_cleanup = run;
      return result;
    }
  }
  const int ran = run->task_worker.ran;
  h2_pal_mem_free(runtime->mem, run);
  if (result != H2_PAL_OK)
    return result;
  return ran == 1 ? H2_PAL_OK : H2_PAL_ERR_INVALID_STATE;
}

h2_pal_result_t
h2_pal_core_baseline_queue(h2_runtime_t *runtime,
                           h2_pal_core_baseline_result_t *e2e_result) {
  h2_pal_core_cleanup_t *run = h2_pal_mem_alloc(runtime->mem, sizeof(*run));
  if (run == NULL)
    return H2_PAL_ERR_NO_MEMORY;
  memset(run, 0, sizeof(*run));
  h2_pal_queue_t *queue = NULL;
  const h2_pal_queue_config_t config = {
      .name = "pal-e2e",
      .item_size = sizeof(int),
      .item_count = 1u,
      .allocator = runtime->mem,
  };
  h2_pal_result_t result =
      (h2_pal_result_t)h2_pal_queue_create(runtime->queue, &config, &queue);
  run->state.queue = queue;
  run->queue_worker = (h2_pal_core_e2e_queue_state_t){
      .runtime = runtime,
      .queue = queue,
      .result = H2_PAL_ERR_INVALID_STATE,
  };
  h2_pal_task_t *task = NULL;
  if (result == H2_PAL_OK) {
    const h2_pal_task_options_t options = {
        .name = h2_pal_core_e2e_queue_task_name,
    };
    result =
        h2_pal_task_start(runtime->task, &options, h2_pal_core_e2e_queue_entry,
                          &run->queue_worker, &task);
  }
  if (result == H2_PAL_OK) {
    result = h2_pal_time_sleep_ms(runtime->time, 1u);
  }
  const int sent = 42;
  if (result == H2_PAL_OK) {
    result =
        (h2_pal_result_t)h2_pal_queue_send(runtime->queue, queue, &sent, 1000u);
  }
  if (task != NULL) {
    /* A failed send still leaves the receiver alive. Close to wake it, then
     * join before releasing either its queue or its entry context. */
    if (result != H2_PAL_OK) {
      const h2_pal_result_t close_result =
          (h2_pal_result_t)h2_pal_queue_close(runtime->queue, queue);
      run->queue_closed = close_result == H2_PAL_OK;
      h2_pal_core_e2e_record_cleanup(e2e_result, close_result);
    }
    run->tasks[0] = task;
    run->started = 1u;
    const h2_pal_result_t join =
        h2_pal_core_e2e_concurrency_join(runtime, &run->tasks[0]);
    if (result == H2_PAL_OK)
      result = join;
    if (run->tasks[0] != NULL) {
      h2_pal_core_e2e_record_cleanup(e2e_result, join);
      e2e_result->retained_cleanup = run;
      return result;
    }
  }
  const h2_pal_core_e2e_queue_state_t state = run->queue_worker;
  const h2_pal_result_t cleanup =
      h2_pal_core_e2e_concurrency_release(runtime, run, e2e_result);
  if (result == H2_PAL_OK)
    result = cleanup;
  return result == H2_PAL_OK && state.result == H2_PAL_OK && state.value == sent
             ? H2_PAL_OK
             : H2_PAL_ERR_INVALID_STATE;
}

h2_pal_result_t
h2_pal_core_baseline_mutex(h2_runtime_t *runtime,
                           h2_pal_core_baseline_result_t *e2e_result) {
  h2_pal_mutex_t *mutex = NULL;
  const h2_pal_mutex_config_t config = {
      .name = "pal-e2e",
      .allocator = runtime->mem,
      .flags = H2_PAL_MUTEX_FLAG_NONE,
  };
  h2_pal_result_t result = h2_pal_mutex_create(runtime->sync, &config, &mutex);
  if (result == H2_PAL_OK)
    result = h2_pal_mutex_lock(runtime->sync, mutex);
  if (result == H2_PAL_OK)
    result = h2_pal_mutex_unlock(runtime->sync, mutex);
  if (mutex != NULL) {
    h2_pal_result_t cleanup = h2_pal_mutex_destroy(runtime->sync, mutex);
    h2_pal_core_e2e_record_cleanup(e2e_result, cleanup);
    if (result == H2_PAL_OK)
      result = cleanup;
  }
  return result;
}

h2_pal_result_t
h2_pal_core_baseline_semaphore(h2_runtime_t *runtime,
                               h2_pal_core_baseline_result_t *e2e_result) {
  h2_pal_semaphore_t *semaphore = NULL;
  const h2_pal_semaphore_config_t config = {
      .name = "pal-e2e",
      .allocator = runtime->mem,
      .initial_count = 0u,
      .max_count = 1u,
  };
  h2_pal_result_t result =
      h2_pal_semaphore_create(runtime->sync, &config, &semaphore);
  if (result == H2_PAL_OK)
    result = h2_pal_semaphore_give(runtime->sync, semaphore);
  if (result == H2_PAL_OK)
    result = h2_pal_semaphore_take(runtime->sync, semaphore, 0u);
  if (semaphore != NULL) {
    h2_pal_result_t cleanup =
        h2_pal_semaphore_destroy(runtime->sync, semaphore);
    h2_pal_core_e2e_record_cleanup(e2e_result, cleanup);
    if (result == H2_PAL_OK)
      result = cleanup;
  }
  return result;
}

h2_pal_result_t
h2_pal_core_baseline_condition(h2_runtime_t *runtime,
                               h2_pal_core_baseline_result_t *e2e_result) {
  h2_pal_core_cleanup_t *run = h2_pal_mem_alloc(runtime->mem, sizeof(*run));
  if (run == NULL)
    return H2_PAL_ERR_NO_MEMORY;
  memset(run, 0, sizeof(*run));
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
  run->state.mutex = mutex;
  run->state.ready_condition = condition;
  run->condition_worker = (h2_pal_core_e2e_condition_state_t){
      .runtime = runtime,
      .mutex = mutex,
      .condition = condition,
      .result = H2_PAL_ERR_INVALID_STATE,
  };
  if (result == H2_PAL_OK) {
    const h2_pal_task_options_t options = {
        .name = h2_pal_core_e2e_condition_task_name,
    };
    result = h2_pal_task_start(runtime->task, &options,
                               h2_pal_core_e2e_condition_entry,
                               &run->condition_worker, &task);
  }
  if (result == H2_PAL_OK) {
    result = h2_pal_time_sleep_ms(runtime->time, 1u);
  }
  if (task != NULL) {
    h2_pal_result_t wake_result = h2_pal_mutex_lock(runtime->sync, mutex);
    if (wake_result == H2_PAL_OK) {
      run->condition_worker.signaled = 1;
      wake_result = h2_pal_cond_signal(runtime->sync, condition);
      const h2_pal_result_t unlock = h2_pal_mutex_unlock(runtime->sync, mutex);
      if (wake_result == H2_PAL_OK)
        wake_result = unlock;
    }
    if (result == H2_PAL_OK)
      result = wake_result;
    run->started = 1u;
    run->tasks[0] = task;
    const h2_pal_result_t join =
        h2_pal_core_e2e_concurrency_join(runtime, &run->tasks[0]);
    if (result == H2_PAL_OK)
      result = join;
    if (run->tasks[0] != NULL) {
      h2_pal_core_e2e_record_cleanup(e2e_result, join);
      e2e_result->retained_cleanup = run;
      return result;
    }
  }
  const h2_pal_core_e2e_condition_state_t state = run->condition_worker;
  const h2_pal_result_t cleanup =
      h2_pal_core_e2e_concurrency_release(runtime, run, e2e_result);
  if (result == H2_PAL_OK)
    result = cleanup;
  if (result != H2_PAL_OK)
    return result;
  return state.waiting && state.signaled && state.result == H2_PAL_OK
             ? H2_PAL_OK
             : H2_PAL_ERR_INVALID_STATE;
}

h2_pal_result_t
h2_pal_core_baseline_concurrency(h2_runtime_t *runtime,
                                 h2_pal_core_baseline_result_t *e2e_result) {
  h2_pal_queue_t *queue = NULL;
  h2_pal_mutex_t *mutex = NULL;
  h2_pal_cond_t *ready_condition = NULL;
  h2_pal_cond_t *start_condition = NULL;
  h2_pal_core_cleanup_t *run = h2_pal_mem_alloc(runtime->mem, sizeof(*run));
  if (run == NULL)
    return H2_PAL_ERR_NO_MEMORY;
  memset(run, 0, sizeof(*run));
  h2_pal_task_t **tasks = run->tasks;
  h2_pal_core_e2e_concurrency_worker_t *workers = run->workers;
  const h2_pal_queue_config_t queue_config = {
      .name = "pal-e2e-concurrency",
      .item_size = sizeof(h2_pal_core_e2e_concurrency_message_t),
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
    result = h2_pal_cond_create(runtime->sync, &ready_condition_config,
                                &ready_condition);
  }
  if (result == H2_PAL_OK) {
    result = h2_pal_cond_create(runtime->sync, &start_condition_config,
                                &start_condition);
  }
  run->state = (h2_pal_core_e2e_concurrency_state_t){
      .runtime = runtime,
      .queue = queue,
      .mutex = mutex,
      .ready_condition = ready_condition,
      .start_condition = start_condition,
  };
  h2_pal_core_e2e_concurrency_state_t *state = &run->state;
  size_t started = 0u;
  while (result == H2_PAL_OK && started < H2_PAL_CORE_E2E_CONCURRENCY_TASKS) {
    workers[started] = (h2_pal_core_e2e_concurrency_worker_t){
        .state = state,
        .index = started < H2_PAL_CORE_E2E_CONCURRENCY_CONSUMERS
                     ? started
                     : started - H2_PAL_CORE_E2E_CONCURRENCY_CONSUMERS,
        .result = H2_PAL_ERR_INVALID_STATE,
    };
    const h2_pal_task_entry_t entry =
        started < H2_PAL_CORE_E2E_CONCURRENCY_CONSUMERS
            ? h2_pal_core_e2e_concurrency_consumer
            : h2_pal_core_e2e_concurrency_producer;
    const h2_pal_task_options_t options = {
        .name = started < H2_PAL_CORE_E2E_CONCURRENCY_CONSUMERS
                    ? h2_pal_core_e2e_consumer_task_name
                    : h2_pal_core_e2e_producer_task_name,
    };
    result = h2_pal_task_start(runtime->task, &options, entry,
                               &workers[started], &tasks[started]);
    if (result == H2_PAL_OK)
      ++started;
    run->started = started;
  }

  if (started > 0u) {
    h2_pal_result_t release_result = h2_pal_mutex_lock(runtime->sync, mutex);
    if (release_result == H2_PAL_OK) {
      while (release_result == H2_PAL_OK && state->ready < started) {
        release_result =
            h2_pal_cond_wait(runtime->sync, ready_condition, mutex,
                             H2_PAL_CORE_E2E_CONCURRENCY_TIMEOUT_MS);
      }
      state->released = 1;
      const h2_pal_result_t broadcast =
          h2_pal_cond_broadcast(runtime->sync, start_condition);
      if (release_result == H2_PAL_OK)
        release_result = broadcast;
      const h2_pal_result_t unlock = h2_pal_mutex_unlock(runtime->sync, mutex);
      if (release_result == H2_PAL_OK)
        release_result = unlock;
    }
    if (result == H2_PAL_OK)
      result = release_result;
  }

  for (size_t index = H2_PAL_CORE_E2E_CONCURRENCY_CONSUMERS; index < started;
       ++index) {
    const h2_pal_result_t join =
        h2_pal_core_e2e_concurrency_join(runtime, &tasks[index]);
    if (result == H2_PAL_OK)
      result = join;
  }

  h2_pal_result_t sentinel_result = H2_PAL_OK;
  const h2_pal_core_e2e_concurrency_message_t sentinel = {
      .producer = H2_PAL_CORE_E2E_CONCURRENCY_SENTINEL,
  };
  const size_t consumers_started =
      started < H2_PAL_CORE_E2E_CONCURRENCY_CONSUMERS
          ? started
          : H2_PAL_CORE_E2E_CONCURRENCY_CONSUMERS;
  for (size_t index = 0u;
       sentinel_result == H2_PAL_OK && index < consumers_started; ++index) {
    sentinel_result = (h2_pal_result_t)h2_pal_queue_send(
        runtime->queue, queue, &sentinel,
        H2_PAL_CORE_E2E_CONCURRENCY_TIMEOUT_MS);
  }
  if (sentinel_result != H2_PAL_OK && queue != NULL) {
    const h2_pal_result_t close =
        (h2_pal_result_t)h2_pal_queue_close(runtime->queue, queue);
    h2_pal_core_e2e_record_cleanup(e2e_result, close);
    run->queue_closed = close == H2_PAL_OK;
  }
  if (result == H2_PAL_OK)
    result = sentinel_result;

  for (size_t index = 0u; index < consumers_started; ++index) {
    const h2_pal_result_t join =
        h2_pal_core_e2e_concurrency_join(runtime, &tasks[index]);
    if (result == H2_PAL_OK)
      result = join;
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
      const h2_pal_result_t retained =
          result == H2_PAL_OK ? H2_PAL_ERR_BUSY : result;
      h2_pal_core_e2e_record_cleanup(e2e_result, retained);
      e2e_result->retained_cleanup = run;
      return retained;
    }
  }
  if (result == H2_PAL_OK &&
      (state->ready != H2_PAL_CORE_E2E_CONCURRENCY_TASKS ||
       state->produced != H2_PAL_CORE_E2E_CONCURRENCY_TOTAL_ITEMS ||
       state->consumed != H2_PAL_CORE_E2E_CONCURRENCY_TOTAL_ITEMS)) {
    result = H2_PAL_ERR_INVALID_STATE;
  }
  for (size_t index = 0u;
       result == H2_PAL_OK && index < H2_PAL_CORE_E2E_CONCURRENCY_TOTAL_ITEMS;
       ++index) {
    if (state->seen[index] != 1u)
      result = H2_PAL_ERR_INVALID_STATE;
  }

  const h2_pal_result_t cleanup =
      h2_pal_core_e2e_concurrency_release(runtime, run, e2e_result);
  if (result == H2_PAL_OK)
    result = cleanup;
  return result;
}

h2_pal_result_t
h2_pal_core_baseline_cleanup(h2_runtime_t *runtime,
                             h2_pal_core_baseline_result_t *result) {
  if (runtime == NULL || result == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  h2_pal_core_cleanup_t *run = result->retained_cleanup;
  if (run == NULL)
    return H2_PAL_OK;

  h2_pal_result_t cleanup = H2_PAL_OK;
  if (run->timer != NULL) {
    cleanup = h2_pal_timer_destroy(runtime->timer, run->timer);
    if (cleanup != H2_PAL_OK) {
      h2_pal_core_e2e_record_cleanup(result, cleanup);
      return cleanup;
    }
    run->timer = NULL;
  }
  if (run->timer_fired != NULL) {
    cleanup = h2_pal_semaphore_destroy(runtime->sync, run->timer_fired);
    if (cleanup != H2_PAL_OK) {
      h2_pal_core_e2e_record_cleanup(result, cleanup);
      return cleanup;
    }
    run->timer_fired = NULL;
  }
  for (size_t index = 0u; index < run->started; ++index) {
    if (run->tasks[index] == NULL)
      continue;
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
      h2_pal_core_e2e_record_cleanup(result, cleanup);
      return cleanup == H2_PAL_OK ? H2_PAL_ERR_BUSY : cleanup;
    }
  }

  result->retained_cleanup = NULL;
  cleanup = h2_pal_core_e2e_concurrency_release(runtime, run, result);
  h2_pal_core_e2e_record_cleanup(result, cleanup);
  return cleanup;
}
