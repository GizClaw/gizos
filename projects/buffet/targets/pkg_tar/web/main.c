/* Web PAL owns the Canvas display and pointer Touch on the browser thread.
 * Key transitions cross to the App's pthread through atomic masks. */
#include "h2_buffet.h"
#include "h2_pal_unsupported.h"
#include "h2_web_platform.h"

#include <emscripten.h>
#include <emscripten/threading.h>
#include <stdatomic.h>
#include <stdio.h>

static atomic_uint held, edges;
static atomic_bool hidden, fine_pointer, reduced_motion;
EMSCRIPTEN_KEEPALIVE void bf_web_desktop(int value, int reduced) {
  atomic_store(&reduced_motion, reduced != 0);
  atomic_store(&fine_pointer, value != 0);
}
EMSCRIPTEN_KEEPALIVE void bf_web_key(unsigned bit, int down) {
  if (down) {
    unsigned old = atomic_fetch_or(&held, bit);
    if (!(old & bit)) atomic_fetch_or(&edges, bit);
  } else
    atomic_fetch_and(&held, ~bit);
}
EMSCRIPTEN_KEEPALIVE void bf_web_hidden(int value) {
  atomic_store(&held, 0);
  atomic_store(&edges, 0);
  atomic_store(&hidden, value != 0);
}
static void poll(void* user, h2_buffet_input_t* input) {
  (void)user;
  unsigned keys = atomic_load(&held), pressed = atomic_exchange(&edges, 0);
  input->dx = ((keys & 2) != 0) - ((keys & 1) != 0);
  input->dy = ((keys & 8) != 0) - ((keys & 4) != 0);
  if (pressed & 1) input->actions |= H2_BUFFET_PREVIOUS;
  if (pressed & 2) input->actions |= H2_BUFFET_NEXT;
  if (pressed & 16) input->actions |= H2_BUFFET_CONFIRM;
  if (pressed & 32) input->actions |= H2_BUFFET_GOAL;
  if (pressed & 64) input->actions |= H2_BUFFET_LANGUAGE;
  if (pressed & 128) input->actions |= H2_BUFFET_ALBUM;
  if (pressed & 256) input->actions |= H2_BUFFET_SOUND;
  input->inactive = atomic_load(&hidden);
}

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
  config.audio = h2_web_platform_audio_api(platform);
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
  const h2_web_platform_config_t surface = {
      .display_width = H2_BUFFET_WIDTH * H2_BUFFET_DENSITY,
      .display_height = H2_BUFFET_HEIGHT * H2_BUFFET_DENSITY};
  h2_web_platform_t* platform = h2_web_platform_create(&surface);
  if (platform == NULL) {
    printf("H2_BUFFET_WEB_FAIL stage=platform\n");
    return 3;
  }
  const h2_runtime_config_t config = runtime_config(platform);
  h2_runtime_t* runtime = NULL;
  int rc = h2_runtime_init(&config, &runtime);
  const h2_buffet_config_t game = {
      .poll = poll,
      .reduced_motion = atomic_load(&reduced_motion),
      .keyboard = atomic_load(&fine_pointer)};
  if (rc == H2_PAL_OK) rc = h2_buffet_run(runtime, &game);
  printf("H2_BUFFET_WEB_EXIT rc=%d\n", rc);
  const int destroyed = runtime != NULL ? h2_runtime_destroy(&runtime) : 0;
  const int teardown =
      destroyed == H2_PAL_OK ? h2_web_platform_destroy(platform) : H2_PAL_OK;
  return rc || destroyed || teardown ? 1 : 0;
}
