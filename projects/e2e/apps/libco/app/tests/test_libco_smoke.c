#include "h2_libco_smoke.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct test_runtime_state {
  size_t alloc_calls;
  size_t fail_alloc_call;
  size_t live_allocations;
  size_t log_calls;
  size_t fail_log_call;
  size_t time_calls;
  size_t fail_time_call;
  size_t fail_time_from_call;
  size_t regress_time_call;
  uint64_t now_ms;
  char log[8192];
  size_t log_len;
} test_runtime_state_t;

static void *test_alloc(void *user, size_t size) {
  test_runtime_state_t *state = user;
  ++state->alloc_calls;
  if (state->fail_alloc_call != 0u &&
      state->alloc_calls == state->fail_alloc_call) {
    return NULL;
  }
  void *memory = malloc(size);
  if (memory != NULL) {
    ++state->live_allocations;
  }
  return memory;
}

static void *test_realloc(void *user, void *memory, size_t size) {
  (void)user;
  return realloc(memory, size);
}

static void test_free(void *user, void *memory) {
  test_runtime_state_t *state = user;
  assert(memory != NULL && state->live_allocations > 0u);
  --state->live_allocations;
  free(memory);
}

static int test_log(void *user, h2_pal_log_level_t level, const char *scope,
                    const char *message) {
  test_runtime_state_t *state = user;
  (void)level;
  (void)scope;
  ++state->log_calls;
  if (state->fail_log_call != 0u && state->log_calls == state->fail_log_call) {
    return H2_PAL_ERR_WRITE;
  }
  int length = snprintf(state->log + state->log_len,
                        sizeof(state->log) - state->log_len, "%s\n", message);
  if (length < 0 || (size_t)length >= sizeof(state->log) - state->log_len) {
    return H2_PAL_ERR_TRUNCATED;
  }
  state->log_len += (size_t)length;
  return H2_PAL_OK;
}

static h2_pal_result_t test_now(void *user, uint64_t *out_ms) {
  test_runtime_state_t *state = user;
  ++state->time_calls;
  if (state->fail_time_call != 0u &&
      state->time_calls == state->fail_time_call) {
    return H2_PAL_ERR_IO;
  }
  if (state->fail_time_from_call != 0u &&
      state->time_calls >= state->fail_time_from_call) {
    return H2_PAL_ERR_IO;
  }
  *out_ms = state->regress_time_call == state->time_calls ? state->now_ms - 1u
                                                          : state->now_ms;
  return H2_PAL_OK;
}

static h2_pal_result_t test_sleep(void *user, uint32_t ms) {
  test_runtime_state_t *state = user;
  state->now_ms += ms;
  return H2_PAL_OK;
}

static h2_runtime_t test_runtime(test_runtime_state_t *state) {
  static const h2_pal_mem_vtable_t mem_vtable = {
      .alloc = test_alloc,
      .realloc = test_realloc,
      .free = test_free,
  };
  static const h2_pal_log_vtable_t log_vtable = {.write = test_log};
  static const h2_pal_time_vtable_t time_vtable = {
      .get_monotonic_ms = test_now,
      .sleep_ms = test_sleep,
  };
  static h2_pal_mem_api_t mem;
  static h2_pal_log_api_t log;
  static h2_pal_time_api_t time;
  mem = (h2_pal_mem_api_t){.user = state, .vtable = &mem_vtable};
  log = (h2_pal_log_api_t){.user = state, .vtable = &log_vtable};
  time = (h2_pal_time_api_t){.user = state, .vtable = &time_vtable};
  h2_runtime_t runtime = {0};
  runtime.mem = &mem;
  runtime.log = &log;
  runtime.time = &time;
  return runtime;
}

static size_t test_occurrences(const char *text, const char *needle) {
  size_t count = 0u;
  size_t needle_len = strlen(needle);
  for (const char *cursor = text; (cursor = strstr(cursor, needle)) != NULL;
       cursor += needle_len) {
    ++count;
  }
  return count;
}

static void test_success_and_defaults(void) {
  test_runtime_state_t state = {0};
  h2_runtime_t runtime = test_runtime(&state);
  assert(h2_libco_smoke_run(&runtime, NULL) == H2_PAL_OK);
  assert(state.live_allocations == 0u);
  assert(test_occurrences(state.log, "H2_LIBCO_SMOKE_STAGE") == 6u);
  assert(test_occurrences(state.log, "H2_LIBCO_SMOKE_PASS rc=0") == 1u);
  assert(strstr(state.log, "H2_LIBCO_SMOKE_FAIL") == NULL);
}

