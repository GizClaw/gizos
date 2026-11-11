/* G99 Web entry: owns the Web PAL, Runtime and page input worker, then runs
 * the portable G99 Main until the page's Stop. Main owns its own workers.
 *
 * Built with H2_G99_WEB_E2E, the same entry runs the G99 product E2E suite
 * instead: the page's keys stay unbound (the suite owns input through Runtime
 * Test Control), `?select=` chooses cases, and the page result follows the
 * suite's exit code. */
#include "h2_pal_unsupported.h"
#include "h2_atomic_static.h"
#include "h2_g99.h"
#include "h2_web_board_config.h"
#include "h2_web_main_thread.h"
#include "h2_web_platform.h"
#include "web_buttons.h"
#if H2_G99_WEB_E2E
#include "h2_g99_e2e.h"
#endif

#include <emscripten.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifndef H2_G99_WEB_NAME
#error "the h2_web_app target must define H2_G99_WEB_NAME"
#endif
#ifndef H2_G99_WEB_E2E
#define H2_G99_WEB_E2E 0
#endif

static struct {
  h2_web_platform_t* platform;
  h2_runtime_t* runtime;
  h2_pal_task_t* input_task;
  h2_pal_result_t input_result;
  h2_runtime_component_id_t buttons[H2_WEB_BOARD_MAX_BUTTONS];
  size_t button_count;
  bool buttons_bound;
  /* Observation bookkeeping, touched only by the Main task. */
  double started_at;
  h2_g99_route_t settled_route;
  uint32_t arrivals;
} s_web;

H2_ATOMIC_DEFINE_STATIC(bool, s_stop_requested, false);
H2_ATOMIC_DEFINE_STATIC(bool, s_input_end, false);

/* ---------- page input ---------- */

/* Only the input worker removes page edges and calls the Runtime; Main reads
 * the atomic stop request. */
/* clang-format off */
EM_JS(void, page_input,
      (void *context, h2_web_main_result_t *result, h2_web_main_completion_t *completion), {
  h2WebMain(context, result, completion, ["i32", "i32", "pointer", "pointer", "i32"], "i32",
    (action, index, key, name, error) => {
      try {
        if (action === 1) {
          if (typeof globalThis.h2WebBindButton !== 'function') return error;
          globalThis.h2WebBindButton(index, UTF8ToString(key), UTF8ToString(name));
          return 0;
        }
        if (action === 2) {
          if (typeof globalThis.h2WebUnbindButtons !== 'function') return error;
          globalThis.h2WebUnbindButtons();
          return 0;
        }
        if (action === 3) return Module.h2WebStopRequested ? 1 : 0;
        if (typeof globalThis.h2WebTakeButtonEdge !== 'function') return error;
        return globalThis.h2WebTakeButtonEdge();
      } catch (_) { return error; }
    });
});
/* clang-format on */

static int page_action(int action, int index, const char* key,
                       const char* name) {
  return h2_web_main_call(page_input,
                          (const void*[]){&action, &index, &key, &name,
                                          &(int){H2_PAL_ERR_IO}})
      .i32;
}

static int should_stop(void* user) {
  (void)user;
  return h2_atomic_bool_load(&s_stop_requested, H2_ATOMIC_ACQUIRE);
}

static bool input_retry(h2_pal_result_t rc) {
  return rc == H2_PAL_ERR_TIMEOUT || rc == H2_PAL_ERR_BUSY ||
         rc == H2_PAL_ERR_WOULD_BLOCK || rc == H2_PAL_ERR_FULL;
}

