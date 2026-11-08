#include "h106_mfg_entry.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <string.h>

static const h2_mfg_step_t step = {"storage", H2_MFG_STORAGE, 1};
const h2_mfg_profile_t h2_mfg_board_profile = {
    .board = "test", .revision = 1, .steps = &step, .total = 1};

static h2_runtime_t runtime;
static int session_storage, mutex_storage;
static h2_mfg_t* const session = (h2_mfg_t*)&session_storage;
static h2_pal_mutex_t* const operation_mutex = (h2_pal_mutex_t*)&mutex_storage;
static int fail_at, expected_ble, input_calls, loader_calls;
static int input_running, stop_calls, stop_failures, before_failure;
static int lock_depth, scenario, mfg_calls;
static unsigned session_calls, runtime_calls, sleeps;
static unsigned session_retries, runtime_retries;
static int completed_session, terminal_runtime_error;
static const h2_device_loader_config_t* active_config;

static int sleep_ms(void* user, uint32_t ms) {
  (void)user;
  assert(ms == 50u);
  ++sleeps;
  return H2_PAL_OK;
}
static const h2_pal_time_vtable_t time_ops = {.sleep_ms = sleep_ms};
static const h2_pal_time_api_t time_api = {.vtable = &time_ops};

static int lock_mutex(void* user, h2_pal_mutex_t* mutex) {
  (void)user;
  assert(mutex == operation_mutex && lock_depth == 0);
  lock_depth = 1;
  return H2_PAL_OK;
}
static int unlock_mutex(void* user, h2_pal_mutex_t* mutex) {
  (void)user;
  assert(mutex == operation_mutex && lock_depth == 1);
  lock_depth = 0;
  return H2_PAL_OK;
}
static const h2_pal_sync_vtable_t sync_ops = {.lock_mutex = lock_mutex,
                                              .unlock_mutex = unlock_mutex};
static const h2_pal_sync_api_t sync_api = {.vtable = &sync_ops};

static int mapping(void* user,
                   const h2_runtime_component_mapping_entry_t* entry) {
  unsigned* calls = user;
  assert(entry->component_id == H2_MFG_NFC_COMPONENT_ID);
  assert(entry->periph_id == 201u);
  ++*calls;
  return H2_PAL_OK;
}

int h2_runtime_init(const h2_runtime_config_t* config, h2_runtime_t** out) {
  unsigned calls = 0;
  assert(config->component_mapper->vtable->list(
             NULL, H2_RUNTIME_COMPONENT_NFC_READER, mapping, &calls) == 0);
  assert(calls == 1u);
  assert(config->component_mapper->vtable->list(
             NULL, H2_RUNTIME_COMPONENT_BUTTON, mapping, &calls) == 0);
  assert(calls == 1u);
  h2_pal_periph_id_t periph = 999;
  assert(config->component_mapper->vtable->get_periph_id(
             NULL, H2_MFG_NFC_COMPONENT_ID, &periph) == 0 &&
         periph == 201u);
  assert(config->component_mapper->vtable->get_periph_id(NULL, 99u, &periph) ==
         H2_PAL_ERR_NOT_FOUND);
  assert(periph == 0u);
  *out = fail_at == 1 ? NULL : &runtime;
  return fail_at == 1 ? H2_PAL_ERR_NO_MEMORY : H2_PAL_OK;
}

int h2_mfg_create(h2_runtime_t* rt, const h2_mfg_profile_t* profile,
                  h2_mfg_t** out) {
  assert(rt == &runtime && profile == &h2_mfg_board_profile);
  *out = fail_at == 2 ? NULL : session;
  return fail_at == 2 ? H2_PAL_ERR_NO_MEMORY : H2_PAL_OK;
}

int h2_runtime_input_start(h2_runtime_t* rt,
                           const h2_runtime_input_poll_config_t* config) {
  assert(rt == &runtime && config == NULL);
  assert(lock_depth == 1 && !input_running);
  ++input_calls;
  if (fail_at == 3) return H2_PAL_ERR_TASK;
  input_running = 1;
  return H2_PAL_OK;
}

