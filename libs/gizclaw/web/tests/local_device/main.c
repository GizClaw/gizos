#include "h2/pal/h2_pal_unsupported.h"
#include "h2_gizclaw_service.h"
#include "h2_lua.h"
#include "h2_lua_capability.h"
#include "h2_lua_job.h"
#include "h2_runtime_component.h"
#include "h2_web_fs.h"
#include "h2_web_gizclaw.h"
#include "h2_web_gizclaw_apps.h"
#include "h2_web_main_thread.h"
#include "h2_web_platform.h"
#include "pb_encode.h"
#include <assert.h>
#include <emscripten.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct {
  h2_web_platform_t *platform;
  h2_web_fs_t *fs;
  h2_runtime_t *runtime;
  h2_gizclaw_lua_app_catalog_t *apps;
  h2_gizclaw_device_t *device;
  h2_gizclaw_service_t *service;
  h2_web_gizclaw_t *bridge;
  h2_web_gizclaw_apps_t *installer;
  const h2_pal_task_api_t *base_task;
  h2_pal_task_t *bridge_task, *lua_task, *owner;
  h2_lua_host_t *host;
  h2_lua_job_id_t job;
  h2_gizclaw_config_t product;
  h2_gizclaw_lua_app_run_t run;
  char params[H2_GIZCLAW_LUA_APP_PARAMS_BYTES_MAX];
  _Atomic int shutdown, phase, pending, retry_lua, retry_close;
  _Atomic int gate, entered, completions, led_actions, brightness, enabled,
      reads;
  _Atomic int finish_lua, stop_lua, fail_lua_join, fail_bridge_join;
  _Atomic int visits, runs, retained, natural, canceled, worker_error,
      completion_result;
  _Atomic h2_lua_capability_request_id_t capability;
} state;
enum { MAIN = 1, LUA_RUNNING, LUA_RETAINED, CLOSE_RETAINED };

EMSCRIPTEN_KEEPALIVE void h2_web_fixture_action(int action) {
  switch (action) {
  case 1:
    atomic_store(&state.gate, 1);
    break;
  case 2:
    atomic_store(&state.gate, 0);
    break;
  case 3:
    atomic_store(&state.finish_lua, 1);
    break;
  case 4:
    atomic_store(&state.stop_lua, 1);
    break;
  case 5:
    atomic_store(&state.retry_lua, 1);
    break;
  case 6:
    atomic_store(&state.retry_close, 1);
    break;
  case 7:
    atomic_store(&state.fail_bridge_join, 1);
    break;
  case 8:
    atomic_store(&state.fail_lua_join, 1);
    break;
  default:
    assert(0);
  }
}
EMSCRIPTEN_KEEPALIVE int h2_web_fixture_state(int field) {
  switch (field) {
  case 1:
    return atomic_load(&state.phase);
  case 2:
    return atomic_load(&state.entered);
  case 3:
    return atomic_load(&state.completions);
  case 4:
    return atomic_load(&state.led_actions);
  case 5:
    return atomic_load(&state.brightness);
  case 6:
    return atomic_load(&state.enabled);
  case 7:
    return atomic_load(&state.visits);
  case 8:
    return atomic_load(&state.runs);
  case 9:
    return atomic_load(&state.retained);
  case 10:
    return atomic_load(&state.reads);
  case 11:
    return atomic_load(&state.worker_error);
  case 12:
    return atomic_load(&state.natural);
  case 13:
    return atomic_load(&state.canceled);
  case 14:
    return atomic_load(&state.completion_result);
  default:
    assert(0);
    return -1;
  }
}
static int start_task(void *u, const h2_pal_task_options_t *o,
                      h2_pal_task_entry_t e, void *c, h2_pal_task_t **out) {
  (void)u;
  int rc = h2_pal_task_start(state.base_task, o, e, c, out);
  if (rc == H2_PAL_OK && !strcmp(o->name, "$gizclaw/web/device"))
    state.bridge_task = *out;
  if (rc == H2_PAL_OK && !strcmp(o->name, "$lua/worker"))
    state.lua_task = *out;
  return rc;
}
static int join_task(void *u, h2_pal_task_t *task) {
  (void)u;
  if (task == state.bridge_task && atomic_exchange(&state.fail_bridge_join, 0))
    return H2_PAL_ERR_IO;
  if (task == state.lua_task && atomic_exchange(&state.fail_lua_join, 0))
    return H2_PAL_ERR_IO;
  return h2_pal_task_join(state.base_task, task);
}
static const h2_pal_task_vtable_t task_vtable = {start_task, join_task};
static const h2_pal_task_api_t tasks = {.vtable = &task_vtable};
static int led_info(void *u, h2_pal_led_id_t id, h2_pal_led_info_t *out) {
  (void)u;
  assert(id == 501);
  *out = (h2_pal_led_info_t){.id = id,
                             .pixel_count = 1,
                             .channels_per_pixel = 3,
                             .supports_brightness = 1,
                             .frame_commit_is_atomic = 1};
  return H2_PAL_OK;
}
static int led_solid(void *u, h2_pal_led_id_t id, h2_pal_led_color_t color) {
  (void)u;
  assert(id == 501);
  atomic_store(&state.enabled, color.r || color.g || color.b || color.w);
  ++state.led_actions;
  return H2_PAL_OK;
}
static int led_clear(void *u, h2_pal_led_id_t id) {
  return led_solid(u, id, (h2_pal_led_color_t){0});
}
static int led_brightness(void *u, h2_pal_led_id_t id, uint8_t percent) {
  (void)u;
  assert(id == 501);
  atomic_store(&state.brightness, percent);
  ++state.led_actions;
  return H2_PAL_OK;
}
static const h2_pal_led_vtable_t led_vtable = {.get_info = led_info,
                                               .set_solid = led_solid,
                                               .clear = led_clear,
                                               .set_brightness_percent =
                                                   led_brightness};
