#ifndef H2_SMOKE_HOST_RUNTIME_H
#define H2_SMOKE_HOST_RUNTIME_H

#include "h2/pal/h2_pal_unsupported.h"
#include "h2_runtime.h"

static inline h2_runtime_config_t h2_smoke_host_runtime_config(
    const char *board, const char *target, const char *chip,
    const h2_pal_mem_api_t *mem, const h2_pal_time_api_t *time,
    const h2_pal_queue_api_t *queue, const h2_pal_display_api_t *display) {
  /* Host smoke launchers intentionally assemble only their implemented PAL
   * subset. Every other capability is bound to the canonical unsupported API
   * and must not be interpreted as complete platform support. */
  h2_runtime_config_t config = {0};
  config.board = board;
  config.target = target;
  config.chip = chip;
  config.firmware_info = h2_pal_unsupported_firmware_info_api();
  config.mem = mem;
  config.log = h2_pal_unsupported_log_api();
  config.time = time;
  config.timer = h2_pal_unsupported_timer_api();
  config.task = h2_pal_unsupported_task_api();
  config.queue = queue;
  config.sync = h2_pal_unsupported_sync_api();
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
  config.display = display;
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

#endif
