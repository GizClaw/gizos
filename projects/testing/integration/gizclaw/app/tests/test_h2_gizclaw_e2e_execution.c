#include "h2_gizclaw_e2e_execution.h"

#include "h2_app_test_mem.h"
#include "h2_app_test_sync.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <string.h>

static h2_app_test_mem_t allocator;
static h2_app_test_sync_t sync;
static char output[16384];
static size_t output_size, output_calls, fail_output_at, run_calls;
static h2_gizclaw_e2e_result_t run_result;
static h2_gizclaw_e2e_exit_t run_exit;
static bool malformed_evidence;
static h2_gizclaw_e2e_evidence_fn retained_observer;
static void* retained_user;
static const char record[] = "H2_GIZCLAW_E2E stage=api api=test rc=0\n";
static const char diagnostic[] = "H2_GIZCLAW_E2E stage=device-api rc=0\n";
static const char boot[] =
    "H2_GIZCLAW_BOOT board=test_board version=test-v1 "
    "execution=000102030405060708090a0b0c0d0e0f\n";
static const char summary[] =
    "H2_GIZCLAW_E2E stage=summary platform=test_platform "
    "endpoint=example.invalid:9821 backend=h2peer suite=all profile=fixture "
    "selected=8 terminal=8 pass=8 fail=0 error=0 blocked=0 cancelled=0 "
    "first_failure_case=- first_failure_rc=0 cleanup_rc=0 retained_resources=0 "
    "complete=true exit_code=0\n";

static int output_write(void* user, h2_pal_log_level_t level, const char* scope,
                        const char* message) {
  (void)user;
  assert(level == H2_PAL_LOG_INFO && !strcmp(scope, "gizclaw-e2e"));
  ++output_calls;
  if (output_calls == fail_output_at) return H2_PAL_ERR_IO;
  const size_t size = strlen(message);
  assert(size != 0u && message[size - 1u] == '\n');
  assert(size < sizeof(output) - output_size);
  memcpy(output + output_size, message, size + 1u);
  output_size += size;
  return H2_PAL_OK;
}

static int random_bytes(void* user, uint8_t* out, size_t size) {
  (void)user;
  assert(size == 16u);
  for (size_t i = 0u; i < size; ++i) out[i] = (uint8_t)i;
  return H2_PAL_OK;
}

static int firmware_current(void* user, h2_pal_firmware_info_t* info) {
  (void)user;
  strcpy(info->version, "test-v1");
  return H2_PAL_OK;
}

static const h2_pal_log_vtable_t log_vtable = {.write = output_write};
static const h2_pal_log_api_t log_api = {.vtable = &log_vtable};
static const h2_pal_crypto_vtable_t crypto_vtable = {.random = random_bytes};
static const h2_pal_crypto_api_t crypto_api = {.vtable = &crypto_vtable};
static const h2_pal_firmware_info_vtable_t firmware_vtable = {
    .get_current = firmware_current};
static const h2_pal_firmware_info_api_t firmware_api = {.vtable =
                                                            &firmware_vtable};
static h2_runtime_t runtime;
static const h2_gizclaw_e2e_config_t config = {
    .server_endpoint = {"example.invalid:9821", 20u},
    .registration_token = {"test-token", 10u},
    .suites = H2_GIZCLAW_E2E_SUITE_ALL};
static const h2_gizclaw_e2e_execution_config_t evidence = {
    .output = &log_api,
    .board = "test_board",
    .platform = "test_platform",
    .physical_audio = true};

/* Exercise the extracted production capture/qualification/replay path without
 * network cases. The real case runner's cleanup is tested by runner_test. */
h2_gizclaw_e2e_exit_t h2_gizclaw_e2e_run(
    h2_runtime_t* supplied_runtime, const h2_gizclaw_e2e_config_t* supplied,
    h2_gizclaw_e2e_result_t* result) {
  assert(supplied_runtime == &runtime && supplied->on_evidence != NULL);
  assert(strncmp(output, boot, sizeof(boot) - 1u) == 0);
  ++run_calls;
  supplied->on_evidence(supplied->evidence_user, record, sizeof(record) - 1u);
  /* Successful API traffic stays captured, with only the old bounded whitelist
   * visible while the App is running. */
  assert(!strcmp(output, boot));
  supplied->on_evidence(supplied->evidence_user, diagnostic,
                        sizeof(diagnostic) - 1u);
  assert(strstr(output,
                "H2_GIZCLAW_DIAGNOSTIC H2_GIZCLAW_E2E stage=device-api ") !=
         NULL);
  if (malformed_evidence)
    supplied->on_evidence(supplied->evidence_user, NULL, 0u);
  if (run_result.retained_resources != 0u) {
    retained_observer = supplied->on_evidence;
    retained_user = supplied->evidence_user;
  }
  *result = run_result;
  return run_exit;
}

