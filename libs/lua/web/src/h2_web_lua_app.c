/* Entry for one embedded Lua script. Board metadata supplies dimensions and
 * Button names/keys; this file owns its Web provider, Runtime and Lua job. */
#include "h2/pal/h2_pal_unsupported.h"
#include "h2_lua.h"
#include "h2_lua_event.h"
#include "h2_lua_job.h"
#include "h2_web_board_config.h"
#include "h2_web_lua_app_config.h"
#include "h2_web_main_thread.h"
#include "h2_web_platform.h"
#if H2_WEB_LUA_APP_EXTENSION
#include "h2_web_lua_app.h"
#endif
#include <emscripten.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

/* Static entry ownership survives a failed checked shutdown. */
static h2_web_platform_t *owned_platform;
static h2_runtime_t *owned_runtime;
static h2_lua_host_t *owned_lua;
static int retained_error, input_error, stop_reported;
static double stop_at;

static const h2_pal_periph_single_button_payload_t button_payload = {
    .delivery = H2_PAL_BUTTON_DELIVERY_PUSH_EDGE};
static h2_pal_result_t periph_get(void *user, h2_pal_periph_id_t id,
                                  h2_pal_periph_info_t *out) {
  (void)user;
  if (out == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  if (id == 0u || id > H2_WEB_BOARD_BUTTON_COUNT)
    return H2_PAL_ERR_NOT_FOUND;
  *out = (h2_pal_periph_info_t){.id = id,
                                .type = H2_PAL_PERIPH_TYPE_SINGLE_BUTTON,
                                .payload = &button_payload,
                                .payload_size = sizeof(button_payload)};
  (void)snprintf(out->name, sizeof(out->name), "%s",
                 h2_web_board_buttons[id - 1u].name);
  return H2_PAL_OK;
}
static h2_pal_result_t periph_list(void *user, h2_pal_periph_type_t filter,
                                   h2_pal_periph_cb_t cb, void *cb_user) {
  if (cb == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  if (filter != H2_PAL_PERIPH_TYPE_ANY &&
      filter != H2_PAL_PERIPH_TYPE_SINGLE_BUTTON)
    return H2_PAL_OK;
  for (size_t i = 0; i < H2_WEB_BOARD_BUTTON_COUNT; ++i) {
    h2_pal_periph_info_t info;
    (void)periph_get(user, (h2_pal_periph_id_t)(i + 1u), &info);
    h2_pal_result_t rc = cb(cb_user, &info);
    if (rc != H2_PAL_OK)
      return rc;
  }
  return H2_PAL_OK;
}
static h2_pal_result_t mapper_get(void *user, h2_runtime_component_id_t id,
                                  h2_pal_periph_id_t *out) {
  (void)user;
  if (out == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  if (id == 0u || id > H2_WEB_BOARD_BUTTON_COUNT)
    return H2_PAL_ERR_NOT_FOUND;
  *out = id;
  return H2_PAL_OK;
}
static h2_pal_result_t mapper_list(void *user, h2_runtime_component_t filter,
                                   h2_runtime_component_mapping_cb_t cb,
                                   void *cb_user) {
  (void)user;
  if (cb == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  if (filter != H2_RUNTIME_COMPONENT_NONE &&
      filter != H2_RUNTIME_COMPONENT_BUTTON)
    return H2_PAL_OK;
  for (size_t i = 0; i < H2_WEB_BOARD_BUTTON_COUNT; ++i) {
    const h2_runtime_component_mapping_entry_t entry = {
        (h2_runtime_component_id_t)(i + 1u), (h2_pal_periph_id_t)(i + 1u)};
    h2_pal_result_t rc = cb(cb_user, &entry);
    if (rc != H2_PAL_OK)
      return rc;
  }
  return H2_PAL_OK;
}
static const h2_pal_periph_vtable_t periph_vtable = {periph_list, periph_get};
static const h2_pal_periph_api_t periph = {.vtable = &periph_vtable};
static const h2_runtime_component_mapper_vtable_t mapper_vtable = {mapper_list,
                                                                   mapper_get};
static const h2_runtime_component_mapper_t mapper = {.vtable = &mapper_vtable};

/* The page merges keyboard/pointer sources and queues edges. Runtime calls
 * happen here on the entry Worker, never from a browser event callback. */
/* clang-format off */
EM_JS(void, page_input,
      (void *context, h2_web_main_result_t *result, h2_web_main_completion_t *completion), {
  h2WebMain(context, result, completion, ["i32", "i32", "pointer", "pointer"], "i32",
    (action, index, key, name) => {
      if (action === 1) { globalThis.h2WebBindButton?.(index, UTF8ToString(key), UTF8ToString(name)); return 0; }
      if (action === 2) { globalThis.h2WebUnbindButtons?.(); return 0; }
      if (action === 3) return Module.h2WebStopRequested ? 1 : 0;
      return globalThis.h2WebTakeButtonEdge?.() || 0;
    });
});
/* clang-format on */
static int page_action(int action, int index, const char *key,
                       const char *name) {
  return h2_web_main_call(page_input,
                          (const void *[]){&action, &index, &key, &name})
      .i32;
}
static int should_stop(void) {
  int edge;
  while ((edge = page_action(0, 0, NULL, NULL)) != 0) {
    input_error = edge < 0 ? H2_PAL_ERR_NO_SPACE
                           : h2_runtime_button_push_edge(
                                 owned_runtime, (h2_pal_periph_id_t)(edge >> 1),
                                 (edge & 1) ? H2_RUNTIME_BUTTON_EDGE_DOWN
                                            : H2_RUNTIME_BUTTON_EDGE_UP);
    if (input_error != H2_PAL_OK)
      return 1;
  }
  int stop = page_action(3, 0, NULL, NULL) ||
             (stop_at > 0.0 && emscripten_get_now() >= stop_at);
  if (stop && !stop_reported) {
    stop_reported = 1;
    puts("H2_WEB_APP name=" H2_WEB_LUA_APP_NAME " stage=stop-requested");
  }
  return stop;
}

/* Component id of the exit Button (its board index + 1), or 0 for none. */
static h2_runtime_component_id_t exit_component(void) {
  for (size_t i = 0u; i < H2_WEB_BOARD_BUTTON_COUNT; ++i) {
    if (strcmp(h2_web_board_buttons[i].name, H2_WEB_LUA_APP_EXIT_BUTTON) == 0)
      return (h2_runtime_component_id_t)(i + 1u);
  }
  return H2_RUNTIME_COMPONENT_ID_NONE;
}

static bool exit_requested(const h2_runtime_event_t *event) {
#if H2_WEB_LUA_APP_EXTENSION
  if (h2_web_lua_app_extension.exit_requested != NULL)
    return h2_web_lua_app_extension.exit_requested(event);
#endif
  return event->kind == H2_RUNTIME_COMPONENT_EVENT_BUTTON_ACTION &&
         event->payload_size >= sizeof(h2_runtime_button_action_event_t) &&
         h2_runtime_button_action_is_released(event->payload);
}

static h2_pal_result_t run_script(h2_runtime_t *runtime) {
  static h2_lua_resource_t resource;
  resource = (h2_lua_resource_t){
      .name = "app.lua",
      .source = H2_WEB_LUA_APP_SOURCE,
      .source_size = H2_WEB_LUA_APP_SOURCE_SIZE,
  };
  char ids[H2_WEB_BOARD_MAX_BUTTONS][12];
  h2_lua_arg_t args[H2_WEB_BOARD_MAX_BUTTONS + H2_WEB_LUA_APP_ARG_COUNT];
  const size_t button_count = H2_WEB_BOARD_BUTTON_COUNT;
  const h2_runtime_component_id_t exit_id = exit_component();
  uint8_t payload[H2_RUNTIME_EVENT_PAYLOAD_MAX];
  h2_runtime_event_t event = {.payload = payload,
                              .payload_capacity = sizeof(payload)};
  h2_lua_host_t *host = NULL;
  h2_lua_job_id_t job = H2_LUA_JOB_ID_NONE;
  h2_lua_job_status_t status = {0};
  bool ready = false;
  /* Set once a cancel was accepted, so later events do not repeat it. */
  bool cancelling = false;
  for (size_t i = 0u; i < button_count; ++i) {
    (void)snprintf(ids[i], sizeof(ids[i]), "%u", (unsigned)(i + 1u));
    args[i] =
        (h2_lua_arg_t){.name = h2_web_board_buttons[i].name, .value = ids[i]};
  }
  for (size_t i = 0u; i < H2_WEB_LUA_APP_ARG_COUNT; ++i)
    args[button_count + i] = h2_web_lua_script_args[i];
  h2_pal_result_t rc = h2_lua_host_create(
      &(h2_lua_host_config_t){
          .runtime = runtime,
          .worker_count = 1u,
          .max_jobs = 1u,
          .event_delivery_capacity = 32u,
          .callback_capacity_per_job = 16u,
          .vm_memory_limit_bytes = H2_WEB_LUA_APP_VM_BYTES,
          .source_limit_bytes = H2_WEB_LUA_APP_SOURCE_LIMIT_BYTES,
          .output_limit_bytes = 1024u,
          .instruction_quantum = 10000u,
          .execution_timeout_ms = UINT32_MAX,
          .resources = &resource,
          .resource_count = 1u,
      },
      &host);
  if (rc != H2_PAL_OK)
    return rc;
  owned_lua = host;
#if H2_WEB_LUA_APP_EXTENSION
  if (h2_web_lua_app_extension.register_host != NULL)
    rc = h2_web_lua_app_extension.register_host(host);
#endif
  if (rc == H2_PAL_OK)
    rc = h2_lua_host_start(host);
  if (rc == H2_PAL_OK)
    rc = h2_lua_job_submit_resource(host, NULL, "app.lua", args,
                                    button_count + H2_WEB_LUA_APP_ARG_COUNT,
                                    &job);
  while (rc == H2_PAL_OK) {
    if (!cancelling && should_stop()) {
      rc = h2_lua_job_cancel(host, job);
      cancelling = rc == H2_PAL_OK;
    }
    for (; rc == H2_PAL_OK;) {
      const h2_pal_result_t poll = h2_runtime_poll_event(runtime, &event);
      if (poll == H2_PAL_ERR_WOULD_BLOCK || poll == H2_PAL_ERR_TIMEOUT)
        break;
      if (poll != H2_PAL_OK) {
        rc = poll;
      } else if (event.component != H2_RUNTIME_COMPONENT_BUTTON || cancelling) {
        continue;
      } else if (exit_id != H2_RUNTIME_COMPONENT_ID_NONE &&
                 event.component_id == exit_id) {
        if (exit_requested(&event)) {
          rc = h2_lua_job_cancel(host, job);
          cancelling = rc == H2_PAL_OK;
        }
      } else {
        rc = h2_lua_dispatch_runtime_event(host, job, &event);
      }
    }
    if (rc == H2_PAL_OK)
      rc = h2_lua_host_step(host);
    if (rc == H2_PAL_OK)
      rc = h2_lua_job_get_status(host, job, &status);
    if (rc != H2_PAL_OK || status.state >= H2_LUA_JOB_SUCCEEDED)
      break;
    if (!ready && status.state == H2_LUA_JOB_WAITING) {
      ready = true;
      puts("H2_WEB_APP name=" H2_WEB_LUA_APP_NAME " stage=ready");
      if (rc != H2_PAL_OK)
        break;
    }
    rc = h2_pal_time_sleep_ms(runtime->time, 5u);
  }
  if (job != H2_LUA_JOB_ID_NONE &&
      h2_lua_job_get_status(host, job, &status) == H2_PAL_OK) {
    if (status.state == H2_LUA_JOB_FAILED ||
        status.state == H2_LUA_JOB_TIMED_OUT) {
      printf("H2_WEB_LUA_APP job state=%d message=%s\n", (int)status.state,
             status.message);
      if (rc == H2_PAL_OK)
        rc = H2_PAL_ERR_INVALID_STATE;
    }
    if (status.state >= H2_LUA_JOB_SUCCEEDED)
      (void)h2_lua_job_release(host, job);
  }
  /* Keep Runtime/provider ownership if Lua cannot release its consumers. */
  h2_pal_result_t cleanup = h2_lua_host_destroy_checked(host);
  while (cleanup == H2_PAL_ERR_BUSY) {
    h2_pal_result_t wait = h2_pal_time_sleep_ms(runtime->time, 1u);
    if (wait != H2_PAL_OK) {
      cleanup = wait;
      break;
    }
    cleanup = h2_lua_host_destroy_checked(host);
  }
  if (cleanup != H2_PAL_OK) {
    retained_error = cleanup;
    return cleanup;
  }
  owned_lua = NULL;
  /* A cancelled job (exit Button or Stop) ends the App with OK. */
  return rc;
}

int main(void) {
  if (owned_platform != NULL || owned_runtime != NULL || owned_lua != NULL)
    return 2;
  retained_error = input_error = H2_PAL_OK;
  stop_reported = 0;
  const h2_web_platform_config_t platform_config = {H2_WEB_BOARD_WIDTH,
                                                    H2_WEB_BOARD_HEIGHT};
  owned_platform = h2_web_platform_create(&platform_config);
  if (owned_platform == NULL)
    return 1;
  h2_runtime_config_t rcfg = {0};
  rcfg.board = "browser";
  rcfg.target = "webassembly";
  rcfg.chip = "wasm32";
  rcfg.firmware_info = h2_pal_unsupported_firmware_info_api();
  rcfg.mem = h2_web_platform_mem_api();
  rcfg.log = h2_web_platform_log_api();
  rcfg.time = h2_web_platform_time_api(owned_platform);
  rcfg.timer = h2_web_platform_timer_api(owned_platform);
  rcfg.task = h2_web_platform_task_api(owned_platform);
  rcfg.queue = h2_web_platform_queue_api(owned_platform);
  rcfg.sync = h2_web_platform_sync_api(owned_platform);
  rcfg.fs = h2_pal_unsupported_fs_api();
  rcfg.disk = h2_pal_unsupported_disk_api();
  rcfg.pref = h2_web_platform_pref_api(owned_platform);
  rcfg.crypto = h2_web_platform_crypto_api(owned_platform);
  rcfg.http = h2_web_platform_http_api(owned_platform);
  rcfg.net = h2_pal_unsupported_net_api();
  rcfg.netif = h2_web_platform_netif_api(owned_platform);
  rcfg.mqtt = h2_pal_unsupported_mqtt_api();
  rcfg.webrtc = h2_web_platform_webrtc_api(owned_platform);
  rcfg.wifi_sta = h2_pal_unsupported_wifi_sta_api();
  rcfg.wifi_ap = h2_pal_unsupported_wifi_ap_api();
  rcfg.wifi_csi = h2_pal_unsupported_wifi_csi_api();
  rcfg.wifi_settings = h2_pal_unsupported_wifi_settings_api();
  rcfg.ble_host = h2_pal_unsupported_ble_host_api();
  rcfg.modem = h2_pal_unsupported_modem_api();
  if (h2_web_platform_fake_wifi_sta_api(owned_platform) != NULL) {
    rcfg.wifi_sta = h2_web_platform_fake_wifi_sta_api(owned_platform);
    rcfg.wifi_settings = h2_web_platform_fake_wifi_settings_api(owned_platform);
    rcfg.modem = h2_web_platform_fake_modem_api(owned_platform);
  }
  rcfg.power = h2_pal_unsupported_power_api();
  rcfg.display = h2_web_platform_display_api(owned_platform);
  rcfg.audio = h2_web_platform_audio_api(owned_platform);
  rcfg.audio_decoder = h2_web_platform_audio_decoder_api(owned_platform);
  rcfg.periph = H2_WEB_BOARD_BUTTON_COUNT != 0u
                    ? &periph
                    : h2_pal_unsupported_periph_api();
  rcfg.button = h2_pal_unsupported_button_api();
  rcfg.touch = h2_web_platform_touch_api(owned_platform);
  rcfg.buzzer = h2_pal_unsupported_buzzer_api();
  rcfg.nfc = h2_pal_unsupported_nfc_api();
  rcfg.nfc_card_emulation = h2_pal_unsupported_nfc_card_emulation_api();
  rcfg.imu = h2_pal_unsupported_imu_api();
  rcfg.gpio_irq = h2_pal_unsupported_gpio_irq_api();
  rcfg.led = h2_pal_unsupported_led_api();
  rcfg.switch_api = h2_pal_unsupported_switch_api();
  rcfg.pwm_switch = h2_pal_unsupported_pwm_switch_api();
  rcfg.input = h2_pal_unsupported_input_api();
  rcfg.system_event = h2_web_platform_system_event_api(owned_platform);
  rcfg.video_decoder = h2_web_platform_video_decoder_api(owned_platform);
  rcfg.component_mapper = H2_WEB_BOARD_BUTTON_COUNT != 0u ? &mapper : NULL;
  rcfg.event_queue_capacity = H2_RUNTIME_DEFAULT_EVENT_QUEUE_CAPACITY;
  int rc = h2_runtime_init(&rcfg, &owned_runtime);
  if (rc == H2_PAL_OK)
    rc = h2_runtime_input_start(owned_runtime, NULL);
  if (rc == H2_PAL_OK) {
    for (size_t i = 0; i < H2_WEB_BOARD_BUTTON_COUNT; ++i)
      (void)page_action(1, (int)i, h2_web_board_buttons[i].key,
                        h2_web_board_buttons[i].name);
    stop_at = H2_WEB_LUA_APP_RUN_MS != 0u
                  ? emscripten_get_now() + H2_WEB_LUA_APP_RUN_MS
                  : 0.0;
    puts("H2_WEB_APP name=" H2_WEB_LUA_APP_NAME " stage=running");
    rc = run_script(owned_runtime);
    (void)page_action(2, 0, NULL, NULL);
    if (rc == H2_PAL_OK)
      rc = input_error;
  }
  if (retained_error == H2_PAL_OK && owned_runtime != NULL)
    retained_error = h2_runtime_input_stop(owned_runtime);
  if (retained_error != H2_PAL_OK) {
    printf("H2_WEB_APP name=%s result=FAIL rc=%d retained=1\n",
           H2_WEB_LUA_APP_NAME, retained_error);
    return 2;
  }
  h2_runtime_deinit(owned_runtime);
  owned_runtime = NULL;
  int destroyed = h2_web_platform_destroy(owned_platform);
  if (destroyed != H2_PAL_OK) {
    printf("H2_WEB_APP name=%s result=FAIL rc=%d retained=1\n",
           H2_WEB_LUA_APP_NAME, destroyed);
    return 2;
  }
  owned_platform = NULL;
  printf("H2_WEB_APP name=%s result=%s rc=%d fs=0 destroy=0\n",
         H2_WEB_LUA_APP_NAME, rc == H2_PAL_OK ? "PASS" : "FAIL", rc);
  return rc == H2_PAL_OK ? 0 : 1;
}
