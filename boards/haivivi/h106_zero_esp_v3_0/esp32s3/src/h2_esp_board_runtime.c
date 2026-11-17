#include "h2_esp_board.h"
#include "h2_esp_board_config.h"
#include "h2_esp_board_private.h"

#include "h2_corehttp.h"

#include "h2_esp_platform_core.h"
#include "h2_esp_platform_image.h"

#include "esp_mac.h"

#include <stdio.h>
#include <string.h>

static h2_pal_fs_api_t s_runtime_fs;
static h2_pal_fs_vtable_t s_runtime_fs_vtable;
static int (*s_runtime_fs_provider_clear)(void* user, const char* path);
static h2_pal_result_t (*s_runtime_fs_provider_prepare)(void* user,
                                                        const char* path);
static h2_runtime_config_t s_runtime_config;
static h2_corehttp_t* s_runtime_http;
static h2_pal_http_api_t s_runtime_http_api;
static int s_runtime_config_ready;
static int s_runtime_audio_pending;
static int s_runtime_fs_pending;
static char s_ble_local_name[17];

static const char* runtime_ble_local_name(void) {
  uint8_t mac[6];
  static const uint8_t no_mac[6] = {0};
  /* Read the stable factory address without starting Wi-Fi. */
  if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK || (mac[0] & 1u) != 0u ||
      memcmp(mac, no_mac, sizeof(mac)) == 0) {
    (void)h2_pal_log_write(h2_esp_board_log_api(), H2_PAL_LOG_WARN,
                           "h106-zero/ble", "device MAC unavailable");
    return NULL;
  }
  const int length = snprintf(s_ble_local_name, sizeof(s_ble_local_name),
                              "h106-zero-%02X%02X%02X", (unsigned)mac[3],
                              (unsigned)mac[4], (unsigned)mac[5]);
  return length == 16 ? s_ble_local_name : NULL;
}

static int runtime_fs_clear(void* user, const char* path) {
  if (path == NULL) {
    return H2_PAL_FS_ERR_INVALID_ARG;
  }
  if (strcmp(path, "/data") == 0) {
    return h2_esp_board_fs_clear(path);
  }
  if (strncmp(path, "/data/", 6u) != 0 || path[6] == '\0') {
    return H2_PAL_FS_ERR_UNSUPPORTED;
  }
  /* Keep provider path validation and its flash-safe directory traversal. */
  return s_runtime_fs_provider_clear != NULL
             ? s_runtime_fs_provider_clear(user, path)
             : H2_PAL_FS_ERR_UNSUPPORTED;
}

static h2_pal_result_t runtime_fs_prepare(void* user, const char* path) {
  if (path == NULL || path[0] == '\0') return H2_PAL_FS_ERR_INVALID_ARG;
  if (s_runtime_fs_provider_prepare != NULL)
    return s_runtime_fs_provider_prepare(user, path);
  if (strcmp(path, "/dl") == 0 || strncmp(path, "/dl/", 4u) == 0)
    return h2_esp_board_fs_mount("/dl");
  if (strcmp(path, "/data") == 0 || strncmp(path, "/data/", 6u) == 0)
    return h2_esp_board_fs_mount("/data");
  return H2_PAL_FS_ERR_UNSUPPORTED;
}

