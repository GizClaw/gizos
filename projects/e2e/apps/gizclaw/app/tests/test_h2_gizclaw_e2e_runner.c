#include "h2_gizclaw_e2e_internal.h"
#include "h2_gizclaw_e2e_catalog.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdlib.h>
#include <string.h>

/* Isolate the runner from network, RPC and audio cases. These doubles test
 * ownership/reporting only, never provide live-case acceptance evidence. */
static void *s_allocations[16];
static size_t s_live, s_connected, s_ran, s_cleaned;
static bool s_fail_deinit;
static uint64_t s_now;
static int s_mutex, s_task;
static h2_gizclaw_e2e_fixture_t *s_retained;

static void *allocate(void *user, size_t len) {
  (void)user;
  for (size_t i = 0; i < 16; ++i) {
    if (s_allocations[i] == NULL) {
      s_allocations[i] = malloc(len);
      assert(s_allocations[i] != NULL);
      ++s_live;
      return s_allocations[i];
    }
  }
  assert(false);
  return NULL;
}

static void release(void *user, void *ptr) {
  (void)user;
  if (ptr == NULL)
    return;
  for (size_t i = 0; i < 16; ++i) {
    if (s_allocations[i] == ptr) {
      assert(ptr != s_retained);
      free(ptr);
      s_allocations[i] = NULL;
      --s_live;
      return;
    }
  }
  assert(false);
}

static int monotonic(void *user, uint64_t *out) {
  (void)user;
  *out = ++s_now;
  return H2_PAL_OK;
}

static int sleep_ms(void *user, uint32_t ms) {
  (void)user;
  s_now += ms;
  return H2_PAL_OK;
}

static int mutex_create(void *user, const h2_pal_mutex_config_t *config,
                        h2_pal_mutex_t **out) {
  (void)user;
  (void)config;
  *out = (h2_pal_mutex_t *)&s_mutex;
  return H2_PAL_OK;
}

static int mutex_op(void *user, h2_pal_mutex_t *mutex) {
  (void)user;
  assert(mutex == (h2_pal_mutex_t *)&s_mutex);
  return H2_PAL_OK;
}

static int task_start(void *user, const h2_pal_task_options_t *options,
                      h2_pal_task_entry_t entry, void *context,
                      h2_pal_task_t **out) {
  (void)user;
  (void)options;
  *out = (h2_pal_task_t *)&s_task;
  entry(context);
  return H2_PAL_OK;
}

static int task_join(void *user, h2_pal_task_t *task) {
  (void)user;
  assert(task == (h2_pal_task_t *)&s_task);
  return H2_PAL_OK;
}

int h2_gizclaw_e2e_fixture_init(h2_gizclaw_e2e_fixture_t *fixture,
                                h2_runtime_t *runtime,
                                const h2_gizclaw_e2e_config_t *config,
                                uint32_t timeout) {
  assert(timeout > 0);
  fixture->runtime = runtime;
  fixture->config = config;
  strcpy(fixture->runtime_profile_name, "mock-runtime-profile");
  return H2_PAL_OK;
}

int h2_gizclaw_e2e_fixture_connect_actors(h2_gizclaw_e2e_fixture_t *fixture,
                                          size_t count) {
  /* The full catalog selects Connectivity then Service; the dedicated
   * catalog runs Connectivity again during the failed-teardown probe. */
  const size_t expected = h2_gizclaw_e2e_case_count == 6u && s_connected == 1u
                              ? 1u : 2u;
  assert(fixture != NULL && count == expected);
  ++s_connected;
  return H2_PAL_OK;
}

int h2_gizclaw_e2e_fixture_set_deadline(h2_gizclaw_e2e_fixture_t *fixture,
                                        uint32_t timeout) {
  assert(fixture != NULL && timeout > 0u);
  return H2_PAL_OK;
}

int h2_gizclaw_e2e_fixture_cleanup(h2_gizclaw_e2e_fixture_t *fixture) {
  assert(fixture != NULL);
  ++s_cleaned;
  return H2_PAL_OK;
}

size_t h2_gizclaw_e2e_fixture_emit_recovery_ledger(
    const h2_gizclaw_e2e_fixture_t *fixture) {
  assert(fixture != NULL);
  return 0;
}

int h2_gizclaw_e2e_fixture_deinit(h2_gizclaw_e2e_fixture_t *fixture) {
  if (s_fail_deinit) {
    s_retained = fixture;
    return H2_PAL_ERR_TIMEOUT;
  }
  return H2_PAL_OK;
}

#define CASE(name)                                                             \
  int h2_gizclaw_e2e_##name(h2_gizclaw_e2e_fixture_t *fixture) {               \
    assert(fixture != NULL);                                                   \
    ++s_ran;                                                                   \
    return H2_PAL_OK;                                                          \
  }