static void input_worker(void* user) {
  (void)user;
  int pending = 0;
  double rejected_at = 0.0;
  while (s_web.input_result == H2_PAL_OK) {
    const int stop = page_action(3, 0, NULL, NULL);
    if (stop < 0) {
      s_web.input_result = stop;
      break;
    }
    if (stop != 0)
      h2_atomic_bool_store(&s_stop_requested, true, H2_ATOMIC_RELEASE);
    if (pending == 0) pending = page_action(0, 0, NULL, NULL);
    if (pending < 0) {
      s_web.input_result = pending == -1 ? H2_PAL_ERR_NO_SPACE : pending;
      break;
    }
    if (pending != 0) {
      const unsigned index = ((unsigned)pending >> 1u) - 1u;
      if (index >= s_web.button_count) {
        s_web.input_result = H2_PAL_ERR_INVALID_ARG;
        break;
      }
      const h2_pal_result_t rc = h2_g99_web_button_edge(
          s_web.runtime, s_web.buttons[index], pending & 1);
      if (rc == H2_PAL_OK) {
        pending = 0;
        rejected_at = 0.0;
        continue;
      }
      if (!input_retry(rc)) {
        s_web.input_result = rc;
        break;
      }
      /* Keep a rejected edge ahead of every later edge, including release. */
      const double now = emscripten_get_now();
      if (rejected_at == 0.0) rejected_at = now;
      if (now - rejected_at >= 3000.0) {
        s_web.input_result = H2_PAL_ERR_TIMEOUT;
        break;
      }
    } else if (h2_atomic_bool_load(&s_input_end, H2_ATOMIC_ACQUIRE)) {
      break;
    }
    s_web.input_result = h2_pal_time_sleep_ms(s_web.runtime->time, 2u);
  }
  if (s_web.input_result != H2_PAL_OK) {
    printf("H2_G99_WEB input_failed rc=%d\n", s_web.input_result);
    h2_atomic_bool_store(&s_stop_requested, true, H2_ATOMIC_RELEASE);
  }
}

static h2_pal_result_t bind_button(const char* name, const char* key) {
  const h2_runtime_component_id_t component = h2_g99_web_component(name);
  if (component == H2_RUNTIME_COMPONENT_ID_NONE) return H2_PAL_ERR_INVALID_ARG;
  if (s_web.button_count >= H2_WEB_BOARD_MAX_BUTTONS)
    return H2_PAL_ERR_NO_SPACE;
  /* Even a partially failed bind must close DOM admission during cleanup. */
  s_web.buttons_bound = true;
  const h2_pal_result_t rc = page_action(1, (int)s_web.button_count, key, name);
  if (rc == H2_PAL_OK) s_web.buttons[s_web.button_count++] = component;
  return rc;
}

static h2_pal_result_t bind_buttons(void) {
  for (size_t i = 0u; i < H2_WEB_BOARD_BUTTON_COUNT; ++i) {
    const h2_pal_result_t rc =
        bind_button(h2_web_board_buttons[i].name, h2_web_board_buttons[i].key);
    if (rc != H2_PAL_OK) return rc;
  }
  /* Keyboard navigation is entry-owned emulation. Keep these virtual keys
   * out of a public board's physical button inventory. */
  static const struct {
    const char* name;
    const char* key;
  } virtual_keys[] = {
      {"up", "ArrowUp"}, {"down", "ArrowDown"}, {"ok", "Enter"}};
  for (size_t i = 0u; i < sizeof(virtual_keys) / sizeof(virtual_keys[0]); ++i) {
    const h2_runtime_component_id_t component =
        h2_g99_web_component(virtual_keys[i].name);
    bool bound = false;
    for (size_t j = 0u; j < s_web.button_count; ++j)
      if (s_web.buttons[j] == component) bound = true;
    if (bound) continue;
    const h2_pal_result_t rc =
        bind_button(virtual_keys[i].name, virtual_keys[i].key);
    if (rc != H2_PAL_OK) return rc;
  }
  return H2_PAL_OK;
}

/* ---------- observation ---------- */

/* The page reads globalThis.h2G99State; E2E drivers wait on it. */
/* clang-format off */
EM_JS(void, page_state,
      (void *context, h2_web_main_result_t *result, h2_web_main_completion_t *completion), {
  h2WebMain(context, result, completion,
      ["u32", "i32", "i32", "i32", "i32", "i32", "i32", "i32", "i32", "i32",
       "i32", "i32", "i32", "i32", "i32", "i32", "pointer"], null,
      (sequence, lifecycle, route, target_route, transition, main_selection,
       sub_selection, editing, edit_index, input_ready, screen_on, volume,
       brightness, screen_timeout_index, width, height, label) => {
        globalThis.h2G99State = {
          sequence, lifecycle, route, targetRoute: target_route, transition,
          mainSelection: main_selection, subSelection: sub_selection,
          editing: !!editing, editIndex: edit_index, inputReady: !!input_ready,
          screenOn: !!screen_on, volume, brightness,
          screenTimeoutIndex: screen_timeout_index, width, height,
        };
        const element = document.getElementById('status');
        if (element && lifecycle < 3) element.textContent = UTF8ToString(label);
      });
});
/* clang-format on */

