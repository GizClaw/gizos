#include "h2_kickpi_k4b_board.h"
#include "h2_kickpi_k4b_board_private.h"

#include "h2_linux_platform.h"
#include "h2/pal/h2_pal_unsupported.h"

#include <string.h>

h2_pal_result_t h2_kickpi_k4b_board_runtime_config(
    h2_runtime_config_t *out_config,
    const h2_kickpi_k4b_board_providers_t *providers) {
    if (out_config == NULL || providers == NULL || providers->audio == NULL ||
        providers->audio_decoder == NULL || providers->video_decoder == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    memset(out_config, 0, sizeof(*out_config));

    const h2_linux_display_config_t display_config = {
        .device_path = "/dev/fb0",
        .width = H2_KICKPI_K4B_DISPLAY_WIDTH,
        .height = H2_KICKPI_K4B_DISPLAY_HEIGHT,
    };
    h2_pal_result_t result = h2_linux_configure_display(&display_config);
    if (result != H2_PAL_OK) return result;
    result = h2_kickpi_k4b_board_configure_input();
    if (result != H2_PAL_OK) return result;

    *out_config = (h2_runtime_config_t){
        .board = "kickpi_k4b",
        .target = "allwinner-linux",
        .chip = "t113-s3",
        .firmware_info = h2_pal_unsupported_firmware_info_api(),
        .mem = h2_linux_mem_api(),
        .log = h2_linux_log_api(),
        .time = h2_linux_time_api(),
        .timer = h2_pal_unsupported_timer_api(),
        .task = h2_linux_task_api(),
        .queue = h2_linux_queue_api(),
        .sync = h2_linux_sync_api(),
        .fs = h2_pal_unsupported_fs_api(),
        .disk = h2_pal_unsupported_disk_api(),
        .pref = h2_pal_unsupported_pref_api(),
        .crypto = h2_pal_unsupported_crypto_api(),
        .http = h2_pal_unsupported_http_api(),
        .net = h2_pal_unsupported_net_api(),
        .netif = h2_linux_netif_api(),
        .mqtt = h2_pal_unsupported_mqtt_api(),
        .webrtc = h2_pal_unsupported_webrtc_api(),
        .wifi_sta = h2_pal_unsupported_wifi_sta_api(),
        .wifi_ap = h2_pal_unsupported_wifi_ap_api(),
        .wifi_csi = h2_pal_unsupported_wifi_csi_api(),
        .wifi_settings = h2_pal_unsupported_wifi_settings_api(),
        .ble_host = h2_pal_unsupported_ble_host_api(),
        .modem = h2_pal_unsupported_modem_api(),
        .power = h2_pal_unsupported_power_api(),
        .display = h2_linux_display_api(),
        .audio = providers->audio,
        .audio_decoder = providers->audio_decoder,
        .periph = h2_kickpi_k4b_board_periph_api(),
        .button = h2_linux_gpio_button_api(),
        .touch = h2_linux_evdev_touch_api(),
        .buzzer = h2_pal_unsupported_buzzer_api(),
        .nfc = h2_pal_unsupported_nfc_api(),
        .nfc_card_emulation = h2_pal_unsupported_nfc_card_emulation_api(),
        .imu = h2_pal_unsupported_imu_api(),
        .gpio_irq = h2_pal_unsupported_gpio_irq_api(),
        .led = h2_pal_unsupported_led_api(),
        .switch_api = h2_pal_unsupported_switch_api(),
        .pwm_switch = h2_pal_unsupported_pwm_switch_api(),
        .input = h2_pal_unsupported_input_api(),
        .system_event = h2_linux_system_event_api(),
        .video_decoder = providers->video_decoder,
        .event_queue_capacity = H2_RUNTIME_DEFAULT_EVENT_QUEUE_CAPACITY,
    };
    return H2_PAL_OK;
}
