#include "h2_libco_smoke.h"

#include "h2_libco.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define H2_LIBCO_SMOKE_SCOPE "libco-smoke"
#define H2_LIBCO_SMOKE_WAIT_KEY ((uintptr_t)0x6c696263u)
#define H2_LIBCO_SMOKE_MAX_LIVE_TASKS 3u
#define H2_LIBCO_SMOKE_CLEANUP_TURNS 8u

typedef struct h2_libco_smoke_state h2_libco_smoke_state_t;

typedef struct h2_libco_smoke_task_slot {
  h2_libco_task_t *task;
  bool joined;
} h2_libco_smoke_task_slot_t;

typedef struct h2_libco_smoke_fifo_task {
  h2_libco_t *core;
  uint32_t id;
  uint32_t *order;
  size_t *order_count;
} h2_libco_smoke_fifo_task_t;

typedef struct h2_libco_smoke_wait_task {
  h2_libco_t *core;
  uint32_t timeout_ms;
  h2_libco_result_t result;
  uint32_t calls;
} h2_libco_smoke_wait_task_t;

typedef struct h2_libco_smoke_join_task {
  h2_libco_t *core;
  h2_libco_task_t *target;
  h2_libco_result_t result;
  int target_result;
} h2_libco_smoke_join_task_t;

typedef struct h2_libco_smoke_stress_task {
  h2_libco_t *core;
  uint32_t iterations;
  uint32_t switches;
  uint32_t checksum;
  h2_libco_result_t result;
} h2_libco_smoke_stress_task_t;

struct h2_libco_smoke_state {
  h2_runtime_t *runtime;
  h2_libco_t *core;
  h2_libco_task_options_t task_options;
  h2_libco_smoke_task_slot_t slots[H2_LIBCO_SMOKE_MAX_LIVE_TASKS];
  size_t slot_count;
  uint64_t last_now_ms;
  bool failure_logged;
  uint32_t fifo_order[6];
  size_t fifo_order_count;
  h2_libco_smoke_fifo_task_t fifo_tasks[3];
  h2_libco_smoke_wait_task_t wait_task;
  h2_libco_smoke_join_task_t join_task;
  h2_libco_smoke_stress_task_t stress_tasks[2];
};

static bool h2_libco_smoke_runtime_valid(const h2_runtime_t *runtime) {
  return runtime != NULL && runtime->mem != NULL &&
         runtime->mem->vtable != NULL && runtime->mem->vtable->alloc != NULL &&
         runtime->mem->vtable->free != NULL && runtime->time != NULL &&
         runtime->time->vtable != NULL &&
         runtime->time->vtable->get_monotonic_ms != NULL &&
         runtime->time->vtable->sleep_ms != NULL && runtime->log != NULL &&
         runtime->log->vtable != NULL && runtime->log->vtable->write != NULL;
}

static int h2_libco_smoke_log(h2_libco_smoke_state_t *state,
                              h2_pal_log_level_t level, const char *message) {
  return h2_pal_log_write(state->runtime->log, level, H2_LIBCO_SMOKE_SCOPE,
                          message);
}

static int h2_libco_smoke_stage(h2_libco_smoke_state_t *state,
                                const char *stage) {
  char message[H2_PAL_LOG_MESSAGE_MAX];
  int length = snprintf(message, sizeof(message),
                        "H2_LIBCO_SMOKE_STAGE stage=%s", stage);
  if (length < 0 || (size_t)length >= sizeof(message)) {
    return H2_PAL_ERR_TRUNCATED;
  }
  return h2_libco_smoke_log(state, H2_PAL_LOG_INFO, message);
}

