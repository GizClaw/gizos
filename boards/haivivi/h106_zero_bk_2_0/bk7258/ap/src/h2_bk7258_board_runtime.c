#include "h2_bk7258_board.h"
#include "h2_bk7258_board_private.h"
#include "h2_zero_bk_spi.h"
#include "h2_zero_bk_trust.h"

#include "h2_corehttp.h"
#include "h2_bk_platform_core.h"
#include "h2_bk_platform_image.h"
#include "h2_bk_platform_storage.h"

#include <string.h>

static h2_pal_fs_api_t s_runtime_fs;
static h2_runtime_config_t s_runtime_config;
static h2_corehttp_t* s_runtime_http;
static h2_pal_http_api_t s_runtime_http_api;
static int s_runtime_config_ready;
static h2_runtime_config_t s_recovery_config;
static int s_recovery_config_ready;

static h2_pal_result_t board_native_boot_init(void) {
  static int ready;
  if (ready) return H2_PAL_OK;
  static const h2_bk_disk_region_t dump = {
      .id = 0x7a0000u,
      .offset = 0x7a0000u,
      .size = 360u * 1024u,
      .name = "coredump",
  };
  h2_pal_result_t rc = h2_bk_platform_disk_configure(&dump);
  if (rc == H2_PAL_OK) rc = h2_bk_platform_boot_startup();
  if (rc == H2_PAL_OK) ready = 1;
  return rc;
}

h2_pal_result_t h2_bk7258_board_recovery_runtime_config(
    h2_runtime_config_t* out_config) {
  if (out_config == NULL) return H2_PAL_ERR_INVALID_ARG;
  memset(out_config, 0, sizeof(*out_config));
  if (s_runtime_config_ready || s_runtime_http != NULL)
    return H2_PAL_ERR_INVALID_STATE;
  if (s_recovery_config_ready) {
    *out_config = s_recovery_config;
    return H2_PAL_OK;
  }
  h2_pal_result_t rc = board_native_boot_init();
  if (rc != H2_PAL_OK) return rc;
  if (h2_zero_bk_spi_init() != 0) return H2_PAL_ERR_NO_MEMORY;
  rc = h2_bk7258_board_fs_init(&s_runtime_fs);
  if (rc != H2_PAL_OK) return rc;
  s_recovery_config = (h2_runtime_config_t){
      .board = H2_BK7258_BOARD_NAME,
      .target = H2_BK7258_CHIP_NAME,
      .chip = H2_BK7258_CHIP_NAME,
      .ble_local_name = h2_bk7258_board_ble_local_name(),
      .firmware_info = h2_bk_platform_firmware_info_api(),
      .device_identity = h2_bk_platform_device_identity_api(),
      .image = h2_bk_platform_image_api(),
      .io_stream = h2_bk_platform_io_stream_api(),
      .mem = h2_bk_platform_default_allocator(),
      .mem_external = h2_bk_platform_psram_allocator(),
      .log = h2_bk_platform_log_api(),
      .time = h2_bk_platform_time_api(),
      .task = h2_bk_platform_task_api(),
      .queue = h2_bk_platform_queue_api(),
      .sync = h2_bk_platform_sync_api(),
      .pref = h2_bk_platform_pref_api(),
      .crypto = h2_bk_platform_crypto_api(),
      .ble_host = h2_bk_platform_ble(),
      .system_event = h2_bk_platform_system_event_api(),
      .power = h2_bk7258_board_power_api(),
      .fs = &s_runtime_fs,
      .disk = h2_bk_platform_disk_api(),
  };
  s_recovery_config_ready = 1;
  *out_config = s_recovery_config;
  return H2_PAL_OK;
}

