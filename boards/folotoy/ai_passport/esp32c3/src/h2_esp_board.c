#include "h2_esp_board_private.h"
#include "h2_pal_unsupported.h"
#include "h2_corehttp.h"
#include "esp_partition.h"

#include <string.h>

static h2_pal_fs_api_t filesystem;
static h2_corehttp_t* http;
static h2_pal_http_api_t http_api;
static h2_runtime_config_t runtime_config;
static int fs_ready, ready;

static int init_files(void) {
  const h2_esp_platform_littlefs_config_t config = {
      .base_path = "/passport",
      .partition_label = "data",
      .format_if_mount_failed = false};
  int rc = h2_esp_platform_littlefs_fs_init(&filesystem, &config);
  if (rc != 0) {
    /* Only an entirely erased partition is eligible for first-use format. */
    const esp_partition_t* partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "data");
    if (partition == NULL) return H2_PAL_ERR_NOT_FOUND;
    uint8_t bytes[256];
    for (size_t at = 0; at < partition->size; at += sizeof(bytes)) {
      size_t size = partition->size - at;
      if (size > sizeof(bytes)) size = sizeof(bytes);
      if (esp_partition_read(partition, at, bytes, size) != ESP_OK)
        return H2_PAL_ERR_IO;
      for (size_t i = 0; i < size; ++i)
        if (bytes[i] != 0xff) return rc;
    }
    rc = h2_esp_platform_littlefs_format("data");
    if (rc == 0) rc = h2_esp_platform_littlefs_fs_init(&filesystem, &config);
    if (rc != 0) return rc;
  }
  fs_ready = 1;
  rc = h2_pal_fs_mkdir(&filesystem, "/data");
  if (rc == 0) rc = h2_pal_fs_mkdir(&filesystem, "/dl");
  return rc;
}

int h2_esp_board_fs_mount(const char* path) {
  if (path == NULL) return H2_PAL_ERR_INVALID_ARG;
  if (strcmp(path, "/data") != 0 && strcmp(path, "/dl") != 0)
    return H2_PAL_ERR_UNSUPPORTED;
  return fs_ready ? h2_pal_fs_mkdir(&filesystem, path)
                  : H2_PAL_ERR_INVALID_STATE;
}

h2_pal_result_t h2_esp_board_runtime_config(h2_runtime_config_t* out) {
  if (out == NULL) return H2_PAL_ERR_INVALID_ARG;
  memset(out, 0, sizeof(*out));
  if (ready) {
    *out = runtime_config;
    return 0;
  }
  int rc = h2_passport_peripherals_init();
  if (rc == 0) rc = init_files();
  const h2_corehttp_config_t config = {
      .allocator = h2_esp_platform_internal_allocator(),
      .net = h2_esp_platform_net_api(),
      .time = h2_esp_platform_time_api(),
      .log = h2_esp_platform_log_api()};
  if (rc == 0) rc = h2_corehttp_create(&config, &http, &http_api);
  if (rc != 0) {
    (void)h2_esp_board_runtime_deinit();
    return rc;
  }
  runtime_config = (h2_runtime_config_t){
      .board = "ai_passport",
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
      .fs = &filesystem,
      .disk = h2_esp_platform_disk_api(),
      .pref = h2_esp_platform_pref_api(),
      .crypto = h2_esp_platform_crypto_api(),
      .http = &http_api,
      .net = h2_esp_platform_net_api(),
      .netif = h2_esp_platform_netif_api(),
      .mqtt = h2_pal_unsupported_mqtt_api(),
      .webrtc = h2_pal_unsupported_webrtc_api(),
      .wifi_sta = h2_esp_platform_wifi_sta(),
      .wifi_ap = h2_esp_platform_wifi_ap(),
      .wifi_csi = h2_pal_unsupported_wifi_csi_api(),
      .wifi_settings = h2_esp_platform_wifi_settings(),
      .ble_host = h2_esp_platform_ble(),
      .modem = h2_pal_unsupported_modem_api(),
      .power = h2_esp_platform_power_api(),
      .display = h2_passport_display(),
      .audio = h2_passport_audio(),
      .audio_decoder = h2_pal_unsupported_audio_decoder_api(),
      .periph = h2_passport_periph(),
      .button = h2_passport_button(),
      .input = h2_passport_input(),
      .touch = h2_pal_unsupported_touch_api(),
      .buzzer = h2_pal_unsupported_buzzer_api(),
      .nfc = h2_pal_unsupported_nfc_api(),
      .nfc_card_emulation = h2_pal_unsupported_nfc_card_emulation_api(),
      .imu = h2_pal_unsupported_imu_api(),
      .gpio_irq = h2_pal_unsupported_gpio_irq_api(),
      .led = h2_pal_unsupported_led_api(),
      .switch_api = h2_pal_unsupported_switch_api(),
      .pwm_switch = h2_pal_unsupported_pwm_switch_api(),
      .system_event = h2_esp_platform_system_event_api(),
      .video_decoder = h2_pal_unsupported_video_decoder_api(),
  };
  ready = 1;
  *out = runtime_config;
  return 0;
}

h2_pal_result_t h2_esp_board_runtime_deinit(void) {
  int rc = h2_passport_peripherals_deinit();
  if (rc != 0) return rc;
  rc = h2_passport_display_deinit();
  if (rc != 0) return rc;
  h2_corehttp_destroy(http);
  http = NULL;
  if (fs_ready) {
    rc = h2_esp_platform_littlefs_fs_deinit("data");
    if (rc != 0) return rc;
    fs_ready = 0;
  }
  memset(&filesystem, 0, sizeof(filesystem));
  memset(&http_api, 0, sizeof(http_api));
  memset(&runtime_config, 0, sizeof(runtime_config));
  ready = 0;
  return 0;
}
