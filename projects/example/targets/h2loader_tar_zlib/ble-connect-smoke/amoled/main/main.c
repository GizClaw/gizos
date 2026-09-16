#include "h2_esp_board.h"
#include "h2_esp_h2loader_ble.h"
#include "h2_esp_h2loader_runtime.h"
#include "h2_esp_platform_core.h"
#include "h2_esp_target_task_policy.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

typedef struct scan_state {
    volatile bool found;
    h2_pal_ble_addr_t addr;
    int rssi;
} scan_state_t;

static bool on_scan_result(void *user, const h2_pal_ble_scan_result_t *result) {
    scan_state_t *state = user;
    bool matches = false;
    if (!matches && result->local_name != NULL &&
        result->local_name_len == 5u &&
        memcmp(result->local_name, "H2PAL", 5u) == 0) {
        matches = true;
    }
    if (result->manufacturer_data.len == 7u &&
        result->manufacturer_data.data != NULL &&
        memcmp(result->manufacturer_data.data, "H2PAL1", 6u) == 0) matches = true;
    if (!matches || !result->connectable) {
        return false;
    }
    state->addr = result->addr;
    state->rssi = result->rssi;
    state->found = true;
    printf("H2_AMOLED_JIELI stage=found rssi=%d type=%u addr_type=%u addr=%02x:%02x:%02x:%02x:%02x:%02x\n",
           result->rssi, (unsigned)result->adv_type,
           (unsigned)result->addr.type,
           result->addr.value[0], result->addr.value[1], result->addr.value[2],
           result->addr.value[3], result->addr.value[4], result->addr.value[5]);
    return true;
}

static void image_entry(void *user) {
    (void)user;
    h2_runtime_config_t config = {0};
    h2_runtime_t *runtime = NULL;
    int rc = h2_esp_board_runtime_config(&config);
    if (rc == H2_PAL_OK) {
        rc = h2_esp_h2loader_app_commands_prepare_serial(
            &config, "ble-connect-smoke", 1u, 3u);
    }
    if (rc == H2_PAL_OK) {
        rc = h2_runtime_init(&config, &runtime);
    }
    if (rc == H2_PAL_OK) {
        rc = h2_esp_h2loader_app_commands_start(runtime, "ble-connect-smoke", 1u, 3u);
    }
    if (rc == H2_PAL_OK) {
        rc = h2_esp_platform_confirm_running_app();
    }
    if (rc == H2_PAL_OK) {
        rc = h2_esp_h2loader_app_confirm(runtime);
    }
    printf("H2_AMOLED_JIELI stage=runtime rc=%d\n", rc);
    if (rc == H2_PAL_OK) {
        rc = h2_pal_ble_start(runtime->ble_host);
    }
    printf("H2_AMOLED_JIELI stage=ready rc=%d\n", rc);

    for (unsigned attempt = 1u; rc == H2_PAL_OK && attempt <= 3u; ++attempt) {
        scan_state_t scan = {0};
        const h2_pal_ble_scan_params_t params = {
            .mode = H2_PAL_BLE_SCAN_MODE_ACTIVE,
            .interval_ms = 50u,
            .window_ms = 50u,
            .timeout_ms = 5000u,
            .type = H2_PAL_BLE_SCAN_TYPE_LEGACY,
            .phy_mask = H2_PAL_BLE_SCAN_PHY_1M,
        };
        printf("H2_AMOLED_JIELI stage=scan attempt=%u\n", attempt);
        rc = h2_pal_ble_start_scan(
            runtime->ble_host, &params, on_scan_result, &scan);
        for (unsigned elapsed = 0u; rc == H2_PAL_OK && !scan.found &&
             elapsed < 5500u; elapsed += 50u) {
            vTaskDelay(pdMS_TO_TICKS(50u));
        }
        (void)h2_pal_ble_stop_scan(runtime->ble_host);
        if (rc != H2_PAL_OK || !scan.found) {
            printf("H2_AMOLED_JIELI stage=scan-result attempt=%u found=0 rc=%d\n",
                   attempt, rc);
            rc = H2_PAL_OK;
            vTaskDelay(pdMS_TO_TICKS(500u));
            continue;
        }
        vTaskDelay(pdMS_TO_TICKS(500u));
        const h2_pal_ble_connect_params_t connect_params = {
            .timeout_ms = 10000u,
            .interval_min_ms = 15u,
            .interval_max_ms = 30u,
            .latency = 0u,
            .supervision_timeout_ms = 4000u,
        };
        uint16_t conn_handle = H2_PAL_BLE_INVALID_CONN_HANDLE;
        printf("H2_AMOLED_JIELI stage=connect attempt=%u rssi=%d\n",
               attempt, scan.rssi);
        int connect_rc = h2_pal_ble_connect(
            runtime->ble_host, &scan.addr, &connect_params, &conn_handle);
        printf("H2_AMOLED_JIELI stage=connect-result attempt=%u rc=%d handle=%u\n",
               attempt, connect_rc, (unsigned)conn_handle);
        if (connect_rc == H2_PAL_OK) {
            vTaskDelay(pdMS_TO_TICKS(3000u));
            int disconnect_rc = h2_pal_ble_disconnect(
                runtime->ble_host, conn_handle);
            printf("H2_AMOLED_JIELI stage=disconnect rc=%d\n", disconnect_rc);
        }
        vTaskDelay(pdMS_TO_TICKS(1000u));
    }
    printf("H2_AMOLED_JIELI stage=finished rc=%d\n", rc);
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000u));
    }
}

void app_main(void) {
    if (h2_esp_target_task_policy_install() != H2_PAL_OK) {
        return;
    }
    int rc = h2_esp_board_start_entry_task(
        "amoled/ble-connect-smoke", image_entry, NULL);
    if (rc != H2_PAL_OK) {
        printf("H2_BOARD_ENTRY_FAIL board=amoled image=ble-connect-smoke code=%d\n",
               rc);
    }
}
