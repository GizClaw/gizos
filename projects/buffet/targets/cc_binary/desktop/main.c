/* SDL3 owns the window, pointer and keyboard on its main thread. */
#include "h2_desktop_platform.h"
#include "h2_pal_unsupported.h"
#include "h2_sdl3.h"
#include "h2_buffet.h"
#include "buffet_desktop_pref.h"

#include <stdio.h>

typedef struct window {
  h2_sdl3_t* sdl;
  bool inactive;
  bool held[4];
} window_t;

static void poll(void* user, h2_buffet_input_t* input) {
  window_t* window = user;
  h2_sdl3_event_t event;
  while (h2_sdl3_poll_event(window->sdl, &event) == H2_PAL_OK) {
    if (event.kind == H2_SDL3_EVENT_CLOSE) input->close = true;
    if (event.kind == H2_SDL3_EVENT_FOCUS_LOST) {
      window->inactive = true;
      for (int i = 0; i < 4; i++) window->held[i] = false;
    }
    if (event.kind == H2_SDL3_EVENT_FOCUS_GAINED) window->inactive = false;
    if (event.kind != H2_SDL3_EVENT_KEY || event.repeat) continue;
    bool pressed = event.pressed != 0;
    switch (event.key) {
      case H2_SDL3_KEY_LEFT:
        window->held[0] = pressed;
        if (pressed) input->actions |= H2_BUFFET_PREVIOUS;
        break;
      case H2_SDL3_KEY_RIGHT:
        window->held[1] = pressed;
        if (pressed) input->actions |= H2_BUFFET_NEXT;
        break;
      case H2_SDL3_KEY_UP:
        window->held[2] = pressed;
        break;
      case H2_SDL3_KEY_DOWN:
        window->held[3] = pressed;
        break;
      case H2_SDL3_KEY_SPACE:
      case H2_SDL3_KEY_ENTER:
        if (pressed) input->actions |= H2_BUFFET_CONFIRM;
        break;
      case H2_SDL3_KEY_G:
        if (pressed) input->actions |= H2_BUFFET_GOAL;
        break;
      case H2_SDL3_KEY_C:
        if (pressed) input->actions |= H2_BUFFET_ALBUM;
        break;
      case H2_SDL3_KEY_L:
        if (pressed) input->actions |= H2_BUFFET_LANGUAGE;
        break;
      case H2_SDL3_KEY_ESCAPE:
        if (pressed) input->close = true;
        break;
      default:
        break;
    }
  }
  input->dx = window->held[1] - window->held[0];
  input->dy = window->held[3] - window->held[2];
  input->inactive = window->inactive;
}

static h2_runtime_config_t runtime_config(h2_sdl3_t* sdl) {
  h2_runtime_config_t config = {0};
  config.board = "desktop";
  config.target = "desktop";
  config.chip = "host";
  config.firmware_info = h2_pal_unsupported_firmware_info_api();
  config.mem = h2_desktop_platform_default_allocator();
  config.log = h2_desktop_platform_log_api();
  config.time = h2_desktop_platform_time_api();
  config.timer = h2_desktop_platform_timer_api();
  config.task = h2_desktop_platform_task_api();
  config.queue = h2_desktop_platform_queue_api();
  config.sync = h2_desktop_platform_sync_api();
  config.fs = h2_pal_unsupported_fs_api();
  config.disk = h2_pal_unsupported_disk_api();
  config.pref = h2_pal_unsupported_pref_api();
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
  config.display = h2_sdl3_display(sdl);
  config.audio = h2_pal_unsupported_audio_api();
  config.audio_decoder = h2_pal_unsupported_audio_decoder_api();
  config.periph = h2_pal_unsupported_periph_api();
  config.button = h2_pal_unsupported_button_api();
  config.touch = h2_sdl3_touch(sdl);
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

int main(void) {
  const h2_sdl3_config_t surface = {.title = "Tidewheel Sushi",
                                    .width = H2_BUFFET_WIDTH,
                                    .height = H2_BUFFET_HEIGHT};
  window_t window = {0};
  if (h2_sdl3_create_dense(&surface, H2_BUFFET_DENSITY, &window.sdl) !=
      H2_PAL_OK) {
    fprintf(stderr, "buffet: SDL3 window failed\n");
    return 1;
  }
  h2_sqlite_t* storage = NULL;
  h2_runtime_config_t config = runtime_config(window.sdl);
  if (buffet_desktop_pref_create(NULL, &storage) == H2_PAL_OK)
    config.pref = h2_sqlite_pref_api(storage);
  h2_runtime_t* runtime = NULL;
  int rc = h2_runtime_init(&config, &runtime);
  if (rc == H2_PAL_OK) {
    const h2_buffet_config_t game = {
        .poll = poll, .user = &window, .keyboard = true};
    rc = h2_buffet_run(runtime, &game);
  }
  const int destroyed = runtime != NULL ? h2_runtime_destroy(&runtime) : 0;
  if (destroyed == H2_PAL_OK) {
    h2_sdl3_destroy(window.sdl);
    if (storage) h2_sqlite_destroy(storage);
  }
  if (rc != H2_PAL_OK || destroyed != H2_PAL_OK) {
    fprintf(stderr, "buffet: rc=%d destroy=%d\n", rc, destroyed);
    return 1;
  }
  return 0;
}
