#include "h2_gizclaw_e2e.h"
#include "h2_gizclaw_e2e_concurrency.h"
#include "h2_gizclaw_e2e_rpc.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

typedef struct test_state {
  uint64_t now_ms;
  size_t progress_records;
  size_t task_join_calls;
  size_t task_join_failures;
  int task_start_rc;
  h2_pal_task_entry_t task_entry;
  void *task_context;
  bool in_task;
  bool task_exited;
  bool stop;
} test_state_t;

static int s_mutex_storage;
static int s_task_storage;

static void *test_alloc(void *user, size_t len) {
  (void)user;
  return malloc(len);
}

static void *test_realloc(void *user, void *ptr, size_t len) {
  (void)user;
  return realloc(ptr, len);
}

static void test_free(void *user, void *ptr) {
  (void)user;
  free(ptr);
}

static h2_pal_result_t test_random(void *user, uint8_t *out, size_t len) {
  (void)user;
  memset(out, 0x5a, len);
  return H2_PAL_OK;
}

static h2_pal_result_t test_time_monotonic(void *user, uint64_t *out_ms) {
  test_state_t *state = user;
  *out_ms = state->now_ms++;
  return H2_PAL_OK;
}

static h2_pal_result_t test_sleep(void *user, uint32_t ms) {
  test_state_t *state = user;
  state->now_ms += ms;
  return H2_PAL_OK;
}

static h2_pal_result_t test_mutex_create(
    void *user, const h2_pal_mutex_config_t *config,
    h2_pal_mutex_t **out_mutex) {
  (void)user;
  (void)config;
  *out_mutex = (h2_pal_mutex_t *)&s_mutex_storage;
  return H2_PAL_OK;
}

static h2_pal_result_t test_mutex_operation(void *user,
                                            h2_pal_mutex_t *mutex) {
  (void)user;
  (void)mutex;
  return H2_PAL_OK;
}

static int test_task_start(void *user, const h2_pal_task_options_t *options,
                           h2_pal_task_entry_t entry, void *context,
                           h2_pal_task_t **out_task) {
  (void)options;
  test_state_t *state = user;
  state->task_entry = entry;
  state->task_context = context;
  state->task_exited = false;
  if (state->task_start_rc != H2_PAL_OK) {
    return state->task_start_rc;
  }
  *out_task = (h2_pal_task_t *)&s_task_storage;
  state->in_task = true;
  entry(context);
  state->in_task = false;
  state->task_exited = true;
  return H2_PAL_OK;
}

static int test_task_join(void *user, h2_pal_task_t *task) {
  test_state_t *state = user;
  (void)task;
  assert(state->task_exited);
  state->task_join_calls++;
  if (state->task_join_failures > 0u) {
    state->task_join_failures--;
    return H2_PAL_ERR_TASK;
  }
  return H2_PAL_OK;
}

static int test_log(void *user, h2_pal_log_level_t level, const char *scope,
                    const char *message) {
  (void)user;
  (void)level;
  (void)scope;
  (void)message;
  return H2_PAL_OK;
}

static bool test_should_stop(void *user) {
  const test_state_t *state = user;
  return state->stop;
}

static void test_progress(void *user,
                          const h2_gizclaw_e2e_progress_t *progress) {
  test_state_t *state = user;
  assert(progress != NULL);
  assert(!state->in_task);
  state->progress_records++;
}

static h2_runtime_t test_runtime(test_state_t *state) {
  static const h2_pal_mem_vtable_t mem_vtable = {
      .alloc = test_alloc,
      .realloc = test_realloc,
      .free = test_free,
  };
  static const h2_pal_mem_api_t mem = {
      .vtable = &mem_vtable,
  };
  static const h2_pal_crypto_vtable_t crypto_vtable = {
      .random = test_random,
  };
  static const h2_pal_crypto_api_t crypto = {
      .vtable = &crypto_vtable,
  };
  static const h2_pal_http_vtable_t http_vtable = {0};
  static const h2_pal_http_api_t http = {
      .vtable = &http_vtable,
  };
  static const h2_pal_webrtc_vtable_t webrtc_vtable = {0};
  static const h2_pal_webrtc_api_t webrtc = {
      .vtable = &webrtc_vtable,
  };
  static const h2_pal_sync_vtable_t sync_vtable = {
      .create_mutex = test_mutex_create,
      .destroy_mutex = test_mutex_operation,
      .lock_mutex = test_mutex_operation,
      .try_lock_mutex = test_mutex_operation,
      .unlock_mutex = test_mutex_operation,
  };
  static const h2_pal_sync_api_t sync = {
      .vtable = &sync_vtable,
  };
  static const h2_pal_task_vtable_t task_vtable = {
      .start = test_task_start,
      .join = test_task_join,
  };
  static h2_pal_task_api_t task = {
      .vtable = &task_vtable,
  };
  static const h2_pal_queue_vtable_t queue_vtable = {0};
  static const h2_pal_queue_api_t queue = {
      .vtable = &queue_vtable,
  };
  static const h2_pal_log_vtable_t log_vtable = {
      .write = test_log,
  };
  static const h2_pal_log_api_t log = {
      .vtable = &log_vtable,
  };
  static h2_pal_time_vtable_t time_vtable = {
      .get_monotonic_ms = test_time_monotonic,
      .sleep_ms = test_sleep,
  };
  static h2_pal_time_api_t time = {
      .vtable = &time_vtable,
  };
  time.user = state;
  task.user = state;
  return (h2_runtime_t){
      .mem = &mem,
      .log = &log,
      .time = &time,
      .task = &task,
      .queue = &queue,
      .sync = &sync,
      .crypto = &crypto,
      .http = &http,
      .webrtc = &webrtc,
  };
}

