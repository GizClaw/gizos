#include "h2_esp_board.h"
#include "h2_esp_h2loader_ble.h"
#include "h2_esp_platform_core.h"
#include "h2_loader_boot.h"
#include "h2_smoke_gizclaw_ping_speed.h"
#include "h2_esp_layout_task_policy.h"

#include "esp_err.h"
#include "esp_ota_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdio.h>
#include <string.h>

#ifndef H2_GIZCLAW_SERVER_ENDPOINT
#define H2_GIZCLAW_SERVER_ENDPOINT ""
#endif

#ifndef H2_GIZCLAW_CONNECT_TIMEOUT_MS
#define H2_GIZCLAW_CONNECT_TIMEOUT_MS 45000
#endif

extern const char h2_gizclaw_client_private_key[];
extern const char h2_smoke_wifi_ssid[];
extern const char h2_smoke_wifi_password[];

static h2_gizclaw_str_t smoke_str(const char *text) {
    h2_gizclaw_str_t value = {
        .data = text,
        .len = text == NULL ? 0u : strlen(text),
    };
    return value;
}

static void smoke_confirm_app(h2_runtime_t *runtime) {
    h2_pal_result_t confirm_rc = h2_esp_platform_confirm_running_app();
    if (confirm_rc != H2_PAL_OK) {
        printf("FAIL gizclaw-ping-speed stage=ota_confirm rc=%d\n", (int)confirm_rc);
        return;
    }
    int rc = h2_loader_mark_app_confirmed(runtime->pref);
    if (rc != H2_PAL_OK) {
        printf("FAIL gizclaw-ping-speed stage=h2loader_confirm rc=%d\n", rc);
    }
}

static int smoke_copy_wifi_field(char *dst, size_t dst_cap, size_t *out_len, const char *src) {
    size_t len;

    if (dst == NULL || dst_cap == 0u || out_len == NULL || src == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    len = strlen(src);
    if (len > dst_cap) {
        return H2_PAL_ERR_NO_SPACE;
    }
    memcpy(dst, src, len);
    if (len < dst_cap) {
        dst[len] = '\0';
    }
    *out_len = len;
    return H2_PAL_OK;
}

static int smoke_apply_wifi_env(h2_runtime_t *runtime) {
    h2_pal_wifi_sta_config_t sta;
    int rc;

    if (h2_smoke_wifi_ssid[0] == '\0') {
        return H2_PAL_OK;
    }
    memset(&sta, 0, sizeof(sta));
    rc = smoke_copy_wifi_field(
        sta.ssid,
        sizeof(sta.ssid) - 1u,
        &sta.ssid_len,
        h2_smoke_wifi_ssid);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    rc = smoke_copy_wifi_field(
        sta.password,
        sizeof(sta.password) - 1u,
        &sta.password_len,
        h2_smoke_wifi_password);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    return h2_pal_wifi_settings_set_saved_sta_config(runtime->wifi_settings, &sta);
}

static void image_entry(void *user) {
    (void)user;
    h2_runtime_config_t board_config = { 0 };
    h2_runtime_t *runtime = NULL;
    int rc = h2_esp_board_runtime_config(&board_config);
    if (rc != H2_PAL_OK) {
        printf("FAIL gizclaw-ping-speed stage=runtime_init rc=%d\n", rc);
        return;
    }
    rc = h2_esp_h2loader_app_commands_prepare_serial(
        &board_config, "gizclaw-ping-speed", 1u, 3u);
    if (rc != H2_PAL_OK) {
        printf("FAIL gizclaw-ping-speed stage=serial_recovery rc=%d\n", rc);
        return;
    }
    rc = h2_runtime_init(&board_config, &runtime);
    if (rc != H2_PAL_OK) {
        printf("FAIL gizclaw-ping-speed stage=runtime_init rc=%d\n", rc);
        (void)h2_esp_board_runtime_deinit();
        return;
    }
    rc = h2_esp_h2loader_app_commands_start(
        runtime, "gizclaw-ping-speed", 1u, 3u);
    if (rc != H2_PAL_OK) {
        printf("FAIL gizclaw-ping-speed stage=command_services rc=%d\n", rc);
        return;
    }
    int wifi_env_rc = smoke_apply_wifi_env(runtime);
    if (wifi_env_rc != H2_PAL_OK) {
        printf("SKIP gizclaw-ping-speed reason=server_info_unavailable stage=wifi_config rc=%d\n", wifi_env_rc);
    }

    h2_smoke_gizclaw_ping_speed_config_t config = {
        .server_endpoint = smoke_str(H2_GIZCLAW_SERVER_ENDPOINT),
        .private_key = smoke_str(h2_gizclaw_client_private_key),
        .cipher_mode = H2_GIZCLAW_CIPHER_CHACHA20_POLY1305,
        .connect_timeout_ms = H2_GIZCLAW_CONNECT_TIMEOUT_MS,
        .wifi_connect_timeout_ms = 90000u,
        .server_info_timeout_ms = 10000u,
        .poll_window_ms = 3000u,
    };

    h2_smoke_gizclaw_result_t result = wifi_env_rc == H2_PAL_OK ?
        h2_smoke_gizclaw_ping_speed_run(runtime, &config) :
        H2_SMOKE_GIZCLAW_SKIP;
    if (result == H2_SMOKE_GIZCLAW_OK) {
        smoke_confirm_app(runtime);
    }

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void app_main(void) {
    if (h2_esp_layout_task_policy_install() != H2_PAL_OK) {
        return;
    }
    h2_pal_result_t rc = h2_esp_board_start_entry_task(
        "amoled/ping", image_entry, NULL);
    if (rc != H2_PAL_OK) {
        printf(
            "H2_BOARD_ENTRY_FAIL board=amoled image=gizclaw-ping-speed code=%d\n",
            rc);
    }
}