static const char* route_name(h2_g99_route_t route) {
  switch (route) {
    case H2_G99_ROUTE_BOOT:
      return "boot";
    case H2_G99_ROUTE_MAIN:
      return "main";
    case H2_G99_ROUTE_LIBRARY:
      return "library";
    case H2_G99_ROUTE_SETTINGS:
      return "settings";
    case H2_G99_ROUTE_APP:
      return "app";
    default:
      return "unknown";
  }
}

static void observe(void* user, const h2_g99_observation_t* o) {
  (void)user;
  if (o == NULL) return;
  /* The suite reports its own lines; a trace per change would drown them. */
  if (!H2_G99_WEB_E2E)
    printf(
        "G99_OBS seq=%u lifecycle=%d route=%d target=%d transition=%d main=%u "
        "sub=%u editing=%d edit=%u ready=%d screen=%d volume=%u "
        "brightness=%u timeout=%u size=%dx%d\n",
        o->sequence, o->lifecycle, o->route, o->target_route, o->transition,
        o->main_selection, o->sub_selection, o->editing ? 1 : 0, o->edit_index,
        o->input_ready ? 1 : 0, o->screen_on ? 1 : 0, o->volume_percent,
        o->brightness_percent, o->screen_timeout_index, o->width, o->height);
  /* One numbered line each time Main settles on a new route, so a browser
   * test can wait for exact steps; t is milliseconds since Main started. */
  if (!H2_G99_WEB_E2E && o->input_ready && o->route != s_web.settled_route) {
    s_web.settled_route = o->route;
    printf("H2_G99_WEB arrival=%u route=%s main=%u sub=%u t=%.0f\n",
           ++s_web.arrivals, route_name(o->route), o->main_selection,
           o->sub_selection, emscripten_get_now() - s_web.started_at);
  }
  char label[64];
  (void)snprintf(label, sizeof(label), "GizPocket · %s", route_name(o->route));
  const char* text = label;
  const uint32_t sequence = o->sequence;
  const int values[] = {o->lifecycle,
                        o->route,
                        o->target_route,
                        o->transition,
                        o->main_selection,
                        o->sub_selection,
                        o->editing ? 1 : 0,
                        o->edit_index,
                        o->input_ready ? 1 : 0,
                        o->screen_on ? 1 : 0,
                        o->volume_percent,
                        o->brightness_percent,
                        o->screen_timeout_index,
                        (int)o->width,
                        (int)o->height};
  (void)h2_web_main_call(
      page_state,
      (const void*[]){&sequence, &values[0], &values[1], &values[2], &values[3],
                      &values[4], &values[5], &values[6], &values[7],
                      &values[8], &values[9], &values[10], &values[11],
                      &values[12], &values[13], &values[14], &text});
}

/* ---------- Runtime ---------- */

