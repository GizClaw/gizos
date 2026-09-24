#include "asm/includes.h"

#include "h2_jieli_ac791n_devkit.h"

#include "h2/pal/h2_pal_unsupported.h"
#include "h2_jieli_wl82_platform_core.h"
#include "h2_runtime.h"

#include <string.h>

static h2_pal_fs_api_t runtime_fs;
static h2_runtime_config_t runtime_config;
static int runtime_ready;

h2_pal_result_t h2_jieli_ac791n_devkit_runtime_config(
    h2_runtime_config_t *out_config) {
  if (out_config == NULL) return H2_PAL_ERR_INVALID_ARG;
  if (runtime_ready) {
    *out_config = runtime_config;
    return H2_PAL_OK;
  }
  memset(&runtime_fs, 0, sizeof(runtime_fs));
  h2_pal_result_t result = h2_jieli_ac791n_devkit_sd_fs_init(&runtime_fs);
  if (result != H2_PAL_OK) return result;

  runtime_config = (h2_runtime_config_t){
      .board = "jieli_ac791n_devkit",
      .target = "wl82",
      .chip = "ac791n",
      .firmware_info = h2_jieli_wl82_platform_firmware_info_api(),
      .mem = h2_jieli_wl82_platform_mem_api(),
      .log = h2_jieli_wl82_platform_log_api(),
      .time = h2_jieli_wl82_platform_time_api(),
      .timer = h2_jieli_wl82_platform_timer_api(),
      .task = h2_jieli_wl82_platform_task_api(),
      .queue = h2_jieli_wl82_platform_queue_api(),
      .sync = h2_jieli_wl82_platform_sync_api(),
      .fs = &runtime_fs,
      .disk = h2_jieli_ac791n_devkit_disk_api(),
      .pref = h2_jieli_ac791n_devkit_pref_api(),
      .crypto = h2_pal_unsupported_crypto_api(),
      .http = h2_pal_unsupported_http_api(),
#ifdef H2_JIELI_NETWORK_ENABLE
      .net = h2_jieli_ac791n_devkit_net_api(),
#else
      .net = h2_pal_unsupported_net_api(),
#endif
#ifdef H2_JIELI_NETWORK_ENABLE
      .netif = h2_jieli_ac791n_devkit_netif_api(),
#else
      .netif = h2_pal_unsupported_netif_api(),
#endif
      .mqtt = h2_pal_unsupported_mqtt_api(),
      .webrtc = h2_pal_unsupported_webrtc_api(),
#ifdef H2_JIELI_NETWORK_ENABLE
      .wifi_sta = h2_jieli_ac791n_devkit_wifi_sta_api(),
      .wifi_ap = h2_jieli_ac791n_devkit_wifi_ap_api(),
#else
      .wifi_sta = h2_pal_unsupported_wifi_sta_api(),
      .wifi_ap = h2_pal_unsupported_wifi_ap_api(),
#endif
      .wifi_csi = h2_pal_unsupported_wifi_csi_api(),
#ifdef H2_JIELI_NETWORK_ENABLE
      .wifi_settings = h2_jieli_ac791n_devkit_wifi_settings_api(),
#else
      .wifi_settings = h2_pal_unsupported_wifi_settings_api(),
#endif
      .ble_host = h2_jieli_ac791n_devkit_ble_host_api(h2_jieli_wl82_platform_log_api()),
      .modem = h2_pal_unsupported_modem_api(),
      .power = h2_pal_unsupported_power_api(),
      .display = h2_jieli_ac791n_devkit_display_api(),
      .audio = h2_jieli_ac791n_devkit_audio_api(),
      .audio_decoder = h2_pal_unsupported_audio_decoder_api(),
      .periph = h2_pal_unsupported_periph_api(),
      .button = h2_jieli_ac791n_devkit_button_api(),
      .touch = h2_jieli_ac791n_devkit_touch_api(),
      .buzzer = h2_pal_unsupported_buzzer_api(),
      .nfc = h2_pal_unsupported_nfc_api(),
      .nfc_card_emulation = h2_pal_unsupported_nfc_card_emulation_api(),
      .imu = h2_pal_unsupported_imu_api(),
      .gpio_irq = h2_pal_unsupported_gpio_irq_api(),
      .led = h2_pal_unsupported_led_api(),
      .switch_api = h2_pal_unsupported_switch_api(),
      .pwm_switch = h2_pal_unsupported_pwm_switch_api(),
      .input = h2_pal_unsupported_input_api(),
      .system_event = h2_jieli_wl82_platform_system_event_api(),
      .video_decoder = h2_pal_unsupported_video_decoder_api(),
  };
  runtime_ready = 1;
  *out_config = runtime_config;
  return H2_PAL_OK;
}

h2_pal_result_t h2_jieli_ac791n_devkit_runtime_deinit(void) {
  if (!runtime_ready) return H2_PAL_OK;
  h2_pal_result_t result = h2_jieli_ac791n_devkit_sd_fs_deinit();
  if (result == H2_PAL_OK) {
    memset(&runtime_config, 0, sizeof(runtime_config));
    memset(&runtime_fs, 0, sizeof(runtime_fs));
    runtime_ready = 0;
  }
  return result;
}
