/* Desktop preview: SDL3 stays on the main thread. Pref is unsupported here,
 * so the best score lasts for one run. */
#include "h2_desktop_platform.h"
#include "h2_garden_guard.h"
#include "h2_pal_unsupported.h"
#include "h2_sdl3.h"

#include <stdio.h>

typedef struct window {
  h2_sdl3_t* sdl;
  bool closed, back;
} window_t;

static bool should_exit(void* user) {
  window_t* window = user;
  h2_sdl3_event_t event;
  while (h2_sdl3_poll_event(window->sdl, &event) == H2_PAL_OK) {
    if (event.kind == H2_SDL3_EVENT_CLOSE)
      window->closed = true;
    else if (event.kind == H2_SDL3_EVENT_KEY && event.pressed &&
             event.key == H2_SDL3_KEY_ESCAPE)
      window->back = true;
  }
  return window->closed;
}

static bool back_pressed(void* user) {
  window_t* window = user;
  const bool pressed = window->back;
  window->back = false;
  return pressed;
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

int main(int argc, char** argv) {
  int width = 480, height = 320;
  if (argc > 2 ||
      (argc == 2 && sscanf(argv[1], "%dx%d", &width, &height) != 2)) {
    fprintf(stderr, "usage: %s [WIDTHxHEIGHT]\n", argv[0]);
    return 2;
  }
  const h2_sdl3_config_t surface = {
      .title = "Garden Guard", .width = width, .height = height};
  window_t window = {0};
  if (h2_sdl3_create(&surface, &window.sdl) != H2_PAL_OK) {
    fprintf(stderr, "garden-guard: SDL3 window failed\n");
    return 1;
  }
  const h2_runtime_config_t config = runtime_config(window.sdl);
  h2_runtime_t* runtime = NULL;
  int rc = h2_runtime_init(&config, &runtime);
  if (rc == H2_PAL_OK) {
    const h2_garden_guard_config_t game = {.should_exit = should_exit,
                                           .back_pressed = back_pressed,
                                           .exit_on_unhandled_back = true,
                                           .user = &window};
    rc = h2_garden_guard_run(runtime, &game);
  }
  const int destroyed = runtime != NULL ? h2_runtime_destroy(&runtime) : 0;
  if (destroyed == H2_PAL_OK) h2_sdl3_destroy(window.sdl);
  if (rc != H2_PAL_OK || destroyed != H2_PAL_OK) {
    fprintf(stderr, "garden-guard: rc=%d destroy=%d\n", rc, destroyed);
    return 1;
  }
  return 0;
}