int h2_runtime_input_stop(h2_runtime_t* rt) {
  assert(rt == &runtime && lock_depth == 1 && input_running);
  ++stop_calls;
  if (stop_failures) {
    --stop_failures;
    return H2_PAL_ERR_TASK;
  }
  input_running = 0;
  return H2_PAL_OK;
}

int h2_device_loader_run(h2_device_loader_t* device, h2_runtime_t* rt,
                         const h2_device_loader_config_t* config,
                         const h2_iostreamikcp_io_t* io, int with_ble) {
  assert(device && rt == &runtime && !io && with_ble == expected_ble);
  /* Startup can write P2 or relay P2 back into P1. Acquisition must not
   * start merely because the entry constructed its Runtime and MFG session. */
  assert(input_calls == 0 && !session_calls && !runtime_calls);
  assert(strcmp(config->board, "test") == 0 && config->mfg_required_total == 1);
  assert(config->identity_policy == H2_DEVICE_IDENTITY_PADDED_WINDOW);
  assert(config->serial_post_write_poll_ms == 1u);
  ++loader_calls;
  active_config = config;
  assert(lock_mutex(NULL, operation_mutex) == 0);
  assert(config->before_disruptive(config->user,
                                   H2_LOADER_DISRUPTIVE_REBOOT_UPGRADE) == 0);
  assert(unlock_mutex(NULL, operation_mutex) == 0);
  config->rearm_mfg(config->user);
  assert(input_calls == 0 && stop_calls == 0);

  char output[8];
  const char* arguments[] = {"status"};
  assert(config->mfg_command(config->user, (h2_loader_command_transport_t)0, 1u,
                             arguments, output, sizeof(output)) == 0);
  assert(strcmp(output, "ok") == 0);

  h2_loader_t loader = {0};
  int rc = config->run_mfg(config->user, &loader, &sync_api, operation_mutex,
                           &sync_api, operation_mutex);
  if (fail_at == 3) {
    assert(rc == H2_PAL_ERR_TASK && mfg_calls == 0 && stop_calls == 0);
  } else {
    int expected = scenario == 3 ? H2_PAL_ERR_IO : H2_PAL_OK;
    assert(rc == expected && mfg_calls == 1 && !input_running);
    if (scenario == 0) {
      /* Rearming itself must not restart acquisition during another startup
       * pass. Only a subsequent actual MFG run acquires new input tasks. */
      config->rearm_mfg(config->user);
      assert(input_calls == 1 && stop_calls == 1);
      assert(config->run_mfg(config->user, &loader, &sync_api, operation_mutex,
                             &sync_api, operation_mutex) == H2_PAL_OK);
      assert(input_calls == 2 && stop_calls == 2 && mfg_calls == 2);
    }
  }
  assert(!input_running && lock_depth == 0);
  return H2_PAL_ERR_IO;
}

int h2_mfg_destroy(h2_mfg_t** value) {
  assert(*value == session && runtime_calls == 0);
  ++session_calls;
  if (session_retries) {
    --session_retries;
    return H2_PAL_ERR_BUSY;
  }
  *value = NULL;
  completed_session = 1;
  return H2_PAL_OK;
}

int h2_runtime_destroy(h2_runtime_t** value) {
  assert(*value == &runtime && (fail_at == 2 || completed_session));
  assert(!input_running);
  ++runtime_calls;
  if (runtime_retries) {
    --runtime_retries;
    return H2_PAL_ERR_BUSY;
  }
  *value = NULL;
  return terminal_runtime_error ? H2_PAL_ERR_IO : H2_PAL_OK;
}

