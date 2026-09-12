#include "h2_web_app_host.h"

#include "h2/pal/h2_pal_unsupported.h"
#include "h2_lvgl_platform.h"
#include "h2_web_fs.h"

#include <emscripten.h>
#include <stdio.h>

struct h2_web_app_host {
  const h2_web_app_host_config_t *config;
  h2_web_platform_t *platform;
  h2_runtime_t *runtime;
  h2_web_app_host_entry_fn entry;
  void *user;
  const h2_pal_fs_api_t *fs;
  h2_pal_task_t *volatile app_task;
  double stop_at_ms;
  h2_pal_result_t result;
  volatile int done;
};

// Time an App gets to honour should_stop before its task is cancelled.
#define H2_WEB_APP_HOST_STOP_GRACE_MS 2000.0

static volatile int s_stop_requested;
static h2_web_app_host_t *s_host;

static const h2_pal_periph_single_button_payload_t s_button_payload = {
    .delivery = H2_PAL_BUTTON_DELIVERY_PUSH_EDGE,
};

// Peripheral ids are button index + 1.
static h2_pal_periph_info_t h2_web_app_host_button_info(size_t index) {
  h2_pal_periph_info_t info = {
      .id = (h2_pal_periph_id_t)(index + 1u),
      .type = H2_PAL_PERIPH_TYPE_SINGLE_BUTTON,
      .payload = &s_button_payload,
      .payload_size = sizeof(s_button_payload),
  };
  const h2_web_app_host_button_t *button = &s_host->config->buttons[index];
  (void)snprintf(info.name, sizeof(info.name), "%s",
                 button->name != NULL  ? button->name
                 : button->key != NULL ? button->key
                                       : "button");
  return info;
}

