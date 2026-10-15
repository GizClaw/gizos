#include "mobile_runner.h"
#include "h2_web_platform.h"
#include "h2/pal/h2_pal_unsupported.h"
#include <emscripten/threading.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
  if (argc != 5 || emscripten_is_main_runtime_thread() ||
      (!h2_gizclaw_e2e_fixture_key()[0] ||
       !h2_gizclaw_e2e_fixture_profile()[0] || !h2_gizclaw_e2e_fixture_value()[0])) return 2;
  FILE *f = fopen("/voice.pcm", "rb");
  if (!f) return 2;
  if (fseek(f,0,SEEK_END)) { fclose(f); return 2; }
  long size = ftell(f);
  if (size <= 0 || size > 1024*1024 || fseek(f,0,SEEK_SET)) { fclose(f); return 2; }
  uint8_t *pcm = malloc((size_t)size);
  if (!pcm) { fclose(f); return 2; }
  if (fread(pcm,1,(size_t)size,f) != (size_t)size) { fclose(f); free(pcm); return 2; }
  fclose(f);
  h2_web_platform_config_t pc = {.display_width=1, .display_height=1};
  h2_web_platform_t *platform = h2_web_platform_create(&pc);
  if (!platform) { free(pcm); return 2; }
  h2_runtime_config_t config = {
      .board="web", .target="wasm", .chip="wasm32",
      .mem=h2_web_platform_mem_api(), .time=h2_web_platform_time_api(platform),
      .log=h2_web_platform_log_api(), .timer=h2_web_platform_timer_api(platform),
      .task=h2_web_platform_task_api(platform), .queue=h2_web_platform_queue_api(platform),
      .sync=h2_web_platform_sync_api(platform), .crypto=h2_web_platform_crypto_api(platform),
      .http=h2_web_platform_http_api(platform), .webrtc=h2_web_platform_webrtc_api(platform)};
  config.fs = h2_pal_unsupported_fs_api();
  config.disk = h2_pal_unsupported_disk_api();
  config.pref = h2_pal_unsupported_pref_api();
  config.net = h2_pal_unsupported_net_api();
  config.netif = h2_pal_unsupported_netif_api();
  config.mqtt = h2_pal_unsupported_mqtt_api();
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
  config.video_decoder = h2_pal_unsupported_video_decoder_api();
  config.firmware_info = h2_pal_unsupported_firmware_info_api();
  config.system_event = h2_web_platform_system_event_api(platform);
  config.event_queue_capacity = H2_RUNTIME_DEFAULT_EVENT_QUEUE_CAPACITY;
  h2_gizclaw_e2e_result_t result = {0};
  int rc = h2_gizclaw_mobile_run(config,"wasm-chromium",argv[1],argv[2],argv[3],argv[4],pcm,(size_t)size,&result);
  free(pcm);
  int teardown = result.retained_resources ? H2_PAL_ERR_INVALID_STATE : h2_web_platform_destroy(platform);
  printf("H2_GIZCLAW_PLATFORM_FINAL rc=%d teardown=%d retained=%zu worker=%d\n",
         rc,teardown,result.retained_resources,!emscripten_is_main_runtime_thread());
  return rc || teardown || result.retained_resources ? 1 : 0;
}
