/* Browser entry: Web PAL with a Canvas display and pointer Touch. Emscripten
 * proxies main() to a pthread, so the game loop may block between frames. */
#include "h2_garden_guard.h"
#include "h2_pal_unsupported.h"
#include "h2_web_platform.h"

#include <emscripten/emscripten.h>
#include <emscripten/proxying.h>
#include <emscripten/threading.h>
#include <stdio.h>
#include <stdatomic.h>

#define WIDTH 480
#define HEIGHT 320

static h2_runtime_config_t runtime_config(h2_web_platform_t* platform) {
  h2_runtime_config_t config = {0};
  config.board = "web";
  config.target = "web";
  config.chip = "wasm32";
  config.firmware_info = h2_pal_unsupported_firmware_info_api();
  config.mem = h2_web_platform_mem_api();
  config.log = h2_web_platform_log_api();
  config.time = h2_web_platform_time_api(platform);
  config.timer = h2_web_platform_timer_api(platform);
  config.task = h2_web_platform_task_api(platform);
  config.queue = h2_web_platform_queue_api(platform);
  config.sync = h2_web_platform_sync_api(platform);
  config.fs = h2_pal_unsupported_fs_api();
  config.disk = h2_pal_unsupported_disk_api();
  config.pref = h2_web_platform_pref_api(platform);
  config.crypto = h2_pal_unsupported_crypto_api();
  config.http = h2_pal_unsupported_http_api();
  config.net = h2_pal_unsupported_net_api();
  config.netif = h2_pal_unsupported_netif_api();
  config.mqtt = h2_pal_unsupported_mqtt_api();
  config.webrtc = h2_pal_unsupported_webrtc_api();
  config.wifi_sta = h2_pal_unsupported_wifi_sta_api();
  config.wifi_ap = h2_pal_unsupported_wifi_ap_api();
  config.wifi_csi = h2_pal_unsupported_wifi_csi_api();
  config.wifi_settings = h2_pal_unsupported_wifi_settings_api();
  config.ble_host = h2_pal_unsupported_ble_host_api();
  config.modem = h2_pal_unsupported_modem_api();
  config.power = h2_pal_unsupported_power_api();
  config.camera = h2_pal_unsupported_camera_api();
  config.display = h2_web_platform_display_api(platform);
  config.audio = h2_pal_unsupported_audio_api();
  config.audio_decoder = h2_pal_unsupported_audio_decoder_api();
  config.periph = h2_pal_unsupported_periph_api();
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
  config.system_event = h2_pal_unsupported_system_event_api();
  config.video_decoder = h2_pal_unsupported_video_decoder_api();
  return config;
}

/* clang-format off */
EM_JS(void, apply_language_js, (const char* language_tag), {
  if (Module['h2GardenLanguage'])
    Module['h2GardenLanguage'](UTF8ToString(language_tag));
});
/* clang-format on */

static void apply_language(void* language_tag) {
  apply_language_js(language_tag);
}

static void language_changed(void* user, const char* language_tag) {
  (void)user;
  (void)emscripten_proxy_sync(emscripten_proxy_get_system_queue(),
                              emscripten_main_runtime_thread_id(),
                              apply_language, (void*)language_tag);
}

/* The page delivers input while the game's state stays on the run thread. */
/* clang-format off */
EM_JS(void, apply_cover_js, (const h2_garden_guard_cover_rect_t* controls,
                             int session_language), {
  if (!Module['h2GardenCover']) return;
  const rects = [];
  if (controls) {
    const first = controls >> 2;
    for (let i = 1; i <= 4; ++i) {
      const base = first + i * 4;
      rects.push([HEAPF32[base], HEAPF32[base + 1], HEAPF32[base + 2], HEAPF32[base + 3]]);
    }
  }
  Module['h2GardenCover'](rects, !!session_language);
});
/* clang-format on */

_Static_assert(sizeof(h2_garden_guard_cover_rect_t) == 4u * sizeof(float),
               "Web cover expects four packed coordinates");
_Static_assert(H2_GARDEN_GUARD_COVER_ACTION_COUNT == 5,
               "Update Web cover controls");

typedef struct web_cover {
  const h2_garden_guard_cover_rect_t* controls;
  bool session_language;
} web_cover_t;

static void apply_cover(void* data) {
  const web_cover_t* cover = data;
  apply_cover_js(cover->controls, cover->session_language);
}

static void cover_changed(void* user,
                          const h2_garden_guard_cover_rect_t* controls,
                          bool session_language) {
  (void)user;
  web_cover_t cover = {controls, session_language};
  (void)emscripten_proxy_sync(emscripten_proxy_get_system_queue(),
                              emscripten_main_runtime_thread_id(), apply_cover,
                              &cover);
}

static atomic_int web_cover_action;

EMSCRIPTEN_KEEPALIVE void h2_garden_guard_web_cover_action(int action) {
  int idle = 0;
  if (action > 0 && action < H2_GARDEN_GUARD_COVER_ACTION_COUNT)
    (void)atomic_compare_exchange_strong(&web_cover_action, &idle, action);
}

static h2_garden_guard_cover_action_t take_cover_action(void* user) {
  (void)user;
  return (h2_garden_guard_cover_action_t)atomic_exchange(&web_cover_action, 0);
}

static atomic_bool web_back;
static atomic_bool web_reduce_motion;

EMSCRIPTEN_KEEPALIVE void h2_garden_guard_web_back(void) {
  atomic_store(&web_back, true);
}

EMSCRIPTEN_KEEPALIVE void h2_garden_guard_web_reduce_motion(int enabled) {
  atomic_store(&web_reduce_motion, enabled != 0);
}

static bool back_pressed(void* user) {
  (void)user;
  return atomic_exchange(&web_back, false);
}

static bool reduce_motion(void* user) {
  (void)user;
  return atomic_load(&web_reduce_motion);
}

int main(void) {
  if (emscripten_is_main_runtime_thread()) return 2;
  const h2_web_platform_config_t surface = {.display_width = WIDTH,
                                            .display_height = HEIGHT};
  h2_web_platform_t* platform = h2_web_platform_create(&surface);
  if (platform == NULL) {
    printf("H2_GARDEN_GUARD_WEB_FAIL stage=platform\n");
    return 3;
  }
  const h2_runtime_config_t config = runtime_config(platform);
  h2_runtime_t* runtime = NULL;
  int rc = h2_runtime_init(&config, &runtime);
  const h2_garden_guard_config_t game = {.language_changed = language_changed,
                                         .back_pressed = back_pressed,
                                         .reduce_motion = reduce_motion,
                                         .cover_changed = cover_changed,
                                         .cover_action = take_cover_action};
  if (rc == H2_PAL_OK) rc = h2_garden_guard_run(runtime, &game);
  printf("H2_GARDEN_GUARD_WEB_EXIT rc=%d\n", rc);
  const int destroyed = runtime != NULL ? h2_runtime_destroy(&runtime) : 0;
  const int teardown =
      destroyed == H2_PAL_OK ? h2_web_platform_destroy(platform) : H2_PAL_OK;
  return rc || destroyed || teardown ? 1 : 0;
}
