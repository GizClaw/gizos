#include "h2_bk7258_board_private.h"

#include "h2_bk_platform_core.h"
#include "h2/pal/h2_pal_unsupported.h"
#include <stdio.h>

static h2_pal_fs_api_t s_fs;

int h2_bk7258_board_runtime_config(h2_runtime_config_t* config) {
  if (config == NULL) return H2_PAL_ERR_INVALID_ARG;
  int rc = h2_bk7258_board_init();
  if (rc != 0) return rc;
  rc = h2_bk7258_board_fs_init(&s_fs);
  if (rc != 0) return rc;
  rc = h2_bk_platform_netif_monitor_init();
  if (rc != 0) return rc;
  int input = h2_h202_input_init();
  int modem = h2_h202_modem_init();
  printf("H202_BSP input=%d modem=%d\n", input, modem);
  *config = (h2_runtime_config_t){
      .board = H2_H202_BOARD_NAME,
      .target = H2_H202_TARGET_NAME,
      .chip = H2_H202_TARGET_NAME,
      .ble_local_name = h2_bk7258_board_ble_local_name(),
      .firmware_info = h2_bk_platform_firmware_info_api(),
      .mem = h2_bk_platform_psram_allocator(),
      .log = h2_bk_platform_log_api(),
      .time = h2_bk_platform_time_api(),
      .timer = h2_bk_platform_timer_api(),
      .task = h2_bk_platform_task_api(),
      .queue = h2_bk_platform_queue_api(),
      .sync = h2_bk_platform_sync_api(),
      .fs = &s_fs,
      .pref = h2_bk_platform_pref_api(),
      .crypto = h2_bk_platform_crypto_api(),
      .net = h2_bk_platform_net_api(),
      .netif = h2_bk_platform_netif_api(),
      .wifi_sta = h2_bk_platform_wifi_sta(),
      .wifi_settings = h2_bk_platform_wifi_settings(),
      .ble_host = h2_bk_platform_ble(),
      .modem =
          modem == 0 ? h2_h202_modem_api() : h2_pal_unsupported_modem_api(),
      .power = h2_h202_power_api(),
      .audio = h2_h202_audio_api(),
      .button = h2_h202_button_api(),
      .nfc = h2_h202_nfc_api() != NULL ? h2_h202_nfc_api()
                                       : h2_pal_unsupported_nfc_api(),
      .led = h2_h202_led_api(),
      .pwm_switch = h2_pal_unsupported_pwm_switch_api(),
      .switch_api = h2_h202_switch_api(),
      .system_event = h2_bk_platform_system_event_api(),
      .disk = h2_pal_unsupported_disk_api(),
      .http = h2_pal_unsupported_http_api(),
      .mqtt = h2_pal_unsupported_mqtt_api(),
      .webrtc = h2_pal_unsupported_webrtc_api(),
      .wifi_ap = h2_bk_platform_wifi_ap(),
      .wifi_csi = h2_bk_platform_wifi_csi(),
      .display = h2_pal_unsupported_display_api(),
      .audio_decoder = h2_pal_unsupported_audio_decoder_api(),
      .periph = h2_pal_unsupported_periph_api(),
      .touch = h2_pal_unsupported_touch_api(),
      .buzzer = h2_pal_unsupported_buzzer_api(),
      .imu = h2_pal_unsupported_imu_api(),
      .gpio_irq = h2_pal_unsupported_gpio_irq_api(),
      .nfc_card_emulation = h2_pal_unsupported_nfc_card_emulation_api(),
      .input = h2_pal_unsupported_input_api(),
      .video_decoder = h2_pal_unsupported_video_decoder_api(),
  };
  return H2_PAL_OK;
}

int h2_bk7258_board_runtime_deinit(void) {
  int rc = h2_h202_modem_deinit();
  if (rc == 0) rc = h2_h202_audio_deinit();
  if (rc == 0) rc = h2_h202_led_deinit();
  if (rc == 0) rc = h2_bk7258_board_fs_deinit();
  return rc;
}
