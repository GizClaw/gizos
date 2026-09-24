#include "h2_app_test_mem.h"
#include "h2_app_test_time.h"
#include "h2_app_test_sync.h"
#include "h2_app_test_task.h"
#include "h2_app_test_crypto.h"
#include "h2_gizclaw_e2e.h"
#include "h2_gizclaw_e2e_catalog.h"
#include "h2_gizclaw_e2e_concurrency.h"
#include "h2_gizclaw_e2e_rpc.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdlib.h>
#include <string.h>

typedef struct test_state {
  h2_app_test_mem_t mem;
  h2_app_test_time_t clock;
  h2_app_test_sync_t sync;
  h2_app_test_task_t task;
  h2_app_test_crypto_t crypto;
  uint8_t entropy[16384];
  size_t progress_records;
  bool stop;
} test_state_t;

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
  assert(!state->task.running);
  state->progress_records++;
}

static h2_runtime_t test_runtime(test_state_t *state) {
  h2_app_test_mem_init(&state->mem, NULL);
  h2_app_test_time_init(&state->clock, 0u);
  state->clock.advance_per_read_ms = 1u;
  h2_app_test_sync_init(&state->sync);
  h2_app_test_task_init(&state->task);
  state->task.run_on_start = true;
  state->task.join.result = H2_PAL_ERR_TASK;
  h2_app_test_crypto_init(&state->crypto);
  memset(state->entropy, 0x5a, sizeof(state->entropy));
  state->crypto.random_bytes = state->entropy;
  state->crypto.random_size = sizeof(state->entropy);
  static const h2_pal_http_vtable_t http_vtable = {0};
  static const h2_pal_http_api_t http = {
      .vtable = &http_vtable,
  };
  static const h2_pal_webrtc_vtable_t webrtc_vtable = {0};
  static const h2_pal_webrtc_api_t webrtc = {
      .vtable = &webrtc_vtable,
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
  return (h2_runtime_t){
      .mem = &state->mem.api,
      .log = &log,
      .time = &state->clock.api,
      .task = &state->task.api,
      .queue = &queue,
      .sync = &state->sync.api,
      .crypto = &state->crypto.api,
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

  assert(h2_gizclaw_e2e_case_count == 8u);
  assert((config.suites & H2_GIZCLAW_E2E_SUITE_DEVICE) != 0);
  state.task.join.remaining = 1u;
  assert(h2_gizclaw_e2e_run(&runtime, &config, &result) ==
         H2_GIZCLAW_E2E_EXIT_CASE_FAILURE);
  assert(result.selected == 8u);
  assert(result.terminal == 8u);
  assert(result.failed == 8u);
  assert(result.errors == 0u);
  assert(result.cleanup_rc == H2_PAL_OK);
  assert(result.complete);
  assert(state.progress_records == 10u);
  assert(state.task.join.calls == 2u);

  state.stop = true;
  state.progress_records = 0u;
  assert(h2_gizclaw_e2e_run(&runtime, &config, &result) ==
         H2_GIZCLAW_E2E_EXIT_HARNESS_ERROR);
  assert(result.selected == 8u);
  assert(result.terminal == 8u);
  assert(result.cancelled == 8u);
  assert(result.complete);
  assert(state.progress_records == 10u);

  state.stop = false;
  state.progress_records = 0u;
  state.task.start = (h2_app_test_fault_t){.result=H2_PAL_ERR_TASK, .remaining=1u};
  assert(h2_gizclaw_e2e_run(&runtime, &config, &result) ==
         H2_GIZCLAW_E2E_EXIT_HARNESS_ERROR);
  assert(result.selected == 8u);
  assert(result.terminal == 8u);
  assert(result.errors == 8u);
  assert(result.cleanup_rc == H2_PAL_ERR_TASK);
  assert(result.complete);
  assert(result.retained_resources == 0u);
  assert(state.progress_records == 10u);

  state.progress_records = 0u;
  state.task.start.remaining = 0u;
  state.task.join.remaining = 100u;
  config.cleanup_timeout_ms = 20u;
  const size_t joins_before_timeout = state.task.join.calls;
  assert(h2_gizclaw_e2e_run(&runtime, &config, &result) ==
         H2_GIZCLAW_E2E_EXIT_HARNESS_ERROR);
  assert(result.complete);
  assert(result.cleanup_rc == H2_PAL_ERR_TIMEOUT);
  assert(result.retained_resources == 3u);
  assert(state.task.join.calls == joins_before_timeout + 2u);
  assert(state.task.complete);

  memset(&result, 0xa5, sizeof(result));
  assert(h2_gizclaw_e2e_run(&runtime, &config, &result) ==
         H2_GIZCLAW_E2E_EXIT_HARNESS_ERROR);
  assert(memcmp(&result, &empty, sizeof(result)) == 0);
  h2_app_test_mem_release_all(&state.mem);
  return 0;
}