static h2_runtime_config_t runtime_config(void) {
  h2_web_platform_t* platform = s_web.platform;
  h2_runtime_config_t config = {
      .board = H2_G99_WEB_NAME,
      .target = "webassembly",
      .chip = "wasm32",
      .firmware_info = h2_pal_unsupported_firmware_info_api(),
      .mem = h2_web_platform_mem_api(),
      .log = h2_web_platform_log_api(),
      .time = h2_web_platform_time_api(platform),
      .timer = h2_web_platform_timer_api(platform),
      .task = h2_web_platform_task_api(platform),
      .queue = h2_web_platform_queue_api(platform),
      .sync = h2_web_platform_sync_api(platform),
      /* Main keeps nothing on a filesystem; settings live in pref, which the
       * Web PAL persists in the page origin's localStorage. */
      .fs = h2_pal_unsupported_fs_api(),
      .disk = h2_pal_unsupported_disk_api(),
      .pref = h2_web_platform_pref_api(platform),
      .crypto = h2_web_platform_crypto_api(platform),
      .http = h2_pal_unsupported_http_api(),
      .net = h2_pal_unsupported_net_api(),
      .netif = h2_web_platform_netif_api(platform),
      .mqtt = h2_pal_unsupported_mqtt_api(),
      .webrtc = h2_pal_unsupported_webrtc_api(),
      .wifi_sta = h2_pal_unsupported_wifi_sta_api(),
      .wifi_ap = h2_pal_unsupported_wifi_ap_api(),
      .wifi_csi = h2_pal_unsupported_wifi_csi_api(),
      .wifi_settings = h2_pal_unsupported_wifi_settings_api(),
      .ble_host = h2_pal_unsupported_ble_host_api(),
      .modem = h2_pal_unsupported_modem_api(),
      .power = h2_pal_unsupported_power_api(),
      .display = h2_web_platform_display_api(platform),
      .audio = h2_web_platform_audio_api(platform),
      .audio_decoder = h2_pal_unsupported_audio_decoder_api(),
      .periph = h2_g99_web_periph_api(),
      .button = h2_pal_unsupported_button_api(),
      .touch = h2_web_platform_touch_api(platform),
      .buzzer = h2_pal_unsupported_buzzer_api(),
      .nfc = h2_pal_unsupported_nfc_api(),
      .nfc_card_emulation = h2_pal_unsupported_nfc_card_emulation_api(),
      .imu = h2_pal_unsupported_imu_api(),
      .gpio_irq = h2_pal_unsupported_gpio_irq_api(),
      .led = h2_pal_unsupported_led_api(),
      .switch_api = h2_pal_unsupported_switch_api(),
      .pwm_switch = h2_pal_unsupported_pwm_switch_api(),
      .input = h2_pal_unsupported_input_api(),
      .system_event = h2_web_platform_system_event_api(platform),
      .video_decoder = h2_pal_unsupported_video_decoder_api(),
      .component_mapper = h2_g99_web_component_mapper(),
      .event_queue_capacity = H2_RUNTIME_DEFAULT_EVENT_QUEUE_CAPACITY,
  };
  /* The browser has no Wi-Fi radio. Only a page that configures the GizOS
   * simulated radio (h2WebConfigureEnvironment before Start) gets one; plain
   * pages report Wi-Fi unsupported. */
  if (h2_web_platform_fake_wifi_sta_api(platform) != NULL) {
    config.wifi_sta = h2_web_platform_fake_wifi_sta_api(platform);
    config.wifi_settings = h2_web_platform_fake_wifi_settings_api(platform);
  }
  return config;
}

#if H2_G99_WEB_E2E
/* Copies the page's `select` query parameter, empty when absent. */
/* clang-format off */
EM_JS(void, page_select,
      (void *context, h2_web_main_result_t *result, h2_web_main_completion_t *completion), {
  h2WebMain(context, result, completion, ["pointer", "i32"], "i32", (out, size) => {
    try {
      const text = new URLSearchParams(location.search).get('select') ?? "";
      if (lengthBytesUTF8(text) + 1 > size) return -1;
      stringToUTF8(text, out, size);
      return 0;
    } catch (_) { return -1; }
  });
});
/* clang-format on */

/* The suite starts and stops Main itself; the launcher keeps the display
 * open across those restarts. */
static h2_pal_result_t run_suite(void) {
  static char select[H2_G99_E2E_SELECT_MAX + 1u];
  char* out = select;
  const int size = (int)sizeof(select);
  if (h2_web_main_call(page_select, (const void*[]){&out, &size}).i32 != 0) {
    printf("H2_E2E_SELECT unknown=\"<too long>\"\n");
    return H2_PAL_ERR_INVALID_ARG;
  }
  h2_pal_result_t rc = h2_pal_display_open(s_web.runtime->display);
  if (rc != H2_PAL_OK) return rc;
  /* The browser has no radio for the test network, even when the page
   * configures the simulated one, so Wi-Fi cases report UNSUPPORTED. */
  const h2_g99_e2e_target_t target = {
      .runtime = s_web.runtime,
      .board = H2_G99_WEB_NAME,
      .target = "web",
      .display_width = H2_WEB_BOARD_WIDTH,
      .display_height = H2_WEB_BOARD_HEIGHT,
      .capabilities = 0u,
      .select = select,
      .observe = observe,
      .should_abort = should_stop,
  };
  h2_g99_e2e_summary_t summary;
  rc = h2_g99_e2e_run(&target, &summary);
  const h2_pal_result_t closed = h2_pal_display_close(s_web.runtime->display);
  if (rc == H2_PAL_OK && h2_g99_e2e_exit_code(&summary) != 0)
    rc = H2_PAL_ERR_INVALID_STATE;
  return rc != H2_PAL_OK ? rc : closed;
}
#endif