static void setup(void) {
  h2_app_test_mem_init(&allocator, NULL);
  h2_app_test_sync_init(&sync);
  runtime = (h2_runtime_t){.mem = &allocator.api,
                           .sync = &sync.api,
                           .crypto = &crypto_api,
                           .firmware_info = &firmware_api};
  output[0] = '\0';
  output_size = output_calls = fail_output_at = run_calls = 0u;
  malformed_evidence = false;
  run_exit = H2_GIZCLAW_E2E_EXIT_PASS;
  run_result = (h2_gizclaw_e2e_result_t){
      .selected = 8u, .terminal = 8u, .passed = 8u, .complete = true};
  strcpy(run_result.runtime_profile_name, "fixture");
}

static h2_gizclaw_e2e_execution_t* create(void) {
  h2_gizclaw_e2e_execution_t* execution = NULL;
  assert(h2_gizclaw_e2e_execution_create(&runtime, &config, &evidence,
                                         &execution) == H2_PAL_OK);
  assert(execution != NULL && output_size == 0u);
  assert(!h2_gizclaw_e2e_execution_qualified(execution));
  return execution;
}

static void destroy(h2_gizclaw_e2e_execution_t** execution) {
  assert(h2_gizclaw_e2e_execution_destroy(execution) == H2_PAL_OK);
  assert(*execution == NULL && allocator.live_blocks == 0u);
}

static void test_replay_and_confirmation_order(void) {
  setup();
  h2_gizclaw_e2e_execution_t* execution = create();
  assert(h2_gizclaw_e2e_execution_publish(execution, H2_PAL_OK) ==
         H2_PAL_ERR_INVALID_STATE);
  assert(h2_gizclaw_e2e_execution_replay(execution) ==
         H2_PAL_ERR_INVALID_STATE);
  assert(h2_gizclaw_e2e_execution_run(execution) == H2_GIZCLAW_E2E_EXIT_PASS);
  assert(h2_gizclaw_e2e_execution_qualified(execution));
  assert(run_calls == 1u && strstr(output, "stage=summary") == NULL);
  const size_t before = output_size;
  assert(h2_gizclaw_e2e_execution_run(execution) ==
         H2_GIZCLAW_E2E_EXIT_HARNESS_ERROR);
  assert(run_calls == 1u && output_size == before);
  assert(h2_gizclaw_e2e_execution_publish(execution, H2_PAL_OK) == H2_PAL_OK);
  assert(!strncmp(output + before, summary, sizeof(summary) - 1u));
  assert(
      !strcmp(output + before + sizeof(summary) - 1u,
              "H2_GIZCLAW_READY board=test_board rc=0 confirm=0 admitted=1\n"));
  const size_t replay_offset = output_size;
  assert(h2_gizclaw_e2e_execution_replay(execution) == H2_PAL_OK);
  const char* begin = output + replay_offset;
  assert(strstr(begin,
                "H2_GIZCLAW_LEDGER stage=begin version=test-v1 "
                "execution=000102030405060708090a0b0c0d0e0f bytes=") == begin);
  assert(strstr(begin, " records=3 crc32=") != NULL);
  assert(strstr(begin, " admitted=1 confirm_rc=0 physical_audio=1\n") != NULL);
  const char* body = strchr(begin, '\n') + 1u;
  assert(!strncmp(body, record, sizeof(record) - 1u));
  body += sizeof(record) - 1u;
  assert(!strncmp(body, diagnostic, sizeof(diagnostic) - 1u));
  body += sizeof(diagnostic) - 1u;
  assert(!strncmp(body, summary, sizeof(summary) - 1u));
  body += sizeof(summary) - 1u;
  assert(strstr(body, "H2_GIZCLAW_LEDGER stage=end execution=") == body);
  const size_t replay_size = output_size - replay_offset;
  assert(h2_gizclaw_e2e_execution_replay(execution) == H2_PAL_OK);
  assert(output_size == replay_offset + replay_size * 2u);
  assert(!memcmp(output + replay_offset, output + replay_offset + replay_size,
                 replay_size));
  assert(h2_gizclaw_e2e_execution_publish(execution, H2_PAL_OK) ==
         H2_PAL_ERR_INVALID_STATE);
  destroy(&execution);
}