static const h2_pal_led_api_t led = {.vtable = &led_vtable};
static const h2_pal_periph_led_strip_payload_t led_payload = {
    .led_count = 1, .channels_per_led = 3};
static int periph_get(void *u, h2_pal_periph_id_t id,
                      h2_pal_periph_info_t *out) {
  (void)u;
  if (id != 501)
    return H2_PAL_ERR_NOT_FOUND;
  *out = (h2_pal_periph_info_t){.id = id,
                                .type = H2_PAL_PERIPH_TYPE_LED_STRIP,
                                .payload = &led_payload,
                                .payload_size = sizeof(led_payload)};
  strcpy(out->name, "fixture-led");
  return H2_PAL_OK;
}
static int periph_list(void *u, h2_pal_periph_type_t filter,
                       h2_pal_periph_cb_t cb, void *user) {
  h2_pal_periph_info_t info;
  int rc = periph_get(u, 501, &info);
  return rc == H2_PAL_OK &&
                 (filter == H2_PAL_PERIPH_TYPE_ANY || filter == info.type)
             ? cb(user, &info)
             : rc;
}
static const h2_pal_periph_vtable_t periph_vtable = {periph_list, periph_get};
static const h2_pal_periph_api_t periph = {.vtable = &periph_vtable};
static int mapper_get(void *u, h2_runtime_component_id_t id,
                      h2_pal_periph_id_t *out) {
  (void)u;
  if (id != 42)
    return H2_PAL_ERR_NOT_FOUND;
  *out = 501;
  return H2_PAL_OK;
}
static int mapper_list(void *u, h2_runtime_component_t filter,
                       h2_runtime_component_mapping_cb_t cb, void *user) {
  (void)u;
  const h2_runtime_component_mapping_entry_t entry = {42, 501};
  return filter == H2_RUNTIME_COMPONENT_NONE ||
                 filter == H2_RUNTIME_COMPONENT_LED_STRIP
             ? cb(user, &entry)
             : H2_PAL_OK;
}
static const h2_runtime_component_mapper_vtable_t mapper_vtable = {mapper_list,
                                                                   mapper_get};
