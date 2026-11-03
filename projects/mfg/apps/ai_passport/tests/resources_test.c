#include "resources.h"
#include "resources_sdk.h"
#include "h2_desktop_platform.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <string.h>

static h2_runtime_t runtime;
static h2_loader_t loader;
static h2_loader_command_config_t command;
static h2_pal_mutex_t *operation, *wifi;
static uint64_t now;
static int audio_live, wifi_live, mode;
static unsigned opens, closes, host_stops, wifi_stops;
static int open_error, close_busy, stop_busy;
static int handle;

static int time_ms(void* user, uint64_t* out) {
  (void)user;
  *out = now;
  return 0;
}
static const h2_pal_time_vtable_t time_vtable = {.get_monotonic_ms = time_ms};
static const h2_pal_time_api_t clock_api = {.vtable = &time_vtable};

int h2_passport_recording_prepare(void) { return 0; }

int h2_esp_board_audio_set_aec_enabled(int enabled) {
  assert(enabled == 0 || enabled == 1);
  mode = enabled;
  return 0;
}
int h2_esp_board_audio_release(void) {
  audio_live = 0;
  return 0;
}
int h2_esp_platform_wifi_stop(void) {
  /* The phase owns the same Wi-Fi mutex used by Loader commands. */
  assert(h2_pal_mutex_try_lock(runtime.sync, wifi) != 0);
  ++wifi_stops;
  wifi_live = 0;
  return 0;
}
static void unlocked_command_owner(void) {
  assert(h2_pal_mutex_try_lock(runtime.sync, operation) == 0);
  assert(h2_pal_mutex_unlock(runtime.sync, operation) == 0);
}
static int service_open(void* user, h2_runtime_t* rt,
                        const h2_loader_command_config_t* cfg,
                        const char* board, uint32_t capabilities, void** out) {
  (void)user;
  unlocked_command_owner();
  assert(rt == &runtime && cfg == &command && strcmp(board, "ai_passport") == 0);
  assert(capabilities == 7 && !audio_live && !wifi_live);
  ++opens;
  *out = &handle;
  return open_error;
}
static int service_close(void* user, void* service) {
  (void)user;
  unlocked_command_owner();
  assert(service == &handle);
  ++closes;
  if (close_busy) { --close_busy; return H2_PAL_ERR_BUSY; }
  return 0;
}
static int host_stop(void* user) {
  (void)user;
  unlocked_command_owner();
  ++host_stops;
  if (stop_busy) { --stop_busy; return H2_PAL_ERR_BUSY; }
  return 0;
}
static const h2_pal_ble_vtable_t ble_vtable = {.stop = host_stop};
static const h2_pal_ble_api_t ble = {.vtable = &ble_vtable};
int h2_esp_h2loader_ble_command_service_with_config(
    const h2loader_ble_command_service_config_t* config,
    h2loader_app_command_service_api_t* out) {
  assert(config->inline_commands && config->server_task_stack_size == 32768);
  assert(config->stream_buffer_size == 2048 && config->stream_window == 8);
  assert(config->input_frame_capacity == 8);
  *out = (h2loader_app_command_service_api_t){.open = service_open,
                                             .close = service_close};
  return 0;
}
static void setup(void) {
  runtime = (h2_runtime_t){.board = "ai_passport", .ble_host = &ble,
      .sync = h2_desktop_platform_sync_api(), .time = &clock_api};
  loader.config.hardware_capabilities = 7;
  command.loader = &loader;
  const h2_pal_mutex_config_t mc = {
      .allocator = h2_desktop_platform_default_allocator()};
  assert(h2_pal_mutex_create(runtime.sync, &mc, &operation) == 0);
  assert(h2_pal_mutex_create(runtime.sync, &mc, &wifi) == 0);
  assert(h2_passport_resources_init(&runtime) == 0);
  h2_passport_resources_bind_wifi(runtime.sync, wifi);
  now = opens = closes = host_stops = wifi_stops = 0;
  audio_live = wifi_live = mode = open_error = close_busy = stop_busy = 0;
}
static int prepare(h2_mfg_check_t check) {
  assert(h2_pal_mutex_lock(runtime.sync, operation) == 0);
  int rc = h2_passport_prepare_check(NULL, check);
  assert(h2_pal_mutex_unlock(runtime.sync, operation) == 0);
  return rc;
}
static void poll(void) {
  assert(h2_passport_resources_poll(NULL, &runtime, &command) == 0);
}
static void teardown(void) {
  assert(h2_passport_resources_stop(NULL, &runtime) == 0);
  h2_passport_resources_bind_wifi(NULL, NULL);
  assert(h2_pal_mutex_destroy(runtime.sync, wifi) == 0);
  assert(h2_pal_mutex_destroy(runtime.sync, operation) == 0);
}
static void phases_and_retryable_shutdown(void) {
  setup();
  assert(prepare(H2_MFG_AEC) == 0 && mode == 1);
  audio_live = 1;
  assert(prepare(H2_MFG_WIFI) == 0 && !audio_live);
  wifi_live = 1;
  assert(prepare(H2_MFG_BLE) == H2_PAL_ERR_WOULD_BLOCK);
  assert(!wifi_live && opens == 0);
  poll();
  assert(opens == 1 && prepare(H2_MFG_BLE) == 0);
  assert(prepare(H2_MFG_POWER) == H2_PAL_ERR_WOULD_BLOCK);
  close_busy = 2;
  stop_busy = 1;
  for (unsigned i = 0; i < 3; ++i) {
    poll();
    assert(prepare(H2_MFG_POWER) == H2_PAL_ERR_WOULD_BLOCK);
  }
  poll();
  assert(prepare(H2_MFG_POWER) == 0 && closes == 3 && host_stops == 2);
  teardown();
}
static void failed_partial_open_is_closed_before_retry(void) {
  setup();
  open_error = H2_PAL_ERR_NO_MEMORY;
  close_busy = 1;
  assert(prepare(H2_MFG_BLE) == H2_PAL_ERR_WOULD_BLOCK);
  poll();
  assert(opens == 1 && closes == 1);
  assert(prepare(H2_MFG_BLE) == H2_PAL_ERR_NO_MEMORY);
  poll();
  assert(closes == 2 && host_stops == 1);
  open_error = 0;
  assert(prepare(H2_MFG_BLE) == H2_PAL_ERR_WOULD_BLOCK);
  poll();
  assert(opens == 2 && prepare(H2_MFG_BLE) == 0);
  teardown();
}
static void timeout_cancels_unstarted_service(void) {
  setup();
  assert(prepare(H2_MFG_BLE) == H2_PAL_ERR_WOULD_BLOCK);
  now = 15000;
  assert(prepare(H2_MFG_BLE) == H2_PAL_ERR_TIMEOUT);
  poll();
  assert(opens == 0);
  teardown();
}
int main(void) {
  phases_and_retryable_shutdown();
  failed_partial_open_is_closed_before_retry();
  timeout_cancels_unstarted_service();
  puts("Passport resources: phase exclusion, unlocked joins, partial open, retry and timeout passed");
  return 0;
}
