#include "h2_esp_board.h"
#include "h2_esp_board_config.h"
#include "h2_esp_board_private.h"

#include "h2_corehttp.h"

#include "h2_esp_platform_core.h"

#include <string.h>

static h2_pal_fs_api_t s_runtime_fs;
static h2_pal_fs_vtable_t s_runtime_fs_vtable;
static h2_runtime_config_t s_runtime_config;
static h2_corehttp_t *s_runtime_http;
static h2_pal_http_api_t s_runtime_http_api;
static int s_runtime_config_ready;

static int runtime_fs_clear(void *user, const char *path) {
    (void)user;
    return h2_esp_board_fs_clear(path);
}

h2_pal_result_t h2_esp_board_runtime_config(h2_runtime_config_t *out_config) {
    if (out_config == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    memset(out_config, 0, sizeof(*out_config));
    if (s_runtime_config_ready) {
        *out_config = s_runtime_config;
        return H2_PAL_OK;
    }
    h2_pal_result_t rc = h2_esp_board_fs_init(&s_runtime_fs);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    if (rc == H2_PAL_OK && s_runtime_fs.vtable != NULL) {
        s_runtime_fs_vtable = *s_runtime_fs.vtable;
        s_runtime_fs_vtable.clear = runtime_fs_clear;
        s_runtime_fs.vtable = &s_runtime_fs_vtable;
    }
    const h2_corehttp_config_t http_config = {
        /* HTTP request buffers (16 KiB headers per request) live in PSRAM:
         * internal RAM on this board is reserved for Wi-Fi/lwIP, BLE and the
         * display DMA buffer, and a second concurrent HTTPS session must not
         * compete with them. */
        .allocator = h2_esp_board_psram_allocator(),
        .net = h2_esp_platform_net_api(),
        .time = h2_esp_board_time_api(),
        .log = h2_esp_board_log_api(),
    };
    rc = h2_corehttp_create(
        &http_config, &s_runtime_http, &s_runtime_http_api);
    if (rc != H2_PAL_OK) {
        (void)h2_esp_board_fs_deinit();
        return rc;
    }
    *out_config = (h2_runtime_config_t){
        .board = H2_ESP_BOARD_NAME,
        .target = "esp32s3",
        .chip = "esp32s3",
        .firmware_info = h2_esp_platform_firmware_info_api(),
        /* Protocol buffers (H2Peer, h2sctp, GizClaw) from PSRAM like the
         * DevKit; with them in internal RAM a live Peer leaves under 10 KiB
         * for the next TLS session and mbedtls_ssl_setup fails. */
        .mem = h2_esp_board_psram_allocator(),
        .log = h2_esp_board_log_api(),
        .time = h2_esp_board_time_api(),
        .timer = h2_pal_unsupported_timer_api(),
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
        .touch = h2_esp_board_touch_api(),
        .buzzer = h2_pal_unsupported_buzzer_api(),
        .nfc = h2_pal_unsupported_nfc_api(),
        .nfc_card_emulation = h2_pal_unsupported_nfc_card_emulation_api(),
        .imu = h2_esp_board_imu_api(),
        .gpio_irq = h2_pal_unsupported_gpio_irq_api(),
        .led = h2_pal_unsupported_led_api(),
        .switch_api = h2_pal_unsupported_switch_api(),
        .pwm_switch = h2_pal_unsupported_pwm_switch_api(),
        .input = h2_esp_board_input_api(),
        .system_event = h2_esp_board_system_event_api(),
        .video_decoder = h2_pal_unsupported_video_decoder_api(),
    };
    s_runtime_config = *out_config;
    s_runtime_config_ready = 1;
    return H2_PAL_OK;
}

h2_pal_result_t h2_esp_board_runtime_deinit(void) {
    if (!s_runtime_config_ready && s_runtime_http == NULL) {
        return H2_PAL_OK;
    }

    h2_pal_result_t rc = h2_esp_board_audio_deinit();
    if (rc != H2_PAL_OK) {
        return rc;
    }
    h2_corehttp_destroy(s_runtime_http);
    s_runtime_http = NULL;
    memset(&s_runtime_http_api, 0, sizeof(s_runtime_http_api));
    memset(&s_runtime_config, 0, sizeof(s_runtime_config));
    memset(&s_runtime_fs, 0, sizeof(s_runtime_fs));
    s_runtime_config_ready = 0;
    return h2_esp_board_fs_deinit();
}
