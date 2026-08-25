#include "h2_bleikcp_speed.h"
#include "h2_esp_board.h"
#include "h2_esp_h2loader_ble.h"
#include "h2_esp_platform_core.h"
#include "h2_loader_boot.h"
#include "h2_esp_layout_task_policy.h"

#include <stdio.h>

static int pause_management_advertising(void *user) {
    (void)user;
    return h2_esp_h2loader_app_commands_pause_ble_advertising();
}

static int resume_management_advertising(void *user) {
    (void)user;
    return h2_esp_h2loader_app_commands_resume_ble_advertising();
}

static int confirm_ready(void *user) {
    h2_runtime_t *runtime = user;
    int rc = h2_esp_platform_confirm_running_app();
    return rc == H2_PAL_OK
        ? h2_loader_mark_app_confirmed(runtime->pref)
        : rc;
}

static void image_entry(void *user) {
    (void)user;
    h2_runtime_config_t config = { 0 };
    h2_runtime_t *runtime = NULL;
    int rc = h2_esp_board_runtime_config(&config);
    if (rc == H2_PAL_OK) {
        rc = h2_esp_h2loader_app_commands_prepare_serial(
            &config, "bleikcp-speed-client", 1u, 3u);
    }
    if (rc == H2_PAL_OK) {
        rc = h2_runtime_init(&config, &runtime);
    }
    if (rc == H2_PAL_OK) {
        rc = h2_esp_h2loader_app_commands_start(
            runtime, "bleikcp-speed-client", 1u, 3u);
    }
    if (rc == H2_PAL_OK) {
        const h2_bleikcp_speed_config_t speed_config = {
            .role = H2_BLEIKCP_SPEED_ROLE_CLIENT,
            .advertising_type = H2_PAL_BLE_ADV_TYPE_EXTENDED,
            .scan_type = H2_PAL_BLE_SCAN_TYPE_EXTENDED,
            .pause_management_advertising = pause_management_advertising,
            .resume_management_advertising = resume_management_advertising,
            .ready = confirm_ready,
            .ready_user = runtime,
        };
        rc = h2_bleikcp_speed_run(runtime, &speed_config);
    }
    printf("H2_BLEIKCP_SPEED_FAIL board=szp role=client stage=run rc=%d\n", rc);
}

void app_main(void) {
    if (h2_esp_layout_task_policy_install() != H2_PAL_OK) {
        return;
    }
    int rc = h2_esp_board_start_entry_task(
        "szp/bleikcp-speed-client", image_entry, NULL);
    if (rc != H2_PAL_OK) {
        printf("H2_BOARD_ENTRY_FAIL board=szp image=bleikcp-speed-client code=%d\n", rc);
    }
}