static int h2_libco_smoke_fail(h2_libco_smoke_state_t *state, const char *stage,
                               int result) {
  if (!state->failure_logged) {
    char message[H2_PAL_LOG_MESSAGE_MAX];
    int length = snprintf(message, sizeof(message),
                          "H2_LIBCO_SMOKE_FAIL stage=%s rc=%d", stage, result);
    state->failure_logged = true;
    if (length >= 0 && (size_t)length < sizeof(message)) {
      (void)h2_libco_smoke_log(state, H2_PAL_LOG_ERROR, message);
    }
  }
  return result == H2_PAL_OK ? H2_PAL_ERR_INVALID_STATE : result;
}

static void *h2_libco_smoke_alloc(void *user, size_t size) {
  h2_libco_smoke_state_t *state = user;
  return h2_pal_mem_alloc(state->runtime->mem, size);
}

static void h2_libco_smoke_free(void *user, void *memory) {
  h2_libco_smoke_state_t *state = user;
  h2_pal_mem_free(state->runtime->mem, memory);
}

static int h2_libco_smoke_refresh_time(h2_libco_smoke_state_t *state) {
  uint64_t now_ms = state->last_now_ms;
  h2_pal_result_t result =
      h2_pal_time_get_monotonic_ms(state->runtime->time, &now_ms);
  if (result != H2_PAL_OK) {
    return result;
  }
  if (now_ms < state->last_now_ms) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  state->last_now_ms = now_ms;
  return H2_PAL_OK;
}

static uint64_t h2_libco_smoke_now_ms(void *user) {
  h2_libco_smoke_state_t *state = user;
  return state->last_now_ms;
}

static int h2_libco_smoke_schedule(h2_libco_smoke_state_t *state, size_t budget,
                                   size_t *out_resumed) {
  int refresh_result = h2_libco_smoke_refresh_time(state);
  h2_libco_result_t result =
      h2_libco_schedule(state->core, budget, out_resumed);
  return result == H2_LIBCO_OK ? refresh_result : result;
}

static void h2_libco_smoke_reset_slots(h2_libco_smoke_state_t *state) {
  memset(state->slots, 0, sizeof(state->slots));
  state->slot_count = 0u;
}

static int h2_libco_smoke_start(h2_libco_smoke_state_t *state,
                                h2_libco_task_entry_fn_t entry, void *user,
                                h2_libco_task_t **out_task) {
  if (state->slot_count >= H2_LIBCO_SMOKE_MAX_LIVE_TASKS) {
    return H2_PAL_ERR_NO_SPACE;
  }
  h2_libco_task_t *task = NULL;
  h2_libco_result_t result = h2_libco_task_start(
      state->core, &state->task_options, entry, user, &task);
  if (result != H2_LIBCO_OK) {
    return result;
  }
  state->slots[state->slot_count].task = task;
  ++state->slot_count;
  *out_task = task;
  return H2_PAL_OK;
}

static int h2_libco_smoke_join_slot(h2_libco_smoke_state_t *state, size_t index,
                                    int *out_entry_result) {
  if (index >= state->slot_count || state->slots[index].joined) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  h2_libco_result_t result = h2_libco_task_join(
      state->core, state->slots[index].task, out_entry_result);
  if (result == H2_LIBCO_OK) {
    state->slots[index].joined = true;
  }
  return result;
}

static int h2_libco_smoke_cleanup(h2_libco_smoke_state_t *state) {
  int cleanup_result = H2_PAL_OK;
  for (size_t index = 0u; index < state->slot_count; ++index) {
    if (!state->slots[index].joined) {
      h2_libco_result_t result =
          h2_libco_task_cancel(state->core, state->slots[index].task);
      if (result != H2_LIBCO_OK && result != H2_LIBCO_ERR_INVALID_STATE &&
          cleanup_result == H2_PAL_OK) {
        cleanup_result = result;
      }
    }
  }
  for (size_t turn = 0u; turn < H2_LIBCO_SMOKE_CLEANUP_TURNS; ++turn) {
    size_t resumed = 0u;
    int result =
        h2_libco_smoke_schedule(state, H2_LIBCO_SMOKE_MAX_LIVE_TASKS, &resumed);
    if (result != H2_PAL_OK) {
      if (cleanup_result == H2_PAL_OK) {
        cleanup_result = result;
      }
      break;
    }
    if (resumed == 0u) {
      break;
    }
  }
  for (size_t index = 0u; index < state->slot_count; ++index) {
    if (!state->slots[index].joined) {
      int result = h2_libco_smoke_join_slot(state, index, NULL);
      if (result != H2_LIBCO_OK && cleanup_result == H2_PAL_OK) {
        cleanup_result = result;
      }
    }
  }
  return cleanup_result;
}