CASE(run_connectivity)
CASE(run_rpc)
CASE(run_firmware)
CASE(prepare_voice)
CASE(run_voice)
CASE(run_concurrency)
CASE(run_service)
#undef CASE

int main(int argc, char **argv) {
  const bool connectivity_only = argc == 2 && !strcmp(argv[1], "connectivity");
  const size_t selected = connectivity_only ? 1u : 2u;
  assert(h2_gizclaw_e2e_case_count == (connectivity_only ? 1u : 6u));
  static const h2_pal_mem_vtable_t mem_vtable = {.alloc = allocate,
                                                 .free = release};
  static const h2_pal_mem_api_t mem = {.vtable = &mem_vtable};
  static const h2_pal_time_vtable_t time_vtable = {
      .get_monotonic_ms = monotonic, .sleep_ms = sleep_ms};
  static const h2_pal_time_api_t time = {.vtable = &time_vtable};
  static const h2_pal_sync_vtable_t sync_vtable = {.create_mutex = mutex_create,
                                                   .destroy_mutex = mutex_op,
                                                   .lock_mutex = mutex_op,
                                                   .unlock_mutex = mutex_op};
  static const h2_pal_sync_api_t sync = {.vtable = &sync_vtable};
  static const h2_pal_task_vtable_t task_vtable = {.start = task_start,
                                                   .join = task_join};
  static const h2_pal_task_api_t task = {.vtable = &task_vtable};
  static const h2_pal_queue_api_t queue = {0};
  static const h2_pal_crypto_api_t crypto = {0};
  static const h2_pal_http_api_t http = {0};
  static const h2_pal_log_api_t log = {0};
  static const h2_pal_webrtc_api_t webrtc = {0};
  h2_runtime_t runtime = {.mem = &mem,
                          .time = &time,
                          .sync = &sync,
                          .task = &task,
                          .queue = &queue,
                          .crypto = &crypto,
                          .http = &http,
                          .log = &log,
                          .webrtc = &webrtc};
  h2_gizclaw_e2e_config_t config = {
      .server_endpoint = {"example.invalid:9821", 20},
      .registration_token = {"mock-token", 10},
      .suites =
          H2_GIZCLAW_E2E_SUITE_CONNECTIVITY | H2_GIZCLAW_E2E_SUITE_SERVICE,
      .case_timeout_ms = 1000,
      .cleanup_timeout_ms = 1000};
  h2_gizclaw_e2e_result_t result;
  if (connectivity_only) {
    const uint32_t unsupported[] = {
        H2_GIZCLAW_E2E_SUITE_ALL, H2_GIZCLAW_E2E_SUITE_SERVICE,
        H2_GIZCLAW_E2E_SUITE_CONNECTIVITY | H2_GIZCLAW_E2E_SUITE_FIRMWARE};
    for (size_t i = 0u; i < sizeof(unsupported) / sizeof(*unsupported); ++i) {
      config.suites = unsupported[i];
      assert(h2_gizclaw_e2e_run(&runtime, &config, &result) ==
             H2_GIZCLAW_E2E_EXIT_HARNESS_ERROR);
      assert(!s_live && !s_connected && !s_ran);
    }
    config.suites = H2_GIZCLAW_E2E_SUITE_CONNECTIVITY;
  }
  runtime.queue = NULL;
  assert(h2_gizclaw_e2e_run(&runtime, &config, &result) ==
         H2_GIZCLAW_E2E_EXIT_HARNESS_ERROR);
  assert(s_live == 0 && s_connected == 0);
  runtime.queue = &queue;
  assert(h2_gizclaw_e2e_run(&runtime, &config, &result) ==
         H2_GIZCLAW_E2E_EXIT_PASS);
  assert(result.complete && result.passed == selected && result.terminal == selected &&
         result.retained_resources == 0);
  assert(s_live == 0 && s_connected == selected && s_ran == selected && s_cleaned == selected);

  s_fail_deinit = true;
  assert(h2_gizclaw_e2e_run(&runtime, &config, &result) ==
         H2_GIZCLAW_E2E_EXIT_HARNESS_ERROR);
  assert(result.retained_resources > 0 &&
         result.cleanup_rc == H2_PAL_ERR_TIMEOUT);
  assert(s_live == 2 && s_retained != NULL);
  assert(s_retained->runtime == &runtime && s_retained->config == &config);
  assert(s_connected == selected + 1u && s_ran == selected + 1u && s_cleaned == selected + 1u);
  /* A failed teardown latches the process guard and prevents another run. */
  assert(h2_gizclaw_e2e_run(&runtime, &config, &result) ==
         H2_GIZCLAW_E2E_EXIT_HARNESS_ERROR);
  assert(s_connected == selected + 1u && s_live == 2);
  /* No real tasks exist in this boundary test; release retained test memory
   * at process teardown, after all lifetime assertions. */
  for (size_t i = 0; i < 16; ++i)
    free(s_allocations[i]);
  return 0;
}