static void test_invalid_inputs(void) {
  test_runtime_state_t state = {0};
  h2_runtime_t runtime = test_runtime(&state);
  h2_libco_smoke_config_t config = {.task_stack_size = 1025u};
  assert(h2_libco_smoke_run(NULL, NULL) == H2_PAL_ERR_INVALID_ARG);
  assert(h2_libco_smoke_run(&runtime, &config) == H2_PAL_ERR_INVALID_ARG);
  config.task_stack_size = H2_LIBCO_SMOKE_MIN_STACK_SIZE / 2u;
  assert(h2_libco_smoke_run(&runtime, &config) == H2_PAL_ERR_INVALID_ARG);
  config.task_stack_size = H2_LIBCO_SMOKE_MAX_STACK_SIZE + 16u;
  assert(h2_libco_smoke_run(&runtime, &config) == H2_PAL_ERR_INVALID_ARG);
  config.task_stack_size = SIZE_MAX;
  assert(h2_libco_smoke_run(&runtime, &config) == H2_PAL_ERR_INVALID_ARG);
  config.task_stack_size = H2_LIBCO_SMOKE_DEFAULT_STACK_SIZE;
  config.switch_iterations = H2_LIBCO_SMOKE_MAX_SWITCH_ITERATIONS + 1u;
  assert(h2_libco_smoke_run(&runtime, &config) == H2_PAL_ERR_INVALID_ARG);
  assert(state.alloc_calls == 0u && state.live_allocations == 0u);
}

static void test_valid_boundaries(void) {
  const size_t stack_sizes[] = {
      H2_LIBCO_SMOKE_MIN_STACK_SIZE,
      H2_LIBCO_SMOKE_MAX_STACK_SIZE,
  };
  for (size_t index = 0u; index < sizeof(stack_sizes) / sizeof(stack_sizes[0]);
       ++index) {
    test_runtime_state_t state = {0};
    h2_runtime_t runtime = test_runtime(&state);
    const h2_libco_smoke_config_t config = {
        .task_stack_size = stack_sizes[index],
        .switch_iterations = 1u,
    };
    assert(h2_libco_smoke_run(&runtime, &config) == H2_PAL_OK);
    assert(state.live_allocations == 0u);
    assert(test_occurrences(state.log, "H2_LIBCO_SMOKE_PASS rc=0") == 1u);
  }
}

static void test_allocation_failures_cleanup(void) {
  h2_libco_smoke_config_t config = {
      .task_stack_size = H2_LIBCO_SMOKE_DEFAULT_STACK_SIZE,
      .switch_iterations = 32u,
  };
  test_runtime_state_t baseline = {0};
  h2_runtime_t runtime = test_runtime(&baseline);
  assert(h2_libco_smoke_run(&runtime, &config) == H2_PAL_OK);
  size_t allocation_count = baseline.alloc_calls;
  assert(allocation_count > 0u && baseline.live_allocations == 0u);

  for (size_t fail_call = 1u; fail_call <= allocation_count; ++fail_call) {
    test_runtime_state_t state = {.fail_alloc_call = fail_call};
    runtime = test_runtime(&state);
    assert(h2_libco_smoke_run(&runtime, &config) != H2_PAL_OK);
    assert(state.live_allocations == 0u);
    assert(strstr(state.log, "H2_LIBCO_SMOKE_PASS") == NULL);
    assert(test_occurrences(state.log, "H2_LIBCO_SMOKE_FAIL") == 1u);
  }
}

static void test_callback_failures(void) {
  h2_libco_smoke_config_t config = {
      .switch_iterations = 8u,
  };
  test_runtime_state_t time_state = {.fail_time_call = 2u};
  h2_runtime_t runtime = test_runtime(&time_state);
  assert(h2_libco_smoke_run(&runtime, &config) != H2_PAL_OK);
  assert(time_state.live_allocations == 0u);
  assert(strstr(time_state.log, "H2_LIBCO_SMOKE_PASS") == NULL);

  test_runtime_state_t permanent_time_state = {.fail_time_from_call = 2u};
  runtime = test_runtime(&permanent_time_state);
  assert(h2_libco_smoke_run(&runtime, &config) != H2_PAL_OK);
  assert(permanent_time_state.live_allocations == 0u);
  assert(strstr(permanent_time_state.log, "H2_LIBCO_SMOKE_PASS") == NULL);

  test_runtime_state_t regress_state = {
      .regress_time_call = 2u,
      .now_ms = 100u,
  };
  runtime = test_runtime(&regress_state);
  assert(h2_libco_smoke_run(&runtime, &config) != H2_PAL_OK);
  assert(regress_state.live_allocations == 0u);
  assert(strstr(regress_state.log, "H2_LIBCO_SMOKE_PASS") == NULL);

  test_runtime_state_t baseline = {0};
  runtime = test_runtime(&baseline);
  assert(h2_libco_smoke_run(&runtime, &config) == H2_PAL_OK);
  assert(baseline.log_calls > 0u && baseline.live_allocations == 0u);
  for (size_t fail_call = 1u; fail_call <= baseline.log_calls; ++fail_call) {
    test_runtime_state_t log_state = {.fail_log_call = fail_call};
    runtime = test_runtime(&log_state);
    assert(h2_libco_smoke_run(&runtime, &config) != H2_PAL_OK);
    assert(log_state.live_allocations == 0u);
    assert(strstr(log_state.log, "H2_LIBCO_SMOKE_PASS") == NULL);
    assert(test_occurrences(log_state.log, "H2_LIBCO_SMOKE_FAIL") == 1u);
  }
}

int main(void) {
  test_success_and_defaults();
  test_invalid_inputs();
  test_valid_boundaries();
  test_allocation_failures_cleanup();
  test_callback_failures();
  puts("libco smoke tests passed");
  return 0;
}