static int h2_libco_smoke_fifo_entry(void *user) {
  h2_libco_smoke_fifo_task_t *task = user;
  task->order[(*task->order_count)++] = task->id;
  h2_libco_result_t result = h2_libco_yield(task->core);
  if (result != H2_LIBCO_OK) {
    return result;
  }
  task->order[(*task->order_count)++] = task->id + 10u;
  return (int)task->id;
}

static int h2_libco_smoke_wait_entry(void *user) {
  h2_libco_smoke_wait_task_t *task = user;
  ++task->calls;
  task->result =
      h2_libco_wait(task->core, H2_LIBCO_SMOKE_WAIT_KEY, task->timeout_ms);
  ++task->calls;
  return task->result;
}

static int h2_libco_smoke_target_entry(void *user) {
  h2_libco_t *core = user;
  h2_libco_result_t result = h2_libco_yield(core);
  return result == H2_LIBCO_OK ? 33 : result;
}

static int h2_libco_smoke_join_entry(void *user) {
  h2_libco_smoke_join_task_t *task = user;
  task->result =
      h2_libco_task_join(task->core, task->target, &task->target_result);
  return task->result == H2_LIBCO_OK ? 12 : task->result;
}

static h2_libco_result_t
h2_libco_smoke_depth_yield(h2_libco_smoke_stress_task_t *task, uint32_t depth,
                           uint32_t token) {
  volatile uint32_t canary = token ^ depth ^ UINT32_C(0xa55aa55a);
  h2_libco_result_t result =
      depth == 0u ? h2_libco_yield(task->core)
                  : h2_libco_smoke_depth_yield(task, depth - 1u, token + 1u);
  if (canary != (token ^ depth ^ UINT32_C(0xa55aa55a))) {
    return H2_LIBCO_ERR_STACK_CORRUPT;
  }
  return result;
}

static int h2_libco_smoke_stress_entry(void *user) {
  h2_libco_smoke_stress_task_t *task = user;
  uint32_t local = UINT32_C(0x12345678) ^ task->iterations;
  for (uint32_t index = 0u; index < task->iterations; ++index) {
    local = local * UINT32_C(1664525) + UINT32_C(1013904223);
    task->result = h2_libco_smoke_depth_yield(task, 4u, local);
    if (task->result != H2_LIBCO_OK) {
      return task->result;
    }
    ++task->switches;
  }
  task->checksum = local;
  return 99;
}

static uint32_t h2_libco_smoke_expected_checksum(uint32_t iterations) {
  uint32_t value = UINT32_C(0x12345678) ^ iterations;
  for (uint32_t index = 0u; index < iterations; ++index) {
    value = value * UINT32_C(1664525) + UINT32_C(1013904223);
  }
  return value;
}

