#include "h2_bleikcp_speed.h"
#include "h2_esp_board.h"
#include "h2_esp_h2loader_ble.h"
#include "h2_esp_platform_core.h"
#include "h2_loader_boot.h"
#include "h2_esp_layout_task_policy.h"

#include <stdio.h>

static int confirm_ready(void *user) {
    h2_runtime_t *runtime = user;
    int rc = h2_esp_platform_confirm_running_app();
    return rc == H2_PAL_OK
        ? h2_loader_mark_app_confirmed(runtime->pref)
        : rc;
}

static int advertise_ble_service(
    void *user,
    const h2_pal_ble_uuid_t *service_uuid) {
    (void)user;
    return h2_esp_h2loader_app_commands_advertise_ble_service(service_uuid);
}

static void image_entry(void *user) {
    (void)user;
    h2_runtime_config_t config = { 0 };
    h2_runtime_t *runtime = NULL;
    const h2_esp_h2loader_app_commands_config_t commands_config = {
        .active_name = "bleikcp-speed-server",
        .h2loader_partition_id = 1u,
        .coredump_partition_id = 3u,
    };
    int rc = h2_esp_board_runtime_config(&config);
    if (rc == H2_PAL_OK) {
        rc = h2_esp_h2loader_app_commands_prepare_serial_with_config(
            &config, &commands_config);
    }
    if (rc == H2_PAL_OK) {
        rc = h2_runtime_init(&config, &runtime);
    }
    if (rc == H2_PAL_OK) {
        rc = h2_esp_h2loader_app_commands_start_with_config(
            runtime, &commands_config);
    }
    if (rc == H2_PAL_OK) {
        const h2_bleikcp_speed_config_t speed_config = {
            .role = H2_BLEIKCP_SPEED_ROLE_SERVER,
            .advertising_type = H2_PAL_BLE_ADV_TYPE_EXTENDED,
            .scan_type = H2_PAL_BLE_SCAN_TYPE_EXTENDED,
            .advertise_server_service = advertise_ble_service,
            .ready = confirm_ready,
            .ready_user = runtime,
        };
        rc = h2_bleikcp_speed_run(runtime, &speed_config);
    }
    printf("H2_BLEIKCP_SPEED_FAIL board=amoled role=server stage=run rc=%d\n", rc);
}

void app_main(void) {
    if (h2_esp_layout_task_policy_install() != H2_PAL_OK) {
        return;
    }
    int rc = h2_esp_board_start_entry_task(
        "amoled/bleikcp-speed-server", image_entry, NULL);
    if (rc != H2_PAL_OK) {
        printf("H2_BOARD_ENTRY_FAIL board=amoled image=bleikcp-speed-server code=%d\n", rc);
    }
}