static const h2_runtime_component_mapper_t mapper = {.vtable = &mapper_vtable};
static int read_led(void *u, h2_gizclaw_mhs_read_t *out) {
  assert(u == &state);
  ++state.reads;
  out->led = (gizclaw_rpc_v1_LedHwdReadResponse){
      .has_enabled = true,
      .enabled = atomic_load(&state.enabled),
      .has_brightness_percent = true,
      .brightness_percent = (uint32_t)atomic_load(&state.brightness)};
  return H2_PAL_OK;
}
static int write_led(void *u, const h2_gizclaw_mhs_write_t *in,
                     h2_gizclaw_mhs_read_t *out) {
  assert(u == &state);
  h2_pal_periph_id_t id = 0;
  int rc = h2_runtime_periph_id(state.runtime, 42, &id);
  if (rc != H2_PAL_OK)
    return rc;
  if (in->led.has_brightness_percent)
    rc = h2_pal_led_set_brightness_percent(state.runtime->led, id,
                                           (uint8_t)in->led.brightness_percent);
  if (rc == H2_PAL_OK && in->led.has_enabled)
    rc = in->led.enabled
             ? h2_pal_led_set_solid(state.runtime->led, id,
                                    (h2_pal_led_color_t){1, 2, 3, 0})
             : h2_pal_led_clear(state.runtime->led, id);
  return rc == H2_PAL_OK ? read_led(u, out) : rc;
}
static int read_wifi(void *u, h2_gizclaw_mhs_read_t *out) {
  assert(u == &state);
  out->wifi = (gizclaw_rpc_v1_WifiHwdReadResponse){.has_connected = true,
                                                   .connected = false};
  return H2_PAL_OK;
}
static void find_complete(void *u, int result) {
  assert(u == &state);
  atomic_store(&state.completion_result, result);
  ++state.completions;
}
static int find_tool(void *u, h2_gizclaw_tool_t tool,
                     h2_gizclaw_rpc_bytes_t bytes,
                     h2_gizclaw_rpc_provider_response_t *out) {
  (void)bytes;
  assert(u == &state && tool == H2_GIZCLAW_TOOL_DEVICE_FIND);
  ++state.entered;
  while (atomic_load(&state.gate) && !atomic_load(&state.shutdown))
    assert(h2_pal_time_sleep_ms(state.runtime->time, 1) == H2_PAL_OK);
  out->on_complete = find_complete;
  out->complete_user = &state;
  return H2_PAL_OK;
}
static int error_tool(void *u, h2_gizclaw_tool_t tool,
                      h2_gizclaw_rpc_bytes_t bytes,
                      h2_gizclaw_rpc_provider_response_t *out) {
  (void)bytes;
  (void)out;
  assert(u == &state && tool == H2_GIZCLAW_TOOL_SOCIAL_PING);
  return -1234;
}
static int get_facts(void *u, h2_gizclaw_device_facts_t *out) {
  assert(u == &state);
  out->has_battery_percent = true;
  out->battery_percent = 0;
  out->has_charging = true;
  out->charging = false;
  return H2_PAL_OK;
}
static void run_complete(void *u, int result) {
  assert(u == &state);
  if (result != H2_PAL_OK)
    h2_gizclaw_lua_app_lease_release(&state.run.lease);
  atomic_store(&state.pending, result == H2_PAL_OK ? 1 : 0);
}
static int prepare_run(void *u, const h2_gizclaw_lua_app_run_t *run,
                       h2_gizclaw_lua_app_handoff_t *out) {
  assert(u == &state);
  if (atomic_load(&state.phase) != MAIN)
    return H2_PAL_ERR_BUSY;
  printf("LOCAL_DEVICE prepare phase=%d pending=%d\n", atomic_load(&state.phase), atomic_load(&state.pending));
  int expected = 0;
  if (!atomic_compare_exchange_strong(&state.pending, &expected, -1))
    return H2_PAL_ERR_BUSY;
  state.run = *run;
  state.run.lease = NULL;
  size_t at = 0;
  for (size_t i = 0; i < run->param_count; ++i)
    for (unsigned j = 0; j < 2; ++j) {
      const char *value = j ? run->params[i].value : run->params[i].name;
      size_t count = strlen(value) + 1;
      if (count > sizeof(state.params) - at) {
        atomic_store(&state.pending, 0);
        return H2_PAL_ERR_NO_SPACE;
      }
      memcpy(state.params + at, value, count);
      if (j)
        state.run.params[i].value = state.params + at;
      else
        state.run.params[i].name = state.params + at;
      at += count;
    }
  state.run.lease = run->lease;
  *out = (h2_gizclaw_lua_app_handoff_t){run_complete, &state};
  return H2_PAL_OK;
}
static const h2_gizclaw_vtable_t product_vtable = {
    .get_facts = get_facts, .prepare_lua_app_run = prepare_run};