h2_pal_result_t h2_bk7258_board_runtime_config(
    h2_runtime_config_t* out_config) {
  h2_pal_result_t rc;

  if (out_config == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (s_runtime_config_ready) {
    *out_config = s_runtime_config;
    return H2_PAL_OK;
  }
  rc = board_native_boot_init();
  if (rc != H2_PAL_OK) return rc;
  if (!s_recovery_config_ready) {
    if (h2_zero_bk_spi_init() != 0) return H2_PAL_ERR_NO_MEMORY;
    rc = h2_bk7258_board_fs_init(&s_runtime_fs);
    if (rc != H2_PAL_OK) return rc;
  }
  const h2_corehttp_config_t http_config = {
      .root_ca_pem = h2_zero_bk_root_ca_pem,
      .root_ca_pem_len = sizeof(h2_zero_bk_root_ca_pem) - 1u,
      .allocator = h2_bk7258_board_default_allocator(),
      .net = h2_bk_platform_net_api(),
      .time = h2_bk7258_board_time_api(),
      .log = h2_bk7258_board_log_api(),
  };
  rc = h2_corehttp_create(&http_config, &s_runtime_http, &s_runtime_http_api);
  if (rc != H2_PAL_OK) {
    if (!s_recovery_config_ready) (void)h2_bk7258_board_fs_deinit();
    return rc;
  }
  s_runtime_config = (h2_runtime_config_t){
      .board = H2_BK7258_BOARD_NAME,
      .target = H2_BK7258_CHIP_NAME,
      .chip = H2_BK7258_CHIP_NAME,
      .ble_local_name = h2_bk7258_board_ble_local_name(),
      .firmware_info = h2_bk_platform_firmware_info_api(),
      .device_identity = h2_bk_platform_device_identity_api(),
      .image = h2_bk_platform_image_api(),
      .io_stream = h2_bk_platform_io_stream_api(),
      .mem = h2_bk7258_board_default_allocator(),
      .mem_external = h2_bk7258_board_psram_allocator(),
      .log = h2_bk7258_board_log_api(),
      .time = h2_bk7258_board_time_api(),
      .timer = h2_bk_platform_timer_api(),
      .task = h2_bk7258_board_task_api(),
      .queue = h2_bk7258_board_queue_api(),
      .sync = h2_bk7258_board_sync_api(),
      .fs = &s_runtime_fs,
      .disk = h2_bk_platform_disk_api(),
      .pref = h2_bk7258_board_pref_api(),
#if defined(CONFIG_TRNG_SUPPORT) && CONFIG_TRNG_SUPPORT
      .crypto = h2_bk7258_board_crypto_api(),
#else
      .crypto = h2_pal_unsupported_crypto_api(),
#endif
      .http = &s_runtime_http_api,
      .net = h2_bk_platform_net_api(),
      .netif = h2_bk_platform_netif_api(),
      .mqtt = h2_bk_platform_mqtt_api(),
      .webrtc = h2_bk7258_board_webrtc_api(),
      .wifi_sta = h2_bk7258_board_wifi_sta(),
      .wifi_ap = h2_bk7258_board_wifi_ap(),
      .wifi_csi = h2_bk_platform_wifi_csi(),
      .wifi_settings = h2_bk7258_board_wifi_settings(),
      .ble_host = h2_bk7258_board_ble(),
      .modem = h2_bk7258_board_modem(),
      .display = h2_bk7258_board_display(),
      .power = h2_bk7258_board_power_api(),
#if defined(CONFIG_AUDIO_PLAY) && CONFIG_AUDIO_PLAY && \
    defined(CONFIG_AUDIO_RECORD) && CONFIG_AUDIO_RECORD
      .audio = h2_bk7258_board_audio(),
#else
      .audio = h2_pal_unsupported_audio_api(),
#endif
      .audio_decoder = h2_pal_unsupported_audio_decoder_api(),
      .periph = h2_bk7258_board_periph_api(),
      .button = h2_bk7258_board_button_api(),
      .touch = h2_pal_unsupported_touch_api(),
      .buzzer = h2_pal_unsupported_buzzer_api(),
      .nfc = h2_bk7258_board_nfc_api(),
      .nfc_card_emulation = h2_pal_unsupported_nfc_card_emulation_api(),
      .imu = h2_pal_unsupported_imu_api(),
      .gpio_irq = h2_pal_unsupported_gpio_irq_api(),
      .led = h2_bk7258_board_led_api(),
      .switch_api = h2_pal_unsupported_switch_api(),
      .pwm_switch = h2_bk7258_board_pwm_switch_api(),
      .input = h2_bk7258_board_input_api(),
      .system_event = h2_bk7258_board_system_event_api(),
      .video_decoder = h2_pal_unsupported_video_decoder_api(),
  };
  s_runtime_config_ready = 1;
  *out_config = s_runtime_config;
  return H2_PAL_OK;
}

h2_pal_result_t h2_bk7258_board_runtime_deinit(void) {
  if (!s_runtime_config_ready && !s_recovery_config_ready &&
      s_runtime_http == NULL) {
    return H2_PAL_OK;
  }

  h2_corehttp_destroy(s_runtime_http);
  s_runtime_http = NULL;
  memset(&s_runtime_http_api, 0, sizeof(s_runtime_http_api));
  memset(&s_runtime_config, 0, sizeof(s_runtime_config));
  memset(&s_recovery_config, 0, sizeof(s_recovery_config));
  memset(&s_runtime_fs, 0, sizeof(s_runtime_fs));
  s_runtime_config_ready = 0;
  s_recovery_config_ready = 0;
  return h2_bk7258_board_fs_deinit();
}