static int h2_libco_smoke_fifo_phase(h2_libco_smoke_state_t *state) {
  h2_libco_smoke_reset_slots(state);
  memset(state->fifo_order, 0, sizeof(state->fifo_order));
  state->fifo_order_count = 0u;
  memset(state->fifo_tasks, 0, sizeof(state->fifo_tasks));
  for (size_t index = 0u; index < 3u; ++index) {
    state->fifo_tasks[index].core = state->core;
    state->fifo_tasks[index].id = (uint32_t)index + 1u;
    state->fifo_tasks[index].order = state->fifo_order;
    state->fifo_tasks[index].order_count = &state->fifo_order_count;
    h2_libco_task_t *task = NULL;
    int result = h2_libco_smoke_start(state, h2_libco_smoke_fifo_entry,
                                      &state->fifo_tasks[index], &task);
    if (result != H2_PAL_OK) {
      return result;
    }
  }
  size_t resumed = 0u;
  int result = h2_libco_smoke_schedule(state, 3u, &resumed);
  if (result != H2_PAL_OK || resumed != 3u || state->fifo_order_count != 3u ||
      state->fifo_order[0] != 1u || state->fifo_order[1] != 2u ||
      state->fifo_order[2] != 3u) {
    return result != H2_PAL_OK ? result : H2_PAL_ERR_INVALID_STATE;
  }
  result = h2_libco_smoke_schedule(state, 3u, &resumed);
  if (result != H2_PAL_OK || resumed != 3u || state->fifo_order_count != 6u ||
      state->fifo_order[3] != 11u || state->fifo_order[4] != 12u ||
      state->fifo_order[5] != 13u) {
    return result != H2_PAL_OK ? result : H2_PAL_ERR_INVALID_STATE;
  }
  for (size_t index = 0u; index < 3u; ++index) {
    int entry_result = 0;
    result = h2_libco_smoke_join_slot(state, index, &entry_result);
    if (result != H2_LIBCO_OK || entry_result != (int)index + 1) {
      return result != H2_LIBCO_OK ? result : H2_PAL_ERR_INVALID_STATE;
    }
  }
  return h2_libco_smoke_stage(state, "fifo-yield");
}

static int h2_libco_smoke_wait_phase(h2_libco_smoke_state_t *state,
                                     const char *stage, uint32_t timeout_ms,
                                     h2_libco_result_t expected, bool wake,
                                     bool cancel) {
  memset(&state->wait_task, 0, sizeof(state->wait_task));
  state->wait_task.core = state->core;
  state->wait_task.timeout_ms = timeout_ms;
  h2_libco_smoke_reset_slots(state);
  h2_libco_task_t *task = NULL;
  int result = h2_libco_smoke_start(state, h2_libco_smoke_wait_entry,
                                    &state->wait_task, &task);
  if (result != H2_PAL_OK) {
    return result;
  }
  size_t resumed = 0u;
  result = h2_libco_smoke_schedule(state, 1u, &resumed);
  if (result != H2_PAL_OK || resumed != 1u || state->wait_task.calls != 1u) {
    return result != H2_PAL_OK ? result : H2_PAL_ERR_INVALID_STATE;
  }
  if (wake) {
    size_t woken = 0u;
    result = h2_libco_wake(state->core, H2_LIBCO_SMOKE_WAIT_KEY, 1u, &woken);
    if (result != H2_LIBCO_OK || woken != 1u || state->wait_task.calls != 1u) {
      return result != H2_LIBCO_OK ? result : H2_PAL_ERR_INVALID_STATE;
    }
  } else if (cancel) {
    result = h2_libco_task_cancel(state->core, task);
    if (result != H2_LIBCO_OK) {
      return result;
    }
  } else {
    result = h2_pal_time_sleep_ms(state->runtime->time, timeout_ms + 1u);
    if (result != H2_PAL_OK) {
      return result;
    }
  }
  result = h2_libco_smoke_schedule(state, 1u, &resumed);
  if (result != H2_PAL_OK || resumed != 1u || state->wait_task.calls != 2u ||
      state->wait_task.result != expected) {
    return result != H2_PAL_OK ? result : H2_PAL_ERR_INVALID_STATE;
  }
  int entry_result = 0;
  result = h2_libco_smoke_join_slot(state, 0u, &entry_result);
  if (result != H2_LIBCO_OK || entry_result != expected) {
    return result != H2_LIBCO_OK ? result : H2_PAL_ERR_INVALID_STATE;
  }
  return h2_libco_smoke_stage(state, stage);
}