h2_pal_result_t h2_esp_board_task_configure(
    const h2_esp_task_policy_config_t* config) {
  if (config == NULL || config->resolver == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  h2_esp_board_arena_init();
  h2_esp_task_policy_config_t policy = *config;
  policy.psram_stack_allocator = h2_esp_board_arena_mem();
  return h2_esp_platform_task_configure(&policy);
}

h2_pal_result_t h2_esp_board_runtime_config(h2_runtime_config_t* out_config) {
  if (out_config == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  memset(out_config, 0, sizeof(*out_config));
  if (s_runtime_config_ready) {
    *out_config = s_runtime_config;
    return H2_PAL_OK;
  }
  if (s_runtime_audio_pending || s_runtime_fs_pending) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  h2_esp_board_arena_init();
  s_runtime_fs_pending = 1;
  h2_pal_result_t rc = h2_esp_board_fs_init(&s_runtime_fs);
  if (rc != H2_PAL_OK) {
    if (h2_esp_board_fs_deinit() == H2_PAL_OK) s_runtime_fs_pending = 0;
    return rc;
  }
  if (rc == H2_PAL_OK && s_runtime_fs.vtable != NULL) {
    s_runtime_fs_vtable = *s_runtime_fs.vtable;
    s_runtime_fs_provider_clear = s_runtime_fs_vtable.clear;
    s_runtime_fs_provider_prepare = s_runtime_fs_vtable.prepare;
    s_runtime_fs_vtable.clear = runtime_fs_clear;
    s_runtime_fs_vtable.prepare = runtime_fs_prepare;
    s_runtime_fs.vtable = &s_runtime_fs_vtable;
  }
  const h2_corehttp_config_t http_config = {
      .allocator = h2_esp_board_psram_allocator(),
      .net = h2_esp_platform_net_api(),
      .time = h2_esp_board_time_api(),
      .log = h2_esp_board_log_api(),
  };
  rc = h2_corehttp_create(&http_config, &s_runtime_http, &s_runtime_http_api);
  if (rc != H2_PAL_OK) {
    if (h2_esp_board_fs_deinit() == H2_PAL_OK) s_runtime_fs_pending = 0;
    return rc;
  }
  *out_config = (h2_runtime_config_t){
      .board = H2_ESP_BOARD_NAME,
      .target = "esp32s3",
      .chip = "esp32s3",
      .ble_local_name = runtime_ble_local_name(),
      .firmware_info = h2_esp_platform_firmware_info_api(),
      .device_identity = h2_esp_platform_device_identity_api(),
      .image = h2_esp_platform_image_api(),
      .io_stream = h2_esp_platform_io_stream_api(),
      .mem = h2_esp_board_psram_allocator(),
      .log = h2_esp_board_log_api(),
      .time = h2_esp_board_time_api(),
      .timer = h2_esp_platform_timer_api(),
      .task = h2_esp_board_task_api(),
      .queue = h2_esp_board_queue_api(),
      .sync = h2_esp_board_sync_api(),
      .fs = &s_runtime_fs,
      .disk = h2_esp_board_disk_api(),
      .pref = h2_esp_board_pref_api(),
      .crypto = h2_esp_board_crypto_api(),
      .http = &s_runtime_http_api,
      .net = h2_esp_platform_net_api(),
      .netif = h2_esp_platform_netif_api(),
      .mqtt = h2_pal_unsupported_mqtt_api(),
      .webrtc = h2_esp_board_webrtc_api(),
      .wifi_sta = h2_esp_board_wifi_sta(),
      .wifi_ap = h2_esp_board_wifi_ap(),
      .wifi_csi = h2_esp_platform_wifi_csi(),
      .wifi_settings = h2_esp_board_wifi_settings(),
      .ble_host = h2_esp_board_ble(),
      .modem = h2_esp_board_modem(),
      .power = h2_esp_board_power_api(),
      .display = h2_esp_board_display(),
      .audio = h2_esp_board_audio(),
      .audio_decoder = h2_pal_unsupported_audio_decoder_api(),
      .periph = h2_esp_board_periph_api(),
      .button = h2_esp_board_button_api(),
      .touch = h2_pal_unsupported_touch_api(),
      .buzzer = h2_pal_unsupported_buzzer_api(),
      .nfc = h2_esp_board_nfc_api(),
      .nfc_card_emulation = h2_pal_unsupported_nfc_card_emulation_api(),
      .imu = h2_pal_unsupported_imu_api(),
      .gpio_irq = h2_pal_unsupported_gpio_irq_api(),
      .led = h2_esp_board_led_api(),
      .switch_api = h2_pal_unsupported_switch_api(),
      .pwm_switch = h2_esp_board_pwm_switch_api(),
      .input = h2_esp_board_input_api(),
      .system_event = h2_esp_board_system_event_api(),
      .video_decoder = h2_pal_unsupported_video_decoder_api(),
  };
  s_runtime_config = *out_config;
  s_runtime_config_ready = 1;
  s_runtime_audio_pending = 1;
  return H2_PAL_OK;
}

h2_pal_result_t h2_esp_board_runtime_deinit(void) {
  h2_pal_result_t first_error = H2_PAL_OK;
  if (s_runtime_audio_pending) {
    first_error = h2_esp_board_audio_deinit();
    if (first_error == H2_PAL_OK) s_runtime_audio_pending = 0;
  }
  if (s_runtime_http != NULL) {
    h2_corehttp_destroy(s_runtime_http);
    s_runtime_http = NULL;
    memset(&s_runtime_http_api, 0, sizeof(s_runtime_http_api));
  }
  s_runtime_config_ready = 0;
  if (s_runtime_fs_pending) {
    const h2_pal_result_t fs_rc = h2_esp_board_fs_deinit();
    if (fs_rc == H2_PAL_OK) {
      s_runtime_fs_pending = 0;
      memset(&s_runtime_fs, 0, sizeof(s_runtime_fs));
      s_runtime_fs_provider_clear = NULL;
      s_runtime_fs_provider_prepare = NULL;
    }
    if (first_error == H2_PAL_OK) first_error = fs_rc;
  }
  if (!s_runtime_audio_pending && !s_runtime_fs_pending) {
    memset(&s_runtime_config, 0, sizeof(s_runtime_config));
  }
  return first_error;
}