static h2_gizclaw_e2e_config_t test_config(test_state_t *state, uint8_t *pcm,
                                           size_t pcm_len) {
  static const char endpoint[] = "e2e.gizclaw.com:9821";
  static const char token[] = "test-registration-token";
  return (h2_gizclaw_e2e_config_t){
      .server_endpoint = {endpoint, sizeof(endpoint) - 1u},
      .registration_token = {token, sizeof(token) - 1u},
      .voice_pcm_s16le_16khz_mono = pcm,
      .voice_pcm_len = pcm_len,
      .suites = H2_GIZCLAW_E2E_SUITE_ALL,
      .case_timeout_ms = 1000u,
      .cleanup_timeout_ms = 1000u,
      .progress_interval_ms = 10000u,
      .should_stop = test_should_stop,
      .should_stop_user = state,
      .on_progress = test_progress,
      .progress_user = state,
  };
}

int main(void) {
  h2_gizclaw_workflow_t default_workflows[] = {
      {.name = "general-assistant"},
      {.name = "doubao-realtime"},
  };
  h2_gizclaw_workflow_page_t default_page = {
      .items = default_workflows,
      .count = sizeof(default_workflows) / sizeof(default_workflows[0]),
  };
  char workflow_name[64];
  assert(h2_gizclaw_e2e_select_workflow_name(
             &default_page, workflow_name, sizeof(workflow_name)) == H2_PAL_OK);
  assert(strcmp(workflow_name, "doubao-realtime") == 0);
  h2_gizclaw_workflow_t single_workflow[] = {{.name = "chat"}};
  h2_gizclaw_workflow_page_t single_page = {
      .items = single_workflow,
      .count = 1u,
  };
  assert(h2_gizclaw_e2e_select_workflow_name(
             &single_page, workflow_name, sizeof(workflow_name)) == H2_PAL_OK);
  assert(strcmp(workflow_name, "chat") == 0);
  h2_gizclaw_workflow_page_t empty_page = {0};
  assert(h2_gizclaw_e2e_select_workflow_name(&empty_page, workflow_name,
                                             sizeof(workflow_name)) ==
         H2_PAL_ERR_NOT_FOUND);
  h2_gizclaw_workflow_page_t malformed_page = {.count = 1u};
  assert(h2_gizclaw_e2e_select_workflow_name(&malformed_page, workflow_name,
                                             sizeof(workflow_name)) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(h2_gizclaw_e2e_select_workflow_name(&single_page, workflow_name, 4u) ==
         H2_PAL_ERR_TRUNCATED);

  h2_gizclaw_workspace_t workspace_response = {
      .name = "workspace-1",
      .available = true,
  };
  assert(h2_gizclaw_e2e_workspace_response_ready(&workspace_response,
                                                 "workspace-1"));
  workspace_response.available = false;
  assert(!h2_gizclaw_e2e_workspace_response_ready(&workspace_response,
                                                  "workspace-1"));
  workspace_response.available = true;
  assert(!h2_gizclaw_e2e_workspace_response_ready(&workspace_response,
                                                  "workspace-2"));
  workspace_response.name = NULL;
  assert(!h2_gizclaw_e2e_workspace_response_ready(&workspace_response,
                                                  "workspace-1"));

  assert(h2_gizclaw_e2e_concurrency_classify(H2_PAL_OK, H2_PAL_OK, H2_PAL_OK,
                                             3u, 3u, 3u, 3u, 0u) == H2_PAL_OK);
  assert(h2_gizclaw_e2e_concurrency_classify(H2_PAL_ERR_TIMEOUT, H2_PAL_OK,
                                             H2_PAL_OK, 2u, 0u, 2u, 2u,
                                             0u) == H2_PAL_ERR_TIMEOUT);
  assert(h2_gizclaw_e2e_concurrency_classify(H2_PAL_OK, H2_PAL_ERR_IO,
                                             H2_PAL_OK, 3u, 3u, 3u, 3u,
                                             0u) == H2_PAL_ERR_IO);
  assert(h2_gizclaw_e2e_concurrency_classify(H2_PAL_OK, H2_PAL_OK, H2_PAL_OK,
                                             3u, 3u, 2u, 3u, 0u) == H2_PAL_OK);
  assert(h2_gizclaw_e2e_concurrency_classify(H2_PAL_OK, H2_PAL_OK, H2_PAL_OK,
                                             3u, 3u, 4u, 3u,
                                             0u) == H2_PAL_ERR_INVALID_STATE);
  assert(h2_gizclaw_e2e_concurrency_classify(H2_PAL_OK, H2_PAL_OK, H2_PAL_OK,
                                             3u, 3u, 0u, 3u,
                                             0u) == H2_PAL_ERR_INVALID_STATE);
  assert(h2_gizclaw_e2e_concurrency_classify(H2_PAL_OK, H2_PAL_OK, H2_PAL_OK,
                                             3u, 3u, 3u, 2u,
                                             0u) == H2_PAL_ERR_INVALID_STATE);
  assert(h2_gizclaw_e2e_concurrency_classify(H2_PAL_OK, H2_PAL_OK, H2_PAL_OK,
                                             3u, 3u, 3u, 3u,
                                             1u) == H2_PAL_ERR_INVALID_STATE);

  h2_gizclaw_e2e_result_t result;
  memset(&result, 0xa5, sizeof(result));
  assert(h2_gizclaw_e2e_run(NULL, NULL, &result) ==
         H2_GIZCLAW_E2E_EXIT_HARNESS_ERROR);
  h2_gizclaw_e2e_result_t empty = {0};
  assert(memcmp(&result, &empty, sizeof(result)) == 0);
  assert(h2_gizclaw_e2e_run(NULL, NULL, NULL) ==
         H2_GIZCLAW_E2E_EXIT_HARNESS_ERROR);

  test_state_t state = {0};
  h2_runtime_t runtime = test_runtime(&state);
  uint8_t pcm[] = {1u, 0u};
  h2_gizclaw_e2e_config_t config = test_config(&state, pcm, sizeof(pcm));
  h2_runtime_t missing_queue_runtime = runtime;
  missing_queue_runtime.queue = NULL;
  config.suites = H2_GIZCLAW_E2E_SUITE_SERVICE;
  assert(h2_gizclaw_e2e_run(&missing_queue_runtime, &config, &result) ==
         H2_GIZCLAW_E2E_EXIT_HARNESS_ERROR);
  assert(memcmp(&result, &empty, sizeof(result)) == 0);
  config.suites = H2_GIZCLAW_E2E_SUITE_ALL;

  state.task_join_failures = 1u;
  assert(h2_gizclaw_e2e_run(&runtime, &config, &result) ==
         H2_GIZCLAW_E2E_EXIT_CASE_FAILURE);
  assert(result.selected == 6u);
  assert(result.terminal == 6u);
  assert(result.failed == 6u);
  assert(result.errors == 0u);
  assert(result.cleanup_rc == H2_PAL_OK);
  assert(result.complete);
  assert(state.progress_records == 8u);
  assert(state.task_join_calls == 2u);

  state.stop = true;
  state.progress_records = 0u;
  assert(h2_gizclaw_e2e_run(&runtime, &config, &result) ==
         H2_GIZCLAW_E2E_EXIT_HARNESS_ERROR);
  assert(result.selected == 6u);
  assert(result.terminal == 6u);
  assert(result.cancelled == 6u);
  assert(result.complete);
  assert(state.progress_records == 8u);

  state.stop = false;
  state.progress_records = 0u;
  state.task_start_rc = H2_PAL_ERR_TASK;
  assert(h2_gizclaw_e2e_run(&runtime, &config, &result) ==
         H2_GIZCLAW_E2E_EXIT_HARNESS_ERROR);
  assert(result.selected == 6u);
  assert(result.terminal == 6u);
  assert(result.errors == 6u);
  assert(result.cleanup_rc == H2_PAL_ERR_TASK);
  assert(result.complete);
  assert(result.retained_resources == 0u);
  assert(state.progress_records == 8u);

  state.progress_records = 0u;
  state.task_start_rc = H2_PAL_OK;
  state.task_join_failures = 100u;
  config.cleanup_timeout_ms = 20u;
  const size_t joins_before_timeout = state.task_join_calls;
  assert(h2_gizclaw_e2e_run(&runtime, &config, &result) ==
         H2_GIZCLAW_E2E_EXIT_HARNESS_ERROR);
  assert(result.complete);
  assert(result.cleanup_rc == H2_PAL_ERR_TIMEOUT);
  assert(result.retained_resources == 3u);
  assert(state.task_join_calls == joins_before_timeout + 2u);
  assert(state.task_exited);

  memset(&result, 0xa5, sizeof(result));
  assert(h2_gizclaw_e2e_run(&runtime, &config, &result) ==
         H2_GIZCLAW_E2E_EXIT_HARNESS_ERROR);
  assert(memcmp(&result, &empty, sizeof(result)) == 0);
  return 0;
}