static int h2_libco_smoke_join_phase(h2_libco_smoke_state_t *state) {
  h2_libco_smoke_reset_slots(state);
  h2_libco_task_t *target = NULL;
  int result = h2_libco_smoke_start(state, h2_libco_smoke_target_entry,
                                    state->core, &target);
  if (result != H2_PAL_OK) {
    return result;
  }
  memset(&state->join_task, 0, sizeof(state->join_task));
  state->join_task.core = state->core;
  state->join_task.target = target;
  h2_libco_task_t *joiner = NULL;
  result = h2_libco_smoke_start(state, h2_libco_smoke_join_entry,
                                &state->join_task, &joiner);
  if (result != H2_PAL_OK) {
    return result;
  }
  size_t resumed = 0u;
  result = h2_libco_smoke_schedule(state, 2u, &resumed);
  if (result == H2_PAL_OK && resumed == 2u) {
    result = h2_libco_smoke_schedule(state, 2u, &resumed);
  }
  if (result == H2_PAL_OK && resumed == 1u) {
    result = h2_libco_smoke_schedule(state, 2u, &resumed);
  }
  if (result != H2_PAL_OK || resumed != 1u ||
      state->join_task.result != H2_LIBCO_OK ||
      state->join_task.target_result != 33) {
    return result != H2_PAL_OK ? result : H2_PAL_ERR_INVALID_STATE;
  }
  state->slots[0].joined = true;
  int entry_result = 0;
  result = h2_libco_smoke_join_slot(state, 1u, &entry_result);
  if (result != H2_LIBCO_OK || entry_result != 12) {
    return result != H2_LIBCO_OK ? result : H2_PAL_ERR_INVALID_STATE;
  }
  return h2_libco_smoke_stage(state, "join");
}

static int h2_libco_smoke_stress_phase(h2_libco_smoke_state_t *state,
                                       uint32_t iterations) {
  size_t task_count = iterations == 1u ? 1u : 2u;
  memset(state->stress_tasks, 0, sizeof(state->stress_tasks));
  state->stress_tasks[0].core = state->core;
  state->stress_tasks[0].iterations = (iterations + 1u) / 2u;
  state->stress_tasks[1].core = state->core;
  state->stress_tasks[1].iterations = iterations / 2u;
  h2_libco_smoke_reset_slots(state);
  for (size_t index = 0u; index < task_count; ++index) {
    h2_libco_task_t *task = NULL;
    int result = h2_libco_smoke_start(state, h2_libco_smoke_stress_entry,
                                      &state->stress_tasks[index], &task);
    if (result != H2_PAL_OK) {
      return result;
    }
  }
  size_t resumed = 0u;
  uint32_t turn_limit = state->stress_tasks[0].iterations + 3u;
  for (uint32_t turn = 0u; turn < turn_limit; ++turn) {
    int result = h2_libco_smoke_schedule(state, task_count, &resumed);
    if (result != H2_PAL_OK) {
      return result;
    }
    if (resumed == 0u) {
      break;
    }
  }
  if (resumed != 0u ||
      state->stress_tasks[0].switches + state->stress_tasks[1].switches !=
          iterations) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  for (size_t index = 0u; index < task_count; ++index) {
    if (state->stress_tasks[index].result != H2_LIBCO_OK ||
        state->stress_tasks[index].switches !=
            state->stress_tasks[index].iterations ||
        state->stress_tasks[index].checksum !=
            h2_libco_smoke_expected_checksum(
                state->stress_tasks[index].iterations)) {
      return H2_PAL_ERR_INVALID_STATE;
    }
    int entry_result = 0;
    int result = h2_libco_smoke_join_slot(state, index, &entry_result);
    if (result != H2_LIBCO_OK || entry_result != 99) {
      return result != H2_LIBCO_OK ? result : H2_PAL_ERR_INVALID_STATE;
    }
  }
  return h2_libco_smoke_stage(state, "switch-stress");
}