static h2_pal_result_t h2_web_app_host_list_components(
    void *user, h2_runtime_component_t filter,
    h2_runtime_component_mapping_cb_t callback, void *callback_user) {
  (void)user;
  if (callback == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  if (filter != H2_RUNTIME_COMPONENT_BUTTON || s_host == NULL)
    return H2_PAL_OK;
  for (size_t index = 0u; index < s_host->config->button_count; ++index) {
    const h2_runtime_component_mapping_entry_t entry = {
        s_host->config->buttons[index].component_id,
        (h2_pal_periph_id_t)(index + 1u)};
    const h2_pal_result_t result = callback(callback_user, &entry);
    if (result != H2_PAL_OK)
      return result;
  }
  return H2_PAL_OK;
}

static h2_pal_result_t h2_web_app_host_component_periph(
    void *user, h2_runtime_component_id_t component_id,
    h2_pal_periph_id_t *out_periph_id) {
  (void)user;
  if (out_periph_id == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  for (size_t index = 0u; s_host != NULL && index < s_host->config->button_count;
       ++index) {
    if (s_host->config->buttons[index].component_id == component_id) {
      *out_periph_id = (h2_pal_periph_id_t)(index + 1u);
      return H2_PAL_OK;
    }
  }
  return H2_PAL_ERR_NOT_FOUND;
}

static h2_pal_result_t h2_web_app_host_list_periph(
    void *user, h2_pal_periph_type_t filter, h2_pal_periph_cb_t callback,
    void *callback_user) {
  (void)user;
  if (callback == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  if (s_host == NULL || (filter != H2_PAL_PERIPH_TYPE_ANY &&
                         filter != H2_PAL_PERIPH_TYPE_SINGLE_BUTTON))
    return H2_PAL_OK;
  for (size_t index = 0u; index < s_host->config->button_count; ++index) {
    const h2_pal_periph_info_t info = h2_web_app_host_button_info(index);
    const h2_pal_result_t result = callback(callback_user, &info);
    if (result != H2_PAL_OK)
      return result;
  }
  return H2_PAL_OK;
}

static h2_pal_result_t h2_web_app_host_get_periph(
    void *user, h2_pal_periph_id_t id, h2_pal_periph_info_t *out_info) {
  (void)user;
  if (out_info == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  if (s_host == NULL || id == 0u || id > s_host->config->button_count)
    return H2_PAL_ERR_NOT_FOUND;
  *out_info = h2_web_app_host_button_info((size_t)id - 1u);
  return H2_PAL_OK;
}

static const h2_runtime_component_mapper_vtable_t s_mapper_vtable = {
    .list = h2_web_app_host_list_components,
    .get_periph_id = h2_web_app_host_component_periph,
};
static const h2_runtime_component_mapper_t s_mapper = {
    .vtable = &s_mapper_vtable,
};
static const h2_pal_periph_vtable_t s_periph_vtable = {
    .list = h2_web_app_host_list_periph,
    .get = h2_web_app_host_get_periph,
};
static const h2_pal_periph_api_t s_periph = {
    .vtable = &s_periph_vtable,
};

/* Called by the shell with the button index and 1 down / 0 up. */
EMSCRIPTEN_KEEPALIVE int h2_web_app_host_button(int index, int pressed) {
  if (s_host == NULL || s_host->runtime == NULL || index < 0 ||
      (size_t)index >= s_host->config->button_count)
    return H2_PAL_ERR_UNAVAILABLE;
  // Edges are addressed by peripheral id; the mapper resolves the component.
  return h2_runtime_button_push_edge(
      s_host->runtime, (h2_pal_periph_id_t)(index + 1),
      pressed ? H2_RUNTIME_BUTTON_EDGE_DOWN : H2_RUNTIME_BUTTON_EDGE_UP);
}

/* The shell's JavaScript owns keyboard and layout input for each Button. */
EM_JS(void, h2_web_app_host_bind_button, (int index, const char *key,
                                          const char *name), {
  const bind = globalThis.h2WebAppHostBindButton;
  if (bind)
    bind(index, key ? UTF8ToString(key) : "", name ? UTF8ToString(name) : "");
});

EM_JS(void, h2_web_app_host_unbind_buttons, (), {
  globalThis.h2WebAppHostUnbindButtons?.();
});

EM_JS(void, h2_web_app_host_status, (const char *text), {
  const status = globalThis.document && document.getElementById('status');
  if (status) status.textContent = UTF8ToString(text);
});

EM_JS(void, h2_web_app_host_size_canvas, (int width, int height), {
  const canvas = Module['canvas'];
  if (canvas) {
    canvas.width = width;
    canvas.height = height;
  }
});

/* Shells call this from a Stop button; endless Apps end cooperatively. */
EMSCRIPTEN_KEEPALIVE void h2_web_app_host_request_stop(void) {
  s_stop_requested = 1;
}

static void h2_web_app_host_mark(const h2_web_app_host_t *host,
                                 const char *stage) {
  char line[160];
  (void)snprintf(line, sizeof(line), "H2_WEB_APP name=%s stage=%s",
                 host->config->name, stage);
  puts(line);
  h2_web_app_host_status(line);
}

int h2_web_app_host_should_stop(void *user) {
  const h2_web_app_host_t *host = user;
  if (s_stop_requested)
    return 1;
  if (host != NULL && host->stop_at_ms > 0.0 &&
      emscripten_get_now() >= host->stop_at_ms) {
    s_stop_requested = 1;
    h2_web_app_host_mark(host, "stop-requested");
    return 1;
  }
  return 0;
}

h2_pal_result_t h2_web_app_host_ready(void *user) {
  h2_web_app_host_mark(user, "ready");
  return H2_PAL_OK;
}

h2_web_platform_t *h2_web_app_host_platform(h2_web_app_host_t *host) {
  return host == NULL ? NULL : host->platform;
}

static h2_runtime_config_t h2_web_app_host_runtime_config(
    h2_web_app_host_t *host, const h2_pal_fs_api_t *fs) {
  h2_web_platform_t *platform = host->platform;
  h2_runtime_config_t config = {0};
  config.board = "browser";
  config.target = "webassembly";
  config.chip = "wasm32";
  config.firmware_info = h2_pal_unsupported_firmware_info_api();
  config.mem = h2_web_platform_mem_api();
  config.log = h2_web_platform_log_api();
  config.time = h2_web_platform_time_api(platform);
  config.timer = h2_web_platform_timer_api(platform);
  config.task = h2_web_platform_task_api(platform);
  config.queue = h2_web_platform_queue_api(platform);
  config.sync = h2_web_platform_sync_api(platform);
  config.fs = fs != NULL ? fs : h2_pal_unsupported_fs_api();
  config.disk = h2_pal_unsupported_disk_api();
  config.pref = h2_web_platform_pref_api(platform);
  config.crypto = h2_web_platform_crypto_api(platform);
  config.http = h2_web_platform_http_api(platform);
  config.net = h2_pal_unsupported_net_api();
  config.netif = h2_web_platform_netif_api(platform);
  config.mqtt = h2_pal_unsupported_mqtt_api();
  config.webrtc = h2_web_platform_webrtc_api(platform);
  config.wifi_sta = h2_pal_unsupported_wifi_sta_api();
  config.wifi_ap = h2_pal_unsupported_wifi_ap_api();
  config.wifi_csi = h2_pal_unsupported_wifi_csi_api();
  config.wifi_settings = h2_pal_unsupported_wifi_settings_api();
  config.ble_host = h2_pal_unsupported_ble_host_api();
  config.modem = h2_pal_unsupported_modem_api();
  config.power = h2_pal_unsupported_power_api();
  config.display = h2_web_platform_display_api(platform);
  config.audio = h2_web_platform_audio_api(platform);
  config.audio_decoder = h2_web_platform_audio_decoder_api(platform);
  config.periph = host->config->button_count != 0u
                      ? &s_periph
                      : h2_pal_unsupported_periph_api();
  config.button = h2_pal_unsupported_button_api();
  config.touch = h2_web_platform_touch_api(platform);
  config.buzzer = h2_pal_unsupported_buzzer_api();
  config.nfc = h2_pal_unsupported_nfc_api();
  config.nfc_card_emulation = h2_pal_unsupported_nfc_card_emulation_api();
  config.imu = h2_pal_unsupported_imu_api();
  config.gpio_irq = h2_pal_unsupported_gpio_irq_api();
  config.led = h2_pal_unsupported_led_api();
  config.switch_api = h2_pal_unsupported_switch_api();
  config.pwm_switch = h2_pal_unsupported_pwm_switch_api();
  config.input = h2_pal_unsupported_input_api();
  config.system_event = h2_web_platform_system_event_api(platform);
  config.video_decoder = h2_web_platform_video_decoder_api(platform);
  config.component_mapper =
      host->config->button_count != 0u ? &s_mapper : NULL;
  config.event_queue_capacity = H2_RUNTIME_DEFAULT_EVENT_QUEUE_CAPACITY;
  return config;
}

static void h2_web_app_host_app_task(void *user) {
  h2_web_app_host_t *host = user;
  h2_web_app_host_mark(host, "running");
  host->result = host->entry(host, host->runtime, host->user);
}

/*
 * The Runtime lives inside this task: its deinit joins Runtime-owned tasks,
 * which only a task can wait for in the browser. The App itself runs in a
 * nested task so a stop can cancel it without cancelling the teardown.
 */
static void h2_web_app_host_task(void *user) {
  h2_web_app_host_t *host = user;
  const h2_web_app_host_config_t *config = host->config;
  const h2_runtime_config_t runtime_config =
      h2_web_app_host_runtime_config(host, host->fs);
  h2_pal_result_t result = h2_runtime_init(&runtime_config, &host->runtime);
  if (result == H2_PAL_OK && config->button_count != 0u) {
    result = h2_runtime_input_start(host->runtime, NULL);
    for (size_t index = 0u; result == H2_PAL_OK && index < config->button_count;
         ++index)
      h2_web_app_host_bind_button((int)index, config->buttons[index].key,
                                  config->buttons[index].name);
  }
  if (result == H2_PAL_OK) {
    if (config->run_ms != 0u)
      host->stop_at_ms = emscripten_get_now() + (double)config->run_ms;
    const h2_pal_task_options_t options = {
        .name = "web-app",
        .min_stack_size = config->stack_size != 0u ? config->stack_size
                                                   : 65536u,
    };
    h2_pal_task_t *app_task = NULL;
    host->result = H2_PAL_ERR_TASK;
    result = h2_pal_task_start(host->runtime->task, &options,
                               h2_web_app_host_app_task, host, &app_task);
    host->app_task = app_task;
  }
  if (result == H2_PAL_OK) {
    const h2_pal_result_t joined =
        h2_pal_task_join(host->runtime->task, host->app_task);
    host->app_task = NULL;
    result = host->result;
    // A requested stop that ends the App through cancellation is normal.
    if (joined == H2_PAL_EXIT ||
        (s_stop_requested &&
         (result == H2_PAL_EXIT || result == H2_PAL_ERR_CLOSED)))
      result = s_stop_requested ? H2_PAL_OK : H2_PAL_EXIT;
  }
  h2_web_app_host_unbind_buttons();
  if (host->runtime != NULL)
    h2_runtime_deinit(host->runtime);
  host->runtime = NULL;
  host->result = result;
  host->done = 1;
}

int h2_web_app_host_run(const h2_web_app_host_config_t *config,
                        h2_web_app_host_entry_fn entry, void *user) {
  if (config == NULL || config->name == NULL || entry == NULL ||
      config->button_count > H2_WEB_APP_HOST_MAX_BUTTONS ||
      (config->button_count != 0u && config->buttons == NULL))
    return 1;
  h2_web_app_host_t host = {
      .config = config,
      .entry = entry,
      .user = user,
      .result = H2_PAL_ERR_TASK,
  };
  s_host = &host;
  const h2_web_platform_config_t platform_config = {
      .display_width = config->display_width,
      .display_height = config->display_height,
  };
  h2_web_app_host_size_canvas(config->display_width, config->display_height);
  host.platform = h2_web_platform_create(&platform_config);
  h2_pal_result_t result =
      host.platform != NULL ? H2_PAL_OK : H2_PAL_ERR_NO_MEMORY;
  h2_web_fs_t *fs = NULL;
  if (result == H2_PAL_OK && config->persistent_root != NULL) {
    const h2_web_fs_config_t fs_config = {
        .persistent_root = config->persistent_root,
        .readonly_roots = config->readonly_roots,
        .readonly_root_count = config->readonly_root_count,
    };
    result = h2_web_fs_open(host.platform, &fs_config, &fs);
    host.fs = h2_web_fs_api(fs);
  }
  int lvgl = 0;
  if (result == H2_PAL_OK && config->lvgl) {
    const h2_lvgl_platform_config_t lvgl_config = {
        .allocator = h2_web_platform_mem_api(),
        .task_api = h2_web_platform_task_api(host.platform),
        .sync_api = h2_web_platform_sync_api(host.platform),
        .queue_api = h2_web_platform_queue_api(host.platform),
        .time_api = h2_web_platform_time_api(host.platform),
    };
    result = h2_lvgl_platform_init(&lvgl_config) == 0 ? H2_PAL_OK
                                                      : H2_PAL_ERR_UNAVAILABLE;
    lvgl = result == H2_PAL_OK;
  }
  h2_pal_task_t *task = NULL;
  const h2_pal_task_api_t *tasks =
      host.platform != NULL ? h2_web_platform_task_api(host.platform) : NULL;
  if (result == H2_PAL_OK) {
    const h2_pal_task_options_t options = {
        .name = "web-app-host",
        .min_stack_size = 65536u,
    };
    result = h2_pal_task_start(tasks, &options, h2_web_app_host_task, &host,
                               &task);
  }
  double cancel_at_ms = 0.0;
  int cancelled = 0;
  while (result == H2_PAL_OK && !host.done) {
    // Apps without a should_stop hook are cancelled cooperatively: their
    // next PAL wait returns EXIT once the stop grace period has passed.
    if (!cancelled && host.app_task != NULL &&
        h2_web_app_host_should_stop(&host)) {
      if (cancel_at_ms == 0.0) {
        cancel_at_ms = emscripten_get_now() + H2_WEB_APP_HOST_STOP_GRACE_MS;
      } else if (emscripten_get_now() >= cancel_at_ms) {
        cancelled = 1;
        h2_web_app_host_mark(&host, "cancel");
        (void)h2_web_platform_task_cancel(host.platform, host.app_task);
      }
    }
    result = h2_web_platform_pump(host.platform, 64u, NULL);
    if (result == H2_PAL_OK && !host.done)
      emscripten_sleep(1u);
  }
  while (task != NULL) {
    const h2_pal_result_t joined = h2_pal_task_join(tasks, task);
    if (joined != H2_PAL_ERR_BUSY) {
      if (result == H2_PAL_OK && joined != H2_PAL_OK)
        result = joined;
      break;
    }
    (void)h2_web_platform_pump(host.platform, 64u, NULL);
    emscripten_sleep(1u);
  }
  if (result == H2_PAL_OK)
    result = host.result;
  s_host = NULL;
  if (lvgl)
    h2_lvgl_platform_deinit();
  const h2_pal_result_t fs_result = h2_web_fs_close(fs);
  const h2_pal_result_t destroy_result = h2_web_platform_destroy(host.platform);
  const int pass = result == H2_PAL_OK && fs_result == H2_PAL_OK &&
                   destroy_result == H2_PAL_OK && host.platform != NULL;
  char line[192];
  (void)snprintf(line, sizeof(line),
                 "H2_WEB_APP name=%s result=%s rc=%d fs=%d destroy=%d",
                 config->name, pass ? "PASS" : "FAIL", result, fs_result,
                 destroy_result);
  puts(line);
  h2_web_app_host_status(line);
  return pass ? 0 : 1;
}