static void test_qualification_gates(void) {
  for (unsigned gate = 0u; gate < 3u; ++gate) {
    setup();
    switch (gate) {
      case 0:
        run_result.complete = false;
        break;
      case 1:
        run_exit = H2_GIZCLAW_E2E_EXIT_CASE_FAILURE;
        break;
      case 2:
        malformed_evidence = true;
        break;
    }
    h2_gizclaw_e2e_execution_t* execution = create();
    assert(h2_gizclaw_e2e_execution_run(execution) == run_exit);
    assert(!h2_gizclaw_e2e_execution_qualified(execution));
    assert(h2_gizclaw_e2e_execution_publish(execution, H2_PAL_OK) ==
           H2_PAL_ERR_INVALID_STATE);
    assert(h2_gizclaw_e2e_execution_publish(
               execution, H2_PAL_ERR_INVALID_STATE) == H2_PAL_OK);
    assert(strstr(output, "admitted=1") == NULL);
    const size_t before = output_size;
    assert(h2_gizclaw_e2e_execution_replay(execution) == H2_PAL_OK);
    if (malformed_evidence) assert(output_size == before);
    destroy(&execution);
  }
}

static void test_output_and_partial_initialization(void) {
  setup();
  h2_gizclaw_e2e_execution_t* execution = NULL;
  sync.create.skip = 1u;
  sync.create.result = H2_PAL_ERR_NO_MEMORY;
  sync.create.remaining = 1u;
  assert(h2_gizclaw_e2e_execution_create(&runtime, &config, &evidence,
                                         &execution) == H2_PAL_ERR_NO_MEMORY);
  assert(execution == NULL && allocator.live_blocks == 0u);
  execution = create();
  fail_output_at = 1u;
  assert(h2_gizclaw_e2e_execution_run(execution) ==
         H2_GIZCLAW_E2E_EXIT_HARNESS_ERROR);
  assert(run_calls == 0u && !h2_gizclaw_e2e_execution_qualified(execution));
  destroy(&execution);

  setup();
  execution = create();
  assert(h2_gizclaw_e2e_execution_run(execution) == H2_GIZCLAW_E2E_EXIT_PASS);
  assert(h2_gizclaw_e2e_execution_publish(execution, H2_PAL_ERR_IO) ==
         H2_PAL_OK);
  assert(strstr(output, "confirm=-") != NULL &&
         strstr(output, "admitted=1") == NULL);
  const size_t replay_offset = output_size;
  fail_output_at = output_calls + 2u;
  assert(h2_gizclaw_e2e_execution_replay(execution) == H2_PAL_ERR_IO);
  assert(!h2_gizclaw_e2e_execution_qualified(execution));
  assert(strstr(output + replay_offset, "stage=begin") != NULL);
  assert(strstr(output + replay_offset, "stage=end") == NULL);
  assert(h2_gizclaw_e2e_execution_replay(execution) == H2_PAL_OK);
  assert(strstr(output + replay_offset, "stage=end") != NULL);
  sync.destroy.result = H2_PAL_ERR_BUSY;
  sync.destroy.remaining = 1u;
  assert(h2_gizclaw_e2e_execution_destroy(&execution) == H2_PAL_ERR_BUSY);
  assert(execution != NULL && allocator.live_blocks != 0u);
  destroy(&execution);
}

static void test_retained_capture_survives_return(void) {
  setup();
  run_result.retained_resources = 1u;
  run_exit = H2_GIZCLAW_E2E_EXIT_HARNESS_ERROR;
  h2_gizclaw_e2e_execution_t* execution = create();
  assert(h2_gizclaw_e2e_execution_run(execution) == run_exit);
  assert(!h2_gizclaw_e2e_execution_qualified(execution));
  assert(h2_gizclaw_e2e_execution_publish(execution, H2_PAL_OK) ==
         H2_PAL_ERR_INVALID_STATE);
  assert(h2_gizclaw_e2e_execution_publish(
             execution, H2_PAL_ERR_INVALID_STATE) == H2_PAL_OK);
  const size_t before = output_size;
  retained_observer(retained_user, record, sizeof(record) - 1u);
  assert(output_size == before && allocator.live_blocks != 0u);
  assert(h2_gizclaw_e2e_execution_replay(execution) == H2_PAL_OK);
  assert(output_size == before);
  assert(h2_gizclaw_e2e_execution_destroy(&execution) == H2_PAL_ERR_BUSY);
  assert(execution != NULL);
  assert(h2_gizclaw_e2e_execution_run(execution) ==
         H2_GIZCLAW_E2E_EXIT_HARNESS_ERROR);
  assert(run_calls == 1u);
  /* The production contract intentionally retains this observer and all its
   * borrows until process teardown. Keep the fake Runtime static for that span.
   */
}

int main(void) {
  test_replay_and_confirmation_order();
  test_qualification_gates();
  test_output_and_partial_initialization();
  test_retained_capture_survives_return();
  return 0;
}
