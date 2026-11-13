/* Browser entry: Web PAL with a Canvas display and pointer Touch. Emscripten
 * proxies main() to a pthread, so the game loop may block between frames. */
#include "h2_fruit_slice.h"
#include "h2_pal_unsupported.h"
#include "h2_web_platform.h"

#include <emscripten/threading.h>
#include <stdio.h>

#define WIDTH 480
#define HEIGHT 480

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

int main(void) {
  if (emscripten_is_main_runtime_thread()) return 2;
  const h2_web_platform_config_t surface = {.display_width = WIDTH,
                                            .display_height = HEIGHT};
  h2_web_platform_t* platform = h2_web_platform_create(&surface);
  if (platform == NULL) {
    printf("H2_FRUIT_SLICE_WEB_FAIL stage=platform\n");
    return 3;
  }
  const h2_runtime_config_t config = runtime_config(platform);
  h2_runtime_t* runtime = NULL;
  int rc = h2_runtime_init(&config, &runtime);
  if (rc == H2_PAL_OK) rc = h2_fruit_slice_run(runtime, NULL);
  printf("H2_FRUIT_SLICE_WEB_EXIT rc=%d\n", rc);
  const int destroyed = runtime != NULL ? h2_runtime_destroy(&runtime) : 0;
  const int teardown =
      destroyed == H2_PAL_OK ? h2_web_platform_destroy(platform) : H2_PAL_OK;
  return rc || destroyed || teardown ? 1 : 0;
}
