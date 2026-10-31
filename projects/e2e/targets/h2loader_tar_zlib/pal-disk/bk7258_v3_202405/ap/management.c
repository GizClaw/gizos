#include "launcher.h"

#include "h2_bk7258_board.h"
#include "h2_bk7258_board_private.h"
#include "h2_bk_h2loader.h"
#include "h2_bk_platform_core.h"
#include "h2/pal/h2_pal_unsupported.h"
#include "h2_runtime.h"

static h2_pal_fs_api_t management_fs;
static h2_runtime_t* runtime;

int h2_bk_pal_disk_management_start(void) {
  int rc = h2_bk7258_board_fs_init(&management_fs);
  if (rc) return rc;
  /* Assemble only management providers. Do not invoke the board's product
   * constructor, which may configure display, audio, NFC or modem pins. */
  const h2_runtime_config_t config = {
      .board = H2_BK7258_BOARD_NAME,
      .target = "bk7258",
      .chip = "bk7258",
      .firmware_info = h2_bk_platform_firmware_info_api(),
      .mem = h2_bk_platform_psram_allocator(),
      .log = h2_bk_platform_log_api(),
      .time = h2_bk_platform_time_api(),
      .timer = h2_bk_platform_timer_api(),
      .task = h2_bk_platform_task_api(),
      .queue = h2_bk_platform_queue_api(),
      .sync = h2_bk_platform_sync_api(),
      .fs = &management_fs,
      .pref = h2_bk_platform_pref_api(),
      .power = h2_bk_h2loader_power_api(),
      .disk = h2_pal_unsupported_disk_api(),
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
      .display = h2_pal_unsupported_display_api(),
      .audio = h2_pal_unsupported_audio_api(),
      .audio_decoder = h2_pal_unsupported_audio_decoder_api(),
      .video_decoder = h2_pal_unsupported_video_decoder_api(),
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
  };
  rc = h2_runtime_init(&config, &runtime);
  if (!rc)
    rc = h2_bk_h2loader_start_app_iostreamikcp_with_capabilities(
        runtime, "pal-disk", H2_LOADER_CAPABILITY_UART);
  /* Installation health is distinct from the subsequent diagnostic verdict. */
  if (!rc) rc = h2_bk_h2loader_confirm_current_app(runtime);
  return rc;
}