void h2_mfg_rearm(void* user) { assert(user == session); }
int h2_mfg_before_disruptive(void* user, h2_loader_disruptive_action_t action) {
  assert(user == session && lock_depth == 1);
  assert(action == H2_LOADER_DISRUPTIVE_REBOOT_UPGRADE);
  if (before_failure) {
    before_failure = 0;
    return H2_PAL_ERR_IO;
  }
  return H2_PAL_OK;
}
int h2_mfg_run(void* user, h2_loader_t* loader, const h2_pal_sync_api_t* sync,
               h2_pal_mutex_t* mutex, const h2_pal_sync_api_t* wifi_sync,
               h2_pal_mutex_t* wifi_mutex) {
  assert(user == session && loader && sync == &sync_api);
  assert(mutex == operation_mutex && wifi_sync == sync && wifi_mutex == mutex);
  assert(input_running && lock_depth == 0);
  ++mfg_calls;
  if (scenario == 1 || scenario == 2) {
    assert(lock_mutex(NULL, operation_mutex) == 0);
    if (scenario == 1)
      stop_failures = 1;
    else
      before_failure = 1;
    int rc = active_config->before_disruptive(
        active_config->user, H2_LOADER_DISRUPTIVE_REBOOT_UPGRADE);
    assert(rc == (scenario == 1 ? H2_PAL_ERR_TASK : H2_PAL_ERR_IO));
    assert(input_running);
    assert(stop_calls == (scenario == 1 ? 1 : 0));
    /* Cleanup/join failure rejects the disruptive action and preserves the
     * acquisition borrow. A retry must actually join it before succeeding. */
    assert(active_config->before_disruptive(
               active_config->user, H2_LOADER_DISRUPTIVE_REBOOT_UPGRADE) == 0);
    assert(!input_running && stop_calls == (scenario == 1 ? 2 : 1));
    assert(unlock_mutex(NULL, operation_mutex) == 0);
  }
  return scenario == 3 ? H2_PAL_ERR_IO : H2_PAL_OK;
}
int h2_mfg_command(void* user, h2_loader_command_transport_t transport,
                   size_t argc, const char* const* argv, char* output,
                   size_t capacity) {
  assert(user == session && transport == (h2_loader_command_transport_t)0);
  assert(argc == 1u && strcmp(argv[0], "status") == 0 && capacity == 8u);
  memcpy(output, "ok", 3u);
  return H2_PAL_OK;
}

static void run(int failure, int ble, unsigned retry_session,
                unsigned retry_runtime, int terminal_error, int mode) {
  fail_at = failure;
  expected_ble = ble;
  session_retries = retry_session;
  runtime_retries = retry_runtime;
  terminal_runtime_error = terminal_error;
  scenario = mode;
  input_calls = loader_calls = completed_session = 0;
  input_running = stop_calls = stop_failures = before_failure = lock_depth = 0;
  mfg_calls = 0;
  session_calls = runtime_calls = sleeps = 0;
  h2_runtime_config_t config = {.time = &time_api};
  h2_device_loader_config_t loader = {
      .identity_policy = H2_DEVICE_IDENTITY_PADDED_WINDOW,
      .serial_post_write_poll_ms = 1u};
  h106_mfg_run(&config, &loader, ble);
  assert(loader_calls == (failure == 0 || failure == 3));
  assert(input_calls ==
         (failure == 0 ? (mode == 0 ? 2 : 1) : (failure == 3 ? 1 : 0)));
  assert(session_calls ==
         (failure == 1 || failure == 2 ? 0u : retry_session + 1u));
  assert(runtime_calls == (failure == 1 ? 0u : retry_runtime + 1u));
  assert(sleeps == retry_session + retry_runtime);
}

int main(void) {
  run(1, 0, 0, 0, 0, 0);
  run(2, 0, 0, 0, 0, 0);
  run(3, 0, 0, 0, 0, 0);
  run(0, 0, 0, 0, 0, 0);
  run(0, 1, 2, 1, 0, 0);
  run(0, 1, 0, 0, 1, 0);
  run(0, 0, 0, 0, 0, 1);
  run(0, 0, 0, 0, 0, 2);
  run(0, 0, 0, 0, 0, 3);
  return 0;
}
