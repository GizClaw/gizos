#include "h2_log_example.h"

#include <assert.h>
#include <string.h>

typedef struct test_log_state {
  size_t calls;
  h2_pal_log_level_t level;
  char scope[32];
  char message[32];
  int result;
} test_log_state_t;

static int test_log_write(void *user, h2_pal_log_level_t level,
                          const char *scope, const char *message) {
  test_log_state_t *state = (test_log_state_t *)user;
  assert(state != NULL);
  assert(scope != NULL);
  assert(message != NULL);
  ++state->calls;
  state->level = level;
  (void)strncpy(state->scope, scope, sizeof(state->scope) - 1u);
  (void)strncpy(state->message, message, sizeof(state->message) - 1u);
  return state->result;
}

static h2_runtime_t test_runtime(test_log_state_t *state) {
  static const h2_pal_log_vtable_t vtable = {
      .write = test_log_write,
  };
  static h2_pal_log_api_t log;
  log = (h2_pal_log_api_t){
      .user = state,
      .vtable = &vtable,
  };
  return (h2_runtime_t){
      .log = &log,
  };
}

static void test_success(void) {
  test_log_state_t state = {0};
  h2_runtime_t runtime = test_runtime(&state);

  assert(h2_log_example_run(&runtime) == H2_PAL_OK);
  assert(state.calls == 1u);
  assert(state.level == H2_PAL_LOG_INFO);
  assert(strcmp(state.scope, "log") == 0);
  assert(strcmp(state.message, "Hello World") == 0);
}

static void test_invalid_runtime(void) {
  static const h2_pal_log_vtable_t missing_write_vtable = {0};
  h2_runtime_t runtime = {0};
  h2_pal_log_api_t log = {0};

  assert(h2_log_example_run(NULL) == H2_PAL_ERR_INVALID_ARG);
  assert(h2_log_example_run(&runtime) == H2_PAL_ERR_INVALID_ARG);
  runtime.log = &log;
  assert(h2_log_example_run(&runtime) == H2_PAL_ERR_INVALID_ARG);
  log.vtable = &missing_write_vtable;
  assert(h2_log_example_run(&runtime) == H2_PAL_ERR_INVALID_ARG);
}

static void test_provider_failure(void) {
  test_log_state_t state = {
      .result = H2_PAL_ERR_IO,
  };
  h2_runtime_t runtime = test_runtime(&state);

  assert(h2_log_example_run(&runtime) == H2_PAL_ERR_IO);
  assert(state.calls == 1u);
}

int main(void) {
  test_success();
  test_invalid_runtime();
  test_provider_failure();
  return 0;
}