static const h2_gizclaw_tool_handler_t tool_handlers[] = {
    {H2_GIZCLAW_TOOL_DEVICE_FIND, find_tool, &state},
    {H2_GIZCLAW_TOOL_SOCIAL_PING, error_tool, &state}};
static const h2_gizclaw_mhs_device_t mhs_devices[] = {
    {.id = "led.fixture",
     .hwd = gizclaw_rpc_v1_ClientHwd_CLIENT_HWD_LED,
     .read = read_led,
     .write = write_led,
     .user = &state},
    {.id = "wifi.fixture",
     .hwd = gizclaw_rpc_v1_ClientHwd_CLIENT_HWD_WIFI,
     .read = read_wifi,
     .user = &state}};

static int bind_sdk(void) {
  const h2_web_gizclaw_config_t config = {.device = state.device,
                                          .allocator = state.runtime->mem,
                                          .task = state.runtime->task,
                                          .queue = state.runtime->queue};
  return h2_web_gizclaw_open(&config, &state.bridge);
}
static int attach_service(void) {
  const h2_gizclaw_service_config_t config = {.client_config = &state.product,
                                              .task = state.runtime->task,
                                              .queue = state.runtime->queue,
                                              .sync = state.runtime->sync,
                                              .runtime = state.runtime,
                                              .device = state.device,
                                              .operation_capacity = 4,
                                              .client_poll_timeout_ms = 10};
  // An offline Main need not start a network Service or hold any credentials.
  return h2_gizclaw_service_init(&config, &state.service);
}
static int detach_service(void) {
  if (!state.service)
    return H2_PAL_OK;
  int rc = h2_gizclaw_service_stop(state.service);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_service_deinit(state.service);
  if (rc == H2_PAL_OK)
    state.service = NULL;
  return rc;
}
static int capability_begin(void *u, h2_lua_capability_request_id_t id,
                            const char *input, const char *options,
                            char *output, size_t capacity, const char **error) {
  (void)options;
  (void)output;
  (void)capacity;
  (void)error;
  assert(u == &state && input);
  char *end = NULL;
  double visits = strtod(input, &end);
  assert(end && !*end && visits > 0 && visits < 10 && visits == (int)visits);
  atomic_store(&state.visits, (int)visits);
  atomic_store(&state.capability, id);
  return H2_PAL_ERR_WOULD_BLOCK;
}
static void capability_cancel(void *u, h2_lua_capability_request_id_t id) {
  assert(u == &state);
  if (atomic_load(&state.capability) == id)
    atomic_store(&state.capability, 0);
}
static int start_lua(void) {
  const h2_lua_host_config_t config = {
      .runtime = state.runtime,
      .worker_count = 1,
      .worker_stack_size = 65536,
      .max_jobs = 1,
      .event_delivery_capacity = 8,
      .callback_capacity_per_job = 8,
      .audio_track_capacity_per_job = 2,
      .max_coroutines_per_vm = 4,
      .ready_queue_capacity = 16,
      .waiter_capacity = 8,
      .pending_capability_capacity = 8,
      .vm_memory_limit_bytes = 2 * 1024 * 1024,
      .source_limit_bytes = 131072,
      .output_limit_bytes = 1024,
      .instruction_quantum = 1000,
      .resume_time_budget_ms = 5,
      .execution_timeout_ms = 30000,
      .storage = {.fs = state.runtime->fs, .root = "/data/gizclaw/user"}};
  int rc = h2_lua_host_create(&config, &state.host);
  if (rc == H2_PAL_OK)
    rc =
        h2_lua_register_capability(state.host, "fixture.finish",
                                   capability_begin, capability_cancel, &state);
  if (rc == H2_PAL_OK)
    rc = h2_lua_host_start(state.host);
  h2_lua_arg_t args[H2_GIZCLAW_LUA_APP_PARAMS_MAX];
  for (size_t i = 0; i < state.run.param_count; ++i)
    args[i] =
        (h2_lua_arg_t){state.run.params[i].name, state.run.params[i].value};
  if (rc == H2_PAL_OK)
    rc = h2_lua_job_submit_path(state.host, state.run.app.app_id, "@demo.lua",
                                state.run.app.entry, args,
                                state.run.param_count, &state.job);
  if (rc == H2_PAL_OK)
    ++state.runs;
  return rc;
}
static int destroy_lua(void) {
  if (state.host) {
    int rc = h2_lua_host_destroy_checked(state.host);
    if (rc != H2_PAL_OK)
      return rc;
    state.host = NULL;
  }
  state.job = H2_LUA_JOB_ID_NONE;
  h2_gizclaw_lua_app_lease_release(&state.run.lease);
  atomic_store(&state.pending, 0);
  return H2_PAL_OK;
}
static int restore_main(void) {
  int rc = h2_gizclaw_device_start(state.device);
  if (rc == H2_PAL_OK)
    rc = attach_service();
  if (rc == H2_PAL_OK)
    rc = bind_sdk();
  if (rc == H2_PAL_OK)
    atomic_store(&state.phase, MAIN);
  return rc;
}
static int begin_handoff(void) {
  int rc = h2_web_gizclaw_close(&state.bridge);
  if (rc == H2_PAL_ERR_IO && state.bridge) {
    assert(h2_gizclaw_device_stop(state.device) == H2_PAL_ERR_BUSY);
    ++state.retained;
    atomic_store(&state.phase, CLOSE_RETAINED);
    return H2_PAL_OK;
  }
  if (rc == H2_PAL_OK)
    rc = detach_service();
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_device_stop(state.device);
  if (rc == H2_PAL_OK)
    rc = start_lua();
  return rc;
}
static void lua_owner(void *u) {
  (void)u;
  while (!atomic_load(&state.shutdown)) {
    int rc = H2_PAL_OK;
    if (atomic_load(&state.pending) == 1 && !state.host &&
        atomic_load(&state.phase) == MAIN)
      rc = begin_handoff();
    if (atomic_load(&state.phase) == CLOSE_RETAINED &&
        atomic_exchange(&state.retry_close, 0))
      rc = begin_handoff();
    if (state.host && atomic_load(&state.phase) != LUA_RETAINED) {
      if (atomic_load(&state.capability))
        atomic_store(&state.phase, LUA_RUNNING);
      if (atomic_exchange(&state.finish_lua, 0)) {
        h2_lua_capability_request_id_t id =
            atomic_exchange(&state.capability, 0);
        rc =
            h2_lua_capability_complete(state.host, id, H2_PAL_OK, "done", NULL);
      }
      if (atomic_exchange(&state.stop_lua, 0))
        rc = h2_lua_job_cancel(state.host, state.job);
      h2_lua_job_status_t status;
      if (rc == H2_PAL_OK)
        rc = h2_lua_job_get_status(state.host, state.job, &status);
      if (rc == H2_PAL_OK && status.state >= H2_LUA_JOB_SUCCEEDED) {
        if (status.state != H2_LUA_JOB_SUCCEEDED &&
            status.state != H2_LUA_JOB_CANCELLED) {
          printf("LOCAL_DEVICE Lua state=%d message=%s\n", (int)status.state,
                 status.message);
          atomic_store(&state.worker_error, H2_PAL_ERR_INVALID_STATE);
          return;
        }
        if (status.state == H2_LUA_JOB_SUCCEEDED)
          ++state.natural;
        else
          ++state.canceled;
        rc = destroy_lua();
        if (rc == H2_PAL_ERR_IO && state.host) {
          ++state.retained;
          atomic_store(&state.phase, LUA_RETAINED);
          rc = H2_PAL_OK;
        } else if (rc == H2_PAL_OK)
          rc = restore_main();
      }
    }
    if (atomic_load(&state.phase) == LUA_RETAINED &&
        atomic_exchange(&state.retry_lua, 0)) {
      rc = destroy_lua();
      if (rc == H2_PAL_OK)
        rc = restore_main();
    }
    if (rc != H2_PAL_OK) {
      atomic_store(&state.worker_error, rc);
      return;
    }
    assert(h2_pal_time_sleep_ms(state.runtime->time, 2) == H2_PAL_OK);
  }
  if (state.host)
    atomic_store(&state.worker_error, destroy_lua());
}

