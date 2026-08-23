#include "h2_desktop_platform.h"
#include "h2_lua_runtime_e2e.h"
#include "h2_pal.h"
#include "h2_runtime.h"

#include <stdio.h>

int main(void) {
  h2_runtime_config_t config = {
      .board = "desktop",
      .target = "host",
      .chip = "host",
      .firmware_info = h2_pal_unsupported_firmware_info_api(),
      .mem = h2_desktop_platform_default_allocator(),
      .log = h2_desktop_platform_log_api(),
      .time = h2_desktop_platform_time_api(),
      .timer = h2_pal_unsupported_timer_api(),
      .task = h2_desktop_platform_task_api(),
      .queue = h2_desktop_platform_queue_api(),
      .sync = h2_desktop_platform_sync_api(),
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
      .periph = h2_lua_runtime_e2e_periph_api(),
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
      .component_mapper = h2_lua_runtime_e2e_component_mapper(),
  };
  h2_runtime_t *runtime = NULL;
  h2_lua_runtime_e2e_report_t report = {0};
  h2_pal_result_t result = h2_runtime_init(&config, &runtime);
  size_t i;
  if (result == H2_PAL_OK) {
    result = h2_lua_runtime_e2e_run(runtime,
                                    &(h2_lua_runtime_e2e_config_t){
                                        .scheduler = "multi-worker",
                                        .worker_count = 2u,
                                    },
                                    &report);
  }
  if (runtime != NULL)
    h2_runtime_deinit(runtime);
  if (result != H2_PAL_OK && report.case_count == 0u) {
    fprintf(stderr, "H2_LUA_E2E result=FAIL rc=%d\n", result);
    return 1;
  }
  for (i = 0u; i < report.case_count; ++i) {
    printf("H2_LUA_E2E_CASE id=%s result=%s rc=%d evidence=%llu\n",
           report.cases[i].id,
           report.cases[i].result == H2_PAL_OK ? "PASS" : "FAIL",
           report.cases[i].result,
           (unsigned long long)report.cases[i].evidence);
  }
  printf("H2_LUA_E2E result=%s scheduler=%s passed=%zu total=%zu\n",
         result == H2_PAL_OK ? "PASS" : "FAIL", report.scheduler, report.passed,
         report.case_count);
  return result == H2_PAL_OK ? 0 : 1;
}
