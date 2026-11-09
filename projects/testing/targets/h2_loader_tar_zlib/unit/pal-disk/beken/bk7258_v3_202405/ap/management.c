#include "launcher.h"

#include "h2_bk7258_board.h"
#include "h2_bk7258_board_private.h"
#include "h2_bk_platform_core.h"
#include "h2_bk_platform_image.h"
#include "h2_device_service.h"
#include "h2_pal_unsupported.h"
#include "h2_runtime.h"

static h2_pal_fs_api_t management_fs;
static h2_runtime_t* runtime;
static h2_device_app_commands_t management = {
    .config =
        {
            .active_name = "pal-disk",
            .hardware_capabilities = H2_LOADER_CAPABILITY_UART,
            .h2loader_partition_id = H2_BK_PRIMARY_PARTITION_ID,
            .app_partition_id = H2_BK_APP_PARTITION_ID,
            .coredump_partition_id = H2_BK7258_COREDUMP_OFFSET,
            .app_entry_path = "app/bk/app_ab_crc.rbl",
            .boot_policy_flags = H2_DEVICE_BOOT_PREPARE_SELECTION |
                                 H2_DEVICE_BOOT_PREPARE_APP_RESTART,
            .identity_policy = H2_DEVICE_IDENTITY_PADDED_WINDOW,
            .serial_post_write_poll_ms = 1u,
            .command_stack_size = 32768u,
        },
};

int h2_bk_pal_disk_management_start(void) {
  int rc = h2_bk7258_board_fs_init(&management_fs);
  /* Configuration alone does not mount storage. App confirmation removes
   * staging files, so both roots must be ready before publishing services. */
  if (!rc) rc = h2_pal_fs_prepare(&management_fs, H2_BK7258_DL_MOUNT_PATH);
  if (!rc) rc = h2_pal_fs_prepare(&management_fs, H2_BK7258_DATA_MOUNT_PATH);
  if (!rc) rc = h2_bk_platform_boot_startup();
  if (rc) return rc;
  /* Assemble only management providers. Do not invoke the board's product
   * constructor, which may configure display, audio, NFC or modem pins. */
  const h2_runtime_config_t config = {
      .board = H2_BK7258_BOARD_NAME,
      .target = H2_BK7258_CHIP_NAME,
      .chip = H2_BK7258_CHIP_NAME,
      .firmware_info = h2_bk_platform_firmware_info_api(),
      .device_identity = h2_bk_platform_device_identity_api(),
      .image = h2_bk_platform_image_api(),
      .io_stream = h2_bk_platform_io_stream_api(),
      .mem = h2_bk_platform_psram_allocator(),
      .mem_external = h2_bk_platform_psram_allocator(),
      .log = h2_bk_platform_log_api(),
      .time = h2_bk_platform_time_api(),
      .timer = h2_bk_platform_timer_api(),
      .task = h2_bk_platform_task_api(),
      .queue = h2_bk_platform_queue_api(),
      .sync = h2_bk_platform_sync_api(),
      .fs = &management_fs,
      .pref = h2_bk_platform_pref_api(),
      .power = h2_bk_platform_power_api(),
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
    rc = h2_device_app_commands_start_serial_named(
        &management, runtime, "pal-disk", H2_LOADER_CAPABILITY_UART);
  /* Installation health is distinct from the subsequent diagnostic verdict.
   * State and providers remain boot-owned even if service startup fails. */
  if (!rc) rc = h2_device_app_confirm(&management, runtime);
  return rc;
}