typedef struct fixture_bytes {
  const uint8_t *data;
  size_t size;
} fixture_bytes_t;
static bool encode_fixture_bytes(pb_ostream_t *stream, const pb_field_t *field,
                                 void *const *user) {
  const fixture_bytes_t *value = *user;
  return pb_encode_tag_for_field(stream, field) &&
         pb_encode_string(stream, value->data, value->size);
}
void h2_web_fixture_assertions(void *, h2_web_main_result_t *,
                               h2_web_main_completion_t *);
static int create_runtime(void) {
  h2_runtime_config_t rcfg = {0};
  rcfg.board = "browser";
  rcfg.target = "webassembly";
  rcfg.chip = "wasm32";
  rcfg.firmware_info = h2_pal_unsupported_firmware_info_api();
  rcfg.mem = h2_web_platform_mem_api();
  rcfg.log = h2_web_platform_log_api();
  rcfg.time = h2_web_platform_time_api(state.platform);
  rcfg.timer = h2_web_platform_timer_api(state.platform);
  rcfg.task = &tasks;
  rcfg.queue = h2_web_platform_queue_api(state.platform);
  rcfg.sync = h2_web_platform_sync_api(state.platform);
  rcfg.fs = h2_web_fs_api(state.fs);
  rcfg.disk = h2_pal_unsupported_disk_api();
  rcfg.pref = h2_web_platform_pref_api(state.platform);
  rcfg.crypto = h2_web_platform_crypto_api(state.platform);
  rcfg.http = h2_web_platform_http_api(state.platform);
  rcfg.net = h2_pal_unsupported_net_api();
  rcfg.netif = h2_web_platform_netif_api(state.platform);
  rcfg.mqtt = h2_pal_unsupported_mqtt_api();
  rcfg.webrtc = h2_web_platform_webrtc_api(state.platform);
  rcfg.wifi_sta = h2_pal_unsupported_wifi_sta_api();
  rcfg.wifi_ap = h2_pal_unsupported_wifi_ap_api();
  rcfg.wifi_csi = h2_pal_unsupported_wifi_csi_api();
  rcfg.wifi_settings = h2_pal_unsupported_wifi_settings_api();
  rcfg.ble_host = h2_pal_unsupported_ble_host_api();
  rcfg.modem = h2_pal_unsupported_modem_api();
  if (h2_web_platform_fake_wifi_sta_api(state.platform) != NULL) {
    rcfg.wifi_sta = h2_web_platform_fake_wifi_sta_api(state.platform);
    rcfg.wifi_settings = h2_web_platform_fake_wifi_settings_api(state.platform);
    rcfg.modem = h2_web_platform_fake_modem_api(state.platform);
  }
  rcfg.power = h2_pal_unsupported_power_api();
  rcfg.display = h2_web_platform_display_api(state.platform);
  rcfg.audio = h2_web_platform_audio_api(state.platform);
  rcfg.audio_decoder = h2_web_platform_audio_decoder_api(state.platform);
  rcfg.periph = &periph;
  rcfg.button = h2_pal_unsupported_button_api();
  rcfg.touch = h2_web_platform_touch_api(state.platform);
  rcfg.buzzer = h2_pal_unsupported_buzzer_api();
  rcfg.nfc = h2_pal_unsupported_nfc_api();
  rcfg.nfc_card_emulation = h2_pal_unsupported_nfc_card_emulation_api();
  rcfg.imu = h2_pal_unsupported_imu_api();
  rcfg.gpio_irq = h2_pal_unsupported_gpio_irq_api();
  rcfg.led = &led;
  rcfg.switch_api = h2_pal_unsupported_switch_api();
  rcfg.pwm_switch = h2_pal_unsupported_pwm_switch_api();
  rcfg.input = h2_pal_unsupported_input_api();
  rcfg.system_event = h2_web_platform_system_event_api(state.platform);
  rcfg.video_decoder = h2_web_platform_video_decoder_api(state.platform);
  rcfg.component_mapper = &mapper;
  rcfg.event_queue_capacity = H2_RUNTIME_DEFAULT_EVENT_QUEUE_CAPACITY;
  return h2_runtime_init(&rcfg, &state.runtime);
}
int main(void) {
  state.platform =
      h2_web_platform_create(&(h2_web_platform_config_t){240, 240});
  assert(state.platform);
  state.base_task = h2_web_platform_task_api(state.platform);
  const char *const readonly[] = {"/data/lua", "/fixtures"};
  const h2_web_fs_config_t fs_config = {.persistent_root = "/data/gizclaw",
                                        .readonly_roots = readonly,
                                        .readonly_root_count = 2};
  int rc = h2_web_fs_open(state.platform, &fs_config, &state.fs);
  assert(rc == H2_PAL_OK);
  assert(h2_pal_fs_clear(h2_web_fs_api(state.fs), "/data/gizclaw") ==
         H2_PAL_OK);
  assert(create_runtime() == H2_PAL_OK);
  const h2_gizclaw_lua_app_catalog_config_t catalog = {
      .allocator = state.runtime->mem,
      .fs = state.runtime->fs,
      .sync = state.runtime->sync,
      .preinstalled_index = "/data/lua/preinstalled.json",
      .install_root = "/data/gizclaw/lua_apps"};
  assert(h2_gizclaw_lua_app_catalog_create(&catalog, &state.apps) == H2_PAL_OK);
  const h2_web_gizclaw_apps_config_t installer = {.apps = state.apps,
                                                  .allocator =
                                                      state.runtime->mem,
                                                  .queue = state.runtime->queue,
                                                  .task = state.runtime->task};
  assert(h2_web_gizclaw_apps_open(&installer, &state.installer) == H2_PAL_OK);
  state.product = (h2_gizclaw_config_t){.allocator = state.runtime->mem,
                                        .time = state.runtime->time,
                                        .http = state.runtime->http,
                                        .audio = state.runtime->audio,
                                        .manufacturer = "GizOS",
                                        .log = state.runtime->log,
                                        .model = "local-web-fixture",
                                        .vtable = &product_vtable,
                                        .user = &state,
                                        .lua_apps = state.apps,
                                        .tool_handlers = tool_handlers,
                                        .tool_handler_count = 2,
                                        .mhs_devices = mhs_devices,
                                        .mhs_device_count = 2};
  const h2_gizclaw_device_config_t device_config = {.client_config =
                                                        &state.product,
                                                    .task = state.runtime->task,
                                                    .sync = state.runtime->sync,
                                                    .runtime = state.runtime};
  assert(h2_gizclaw_device_create(&device_config, &state.device) == H2_PAL_OK);
  assert(h2_gizclaw_device_start(state.device) == H2_PAL_OK);
  assert(bind_sdk() == H2_PAL_OK);
  atomic_store(&state.phase, MAIN);
  const h2_pal_task_options_t owner = {.name = "web-local-device/lua",
                                       .min_stack_size = 65536};
  assert(h2_pal_task_start(state.runtime->task, &owner, lua_owner, NULL,
                           &state.owner) == H2_PAL_OK);
  h2_pal_fs_stat_t stat;
  assert(h2_pal_fs_stat(state.runtime->fs, "/fixtures/update.tar.zlib",
                        &stat) == H2_PAL_OK &&
         stat.size < 1048576);
  uint8_t *package = h2_pal_mem_alloc(state.runtime->mem, (size_t)stat.size);
  assert(package);
  h2_pal_fs_file_t *file = NULL;
  size_t count = 0;
  assert(h2_pal_fs_open(state.runtime->fs, "/fixtures/update.tar.zlib",
                        H2_PAL_FS_OPEN_READ, &file) == H2_PAL_OK);
  assert(h2_pal_fs_read(state.runtime->fs, file, package, (size_t)stat.size,
                        &count) == H2_PAL_OK &&
         count == stat.size);
  assert(h2_pal_fs_close(state.runtime->fs, file) == H2_PAL_OK);
  const uint32_t package_size = (uint32_t)count;
  uint8_t readonly_inner[16], readonly_request[128];
  gizclaw_rpc_v1_WifiHwdReadResponse wifi = {.has_connected = true,
                                             .connected = false};
  pb_ostream_t wire =
      pb_ostream_from_buffer(readonly_inner, sizeof(readonly_inner));
  assert(pb_encode(&wire, gizclaw_rpc_v1_WifiHwdReadResponse_fields, &wifi));
  fixture_bytes_t bytes = {readonly_inner, wire.bytes_written};
  gizclaw_rpc_v1_ClientMhsV0WriteRequest readonly_envelope = {
      .hwd = gizclaw_rpc_v1_ClientHwd_CLIENT_HWD_WIFI,
      .payload = {.funcs.encode = encode_fixture_bytes, .arg = &bytes}};
  strcpy(readonly_envelope.id, "wifi.fixture");
  wire = pb_ostream_from_buffer(readonly_request, sizeof(readonly_request));
  assert(pb_encode(&wire, gizclaw_rpc_v1_ClientMhsV0WriteRequest_fields,
                   &readonly_envelope));
  const uint32_t readonly_size = (uint32_t)wire.bytes_written;
  const uint8_t *readonly_data = readonly_request;
  const int readonly_method = H2_GIZCLAW_RPC_CLIENT_MHS_V0_WRITE;
  puts("LOCAL_DEVICE ready");
  rc =
      h2_web_main_call(h2_web_fixture_assertions,
                       (const void *[]){&package, &package_size, &readonly_data,
                                        &readonly_size, &readonly_method})
          .i32;
  h2_pal_mem_free(state.runtime->mem, package);
  atomic_store(&state.shutdown, 1);
  atomic_store(&state.gate, 0);
  assert(h2_pal_task_join(state.runtime->task, state.owner) == H2_PAL_OK);
  state.owner = NULL;
  if (rc == H2_PAL_OK)
    rc = atomic_load(&state.worker_error);
  int cleanup = h2_web_gizclaw_close(&state.bridge);
  if (cleanup == H2_PAL_OK)
    cleanup = detach_service();
  if (cleanup == H2_PAL_OK)
    cleanup = h2_gizclaw_device_stop(state.device);
  if (cleanup == H2_PAL_OK && !state.host)
    h2_gizclaw_lua_app_lease_release(&state.run.lease);
  if (cleanup == H2_PAL_OK)
    cleanup = h2_gizclaw_device_destroy(&state.device);
  if (cleanup == H2_PAL_OK)
    cleanup = h2_web_gizclaw_apps_close(&state.installer);
  if (cleanup == H2_PAL_OK)
    cleanup = h2_gizclaw_lua_app_catalog_destroy(state.apps);
  if (cleanup == H2_PAL_OK)
    state.apps = NULL;
  if (cleanup == H2_PAL_OK)
    cleanup = h2_runtime_destroy(&state.runtime);
  if (cleanup != H2_PAL_OK || state.host || state.run.lease) {
    printf("H2_WEB_APP name=local-device result=FAIL rc=%d retained=1\n",
           cleanup);
    return 2;
  }
  int closed = h2_web_fs_close(state.fs);
  int destroyed = closed == H2_PAL_OK ? h2_web_platform_destroy(state.platform)
                                      : H2_PAL_ERR_BUSY;
  if (rc == H2_PAL_OK)
    rc = closed != H2_PAL_OK ? closed : destroyed;
  printf("H2_WEB_APP name=local-device result=%s rc=%d fs=%d destroy=%d\n",
         rc == H2_PAL_OK ? "PASS" : "FAIL", rc, closed, destroyed);
  return rc == H2_PAL_OK ? 0 : 1;
}
