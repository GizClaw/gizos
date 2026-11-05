#include "h2_chip_board.h"
#include "h2/pal/h2_pal_unsupported.h"
#include "h2_esp_platform_core.h"

const char* h2_chip_board_name(void) { return "esp32c3_chip"; }

h2_pal_result_t h2_chip_board_runtime_config(h2_runtime_config_t* out_config) {
  if (out_config == NULL) return H2_PAL_ERR_INVALID_ARG;
  *out_config = (h2_runtime_config_t){
      .board = "esp32c3_chip",
      .target = "esp32c3",
      .chip = "esp32c3",
      .firmware_info = h2_esp_platform_firmware_info_api(),
      .mem = h2_esp_platform_internal_allocator(),
      .log = h2_esp_platform_log_api(),
      .time = h2_esp_platform_time_api(),
      .timer = h2_esp_platform_timer_api(),
      .task = h2_esp_platform_task_api(),
      .queue = h2_esp_platform_queue_api(),
      .sync = h2_esp_platform_sync_api(),
      .fs = h2_pal_unsupported_fs_api(),
      .disk = h2_pal_unsupported_disk_api(),
      .pref = h2_pal_unsupported_pref_api(),
      .crypto = h2_pal_unsupported_crypto_api(),
      .http = h2_pal_unsupported_http_api(),
      .net = h2_pal_unsupported_net_api(),
      .netif = h2_pal_unsupported_netif_api(),
      .mqtt = h2_pal_unsupported_mqtt_api(),
      .webrtc = h2_pal_unsupported_webrtc_api(),
      .wifi_sta = h2_pal_unsupported_wifi_sta_api(),
      .wifi_ap = h2_pal_unsupported_wifi_ap_api(),
      .wifi_csi = h2_pal_unsupported_wifi_csi_api(),
      .wifi_settings = h2_pal_unsupported_wifi_settings_api(),
      .ble_host = h2_pal_unsupported_ble_host_api(),
      .modem = h2_pal_unsupported_modem_api(),
      .power = h2_pal_unsupported_power_api(),
      .display = h2_pal_unsupported_display_api(),
      .audio = h2_pal_unsupported_audio_api(),
      .audio_decoder = h2_pal_unsupported_audio_decoder_api(),
      .periph = h2_pal_unsupported_periph_api(),
      .button = h2_pal_unsupported_button_api(),
      .touch = h2_pal_unsupported_touch_api(),
      .buzzer = h2_pal_unsupported_buzzer_api(),
      .nfc = h2_pal_unsupported_nfc_api(),
      .nfc_card_emulation = h2_pal_unsupported_nfc_card_emulation_api(),
      .imu = h2_pal_unsupported_imu_api(),
      .gpio_irq = h2_pal_unsupported_gpio_irq_api(),
      .led = h2_pal_unsupported_led_api(),
      .switch_api = h2_pal_unsupported_switch_api(),
      .pwm_switch = h2_pal_unsupported_pwm_switch_api(),
      .input = h2_pal_unsupported_input_api(),
      .system_event = h2_pal_unsupported_system_event_api(),
      .video_decoder = h2_pal_unsupported_video_decoder_api(),
  };
  return H2_PAL_OK;
}