int h2_libco_smoke_run(h2_runtime_t *runtime,
                       const h2_libco_smoke_config_t *config) {
  size_t stack_size = config == NULL || config->task_stack_size == 0u
                          ? H2_LIBCO_SMOKE_DEFAULT_STACK_SIZE
                          : config->task_stack_size;
  uint32_t iterations = config == NULL || config->switch_iterations == 0u
                            ? H2_LIBCO_SMOKE_DEFAULT_SWITCH_ITERATIONS
                            : config->switch_iterations;
  if (!h2_libco_smoke_runtime_valid(runtime)) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  h2_libco_smoke_state_t state = {
      .runtime = runtime,
      .task_options = {.stack_size = stack_size},
  };
  if (stack_size < H2_LIBCO_SMOKE_MIN_STACK_SIZE ||
      stack_size > H2_LIBCO_SMOKE_MAX_STACK_SIZE ||
      stack_size > SIZE_MAX / H2_LIBCO_SMOKE_MAX_LIVE_TASKS ||
      stack_size % H2_LIBCO_STACK_ALIGNMENT != 0u || iterations == 0u ||
      iterations > H2_LIBCO_SMOKE_MAX_SWITCH_ITERATIONS) {
    return h2_libco_smoke_fail(&state, "config", H2_PAL_ERR_INVALID_ARG);
  }

  int result = h2_libco_smoke_refresh_time(&state);
  if (result != H2_PAL_OK) {
    return h2_libco_smoke_fail(&state, "runtime", result);
  }
  h2_libco_config_t libco_config = {
      .user = &state,
      .alloc = h2_libco_smoke_alloc,
      .free = h2_libco_smoke_free,
      .now_ms = h2_libco_smoke_now_ms,
  };
  result = h2_libco_create(&libco_config, &state.core);
  const char *stage = "create";
  if (result == H2_LIBCO_OK) {
    result = h2_libco_smoke_fifo_phase(&state);
    stage = "fifo-yield";
  }
  if (result == H2_PAL_OK) {
    result =
        h2_libco_smoke_wait_phase(&state, "wait-wake", H2_LIBCO_WAIT_FOREVER,
                                  H2_LIBCO_WOKEN, true, false);
    stage = "wait-wake";
  }
  if (result == H2_PAL_OK) {
    result = h2_libco_smoke_wait_phase(&state, "timeout", 1u,
                                       H2_LIBCO_ERR_TIMEOUT, false, false);
    stage = "timeout";
  }
  if (result == H2_PAL_OK) {
    result = h2_libco_smoke_wait_phase(&state, "cancel", H2_LIBCO_WAIT_FOREVER,
                                       H2_LIBCO_ERR_CANCELLED, false, true);
    stage = "cancel";
  }
  if (result == H2_PAL_OK) {
    result = h2_libco_smoke_join_phase(&state);
    stage = "join";
  }
  if (result == H2_PAL_OK) {
    result = h2_libco_smoke_stress_phase(&state, iterations);
    stage = "switch-stress";
  }

  int cleanup_result = H2_PAL_OK;
  if (state.core != NULL) {
    if (result != H2_PAL_OK) {
      cleanup_result = h2_libco_smoke_cleanup(&state);
    }
    h2_libco_result_t destroy_result = h2_libco_destroy(&state.core);
    if (destroy_result != H2_LIBCO_OK && cleanup_result == H2_PAL_OK) {
      cleanup_result = destroy_result;
    }
  }
  if (result == H2_PAL_OK && cleanup_result != H2_PAL_OK) {
    result = cleanup_result;
    stage = "cleanup";
  }
  if (result != H2_PAL_OK) {
    return h2_libco_smoke_fail(&state, stage, result);
  }
  result =
      h2_libco_smoke_log(&state, H2_PAL_LOG_INFO, "H2_LIBCO_SMOKE_PASS rc=0");
  return result == H2_PAL_OK ? H2_PAL_OK
                             : h2_libco_smoke_fail(&state, "pass", result);
}
