#include "h2/pal/h2_pal_unsupported.h"
#include "h2_jieli_br35_platform_core.h"
#include "h2_pal_e2e.h"
#include <string.h>

h2_pal_e2e_result_t h2_ac707n_pal_e2e_result;
volatile int h2_ac707n_pal_e2e_status;

static h2_runtime_config_t make_runtime_config(void) {
  h2_runtime_config_t config;
  memset(&config, 0, sizeof(config));
  config.board = "ac707n_chip";
  config.target = "ac707n";
  config.chip = "ac707n";
  config.firmware_info = h2_jieli_br35_platform_firmware_info_api();
  config.mem = h2_jieli_br35_platform_mem_api();
  config.log = h2_jieli_br35_platform_log_api();
  config.time = h2_jieli_br35_platform_time_api();
  config.timer = h2_jieli_br35_platform_timer_api();
  config.task = h2_jieli_br35_platform_task_api();
  config.queue = h2_jieli_br35_platform_queue_api();
  config.sync = h2_jieli_br35_platform_sync_api();
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
  config.display = h2_pal_unsupported_display_api();
  config.audio = h2_pal_unsupported_audio_api();
  config.audio_decoder = h2_pal_unsupported_audio_decoder_api();
  config.periph = h2_pal_unsupported_periph_api();
  config.button = h2_pal_unsupported_button_api();
  config.touch = h2_pal_unsupported_touch_api();
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
  config.event_queue_capacity = H2_RUNTIME_DEFAULT_EVENT_QUEUE_CAPACITY;
  return config;
}
void app_main(void) {
  h2_runtime_t *runtime = NULL;
  h2_runtime_config_t config = make_runtime_config();
  h2_ac707n_pal_e2e_status = h2_runtime_init(&config, &runtime);
  if (h2_ac707n_pal_e2e_status == H2_PAL_OK) {
    const h2_pal_e2e_config_t suite = {.suite_mask = H2_PAL_E2E_SUITE_CORE};
    h2_ac707n_pal_e2e_status =
        h2_pal_e2e_run(runtime, &suite, &h2_ac707n_pal_e2e_result);
  }
  for (;;)
    h2_pal_time_sleep_ms(config.time, 1000);
}