static h2_pal_result_t run_main(void) {
#if H2_G99_WEB_E2E
  return run_suite();
#else
  h2_pal_result_t rc = h2_pal_display_open(s_web.runtime->display);
  if (rc != H2_PAL_OK) return rc;
  static const h2_g99_config_t config = {
      .main_version = "web-dev",
      .loader_version = NULL,
      .board_name = H2_G99_WEB_NAME,
      .should_stop = should_stop,
      .observe = observe,
  };
  s_web.started_at = emscripten_get_now();
  s_web.settled_route = H2_G99_ROUTE_BOOT;
  rc = h2_g99_run(s_web.runtime, &config);
  const h2_pal_result_t closed = h2_pal_display_close(s_web.runtime->display);
  return rc != H2_PAL_OK ? rc : closed;
#endif
}

int main(void) {
  /* A failed checked shutdown keeps these owners alive. Re-entry must never
   * overwrite their pointers or reset a stop flag they still borrow. */
  if (s_web.platform != NULL || s_web.runtime != NULL ||
      s_web.input_task != NULL)
    return 2;
  memset(&s_web, 0, sizeof(s_web));
  h2_atomic_bool_store(&s_stop_requested, false, H2_ATOMIC_RELEASE);
  h2_atomic_bool_store(&s_input_end, false, H2_ATOMIC_RELEASE);
  const h2_web_platform_config_t platform_config = {
      .display_width = H2_WEB_BOARD_WIDTH,
      .display_height = H2_WEB_BOARD_HEIGHT,
  };
  s_web.platform = h2_web_platform_create(&platform_config);
  if (s_web.platform == NULL) {
    printf("H2_WEB_APP name=%s result=FAIL rc=%d destroy=0\n", H2_G99_WEB_NAME,
           H2_PAL_ERR_NO_MEMORY);
    return 1;
  }
  const h2_runtime_config_t config = runtime_config();
  h2_pal_result_t rc = h2_runtime_init(&config, &s_web.runtime);
  if (rc == H2_PAL_OK) rc = h2_runtime_input_start(s_web.runtime, NULL);
  /* Test Control owns input during E2E; page keys would race it. */
  if (rc == H2_PAL_OK && !H2_G99_WEB_E2E) rc = bind_buttons();
  if (rc == H2_PAL_OK) {
    const h2_pal_task_options_t input_options = {.name = "g99/web/input",
                                                 .min_stack_size = 16384u};
    rc = h2_pal_task_start(s_web.runtime->task, &input_options, input_worker,
                           NULL, &s_web.input_task);
  }
  if (rc == H2_PAL_OK) {
    printf("H2_WEB_APP name=%s stage=running wifi=%s\n", H2_G99_WEB_NAME,
           h2_web_platform_fake_wifi_sta_api(s_web.platform) != NULL
               ? "simulated"
               : "unsupported");
    rc = run_main();
  }

  /* Stop DOM admission and join the sole Runtime edge producer before
   * destroying any provider it borrows. */
  h2_pal_result_t cleanup = H2_PAL_OK;
  if (s_web.buttons_bound) cleanup = page_action(2, 0, NULL, NULL);
  h2_atomic_bool_store(&s_input_end, true, H2_ATOMIC_RELEASE);
  if (s_web.input_task != NULL) {
    const h2_pal_result_t joined =
        h2_pal_task_join(s_web.runtime->task, s_web.input_task);
    if (joined != H2_PAL_OK) {
      cleanup = joined;
      goto retained;
    }
    s_web.input_task = NULL;
  }
  if (cleanup != H2_PAL_OK) goto retained;
  if (rc == H2_PAL_OK) rc = s_web.input_result;
  cleanup = h2_runtime_destroy(&s_web.runtime);
  if (s_web.runtime != NULL) goto retained;
  if (rc == H2_PAL_OK) rc = cleanup;
  const h2_pal_result_t destroyed = h2_web_platform_destroy(s_web.platform);
  if (destroyed != H2_PAL_OK) {
    cleanup = destroyed;
    goto retained;
  }
  s_web.platform = NULL;
  printf("H2_WEB_APP name=%s result=%s rc=%d destroy=%d\n", H2_G99_WEB_NAME,
         rc == H2_PAL_OK ? "PASS" : "FAIL", rc, destroyed);
  return rc == H2_PAL_OK ? 0 : 1;

retained:
  printf("H2_WEB_APP name=%s result=FAIL rc=%d retained=1\n", H2_G99_WEB_NAME,
         cleanup);
  return 2;
}
