#include "h2_esp_board.h"
#include "h2_esp_h2loader_ble.h"
#include "h2_esp_platform_core.h"
#include "h2_esp_h2loader_runtime.h"
#include "h2_partial_update_smoke.h"
#include "h2_esp_target_task_policy.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"

#include <stdio.h>

static void image_entry(void *user) {
    h2_runtime_config_t runtime_config = {0};
    h2_runtime_t *runtime = NULL;
    h2_partial_update_smoke_config_t smoke_config = {
        .app_generation = H2_PARTIAL_APP_GENERATION,
    };
    const h2_esp_h2loader_app_commands_config_t commands_config = {
        .active_name = "partial-update",
        .active_version = H2_PARTIAL_APP_GENERATION,
        .hardware_capabilities =
            H2_LOADER_CAPABILITY_UART | H2_LOADER_CAPABILITY_BLE,
        .h2loader_partition_id = 1u,
        .app_partition_id = 2u,
        .coredump_partition_id = 3u,
    };
    int rc;
    (void)user;

    rc = h2_esp_board_runtime_config(&runtime_config);
    if (rc != H2_PAL_OK) {
        printf("H2_PARTIAL_UPDATE_SMOKE result=FAIL stage=board rc=%d\n", rc);
        esp_restart();
    }
    rc = h2_esp_h2loader_app_commands_prepare_serial_with_config(
        &runtime_config, &commands_config);
    if (rc != H2_PAL_OK) {
        printf("H2_PARTIAL_UPDATE_SMOKE result=FAIL stage=serial_recovery rc=%d\n", rc);
        esp_restart();
    }
    rc = h2_runtime_init(&runtime_config, &runtime);
    if (rc != H2_PAL_OK) {
        printf("H2_PARTIAL_UPDATE_SMOKE result=FAIL stage=runtime_init rc=%d\n", rc);
        esp_restart();
    }
    rc = h2_esp_h2loader_app_commands_start_with_config(
        runtime, &commands_config);
    if (rc != H2_PAL_OK) {
        printf("H2_PARTIAL_UPDATE_SMOKE result=FAIL stage=command_services rc=%d\n", rc);
        esp_restart();
    }
    if (rc == H2_PAL_OK) {
        rc = h2_partial_update_smoke_run(runtime, &smoke_config);
    }
    if (rc == H2_PAL_OK) {
        rc = h2_esp_platform_confirm_running_app();
    }
    if (rc == H2_PAL_OK) {
        rc = h2_esp_h2loader_app_confirm(runtime);
    }
    if (rc != H2_PAL_OK) {
        printf("H2_PARTIAL_UPDATE_SMOKE result=FAIL stage=confirm rc=%d\n", rc);
        esp_restart();
    }
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        (void)h2_partial_update_smoke_run(runtime, &smoke_config);
    }
}

void app_main(void) {
    if (h2_esp_target_task_policy_install() != H2_PAL_OK) {
        return;
    }
    h2_pal_result_t rc = h2_esp_board_start_entry_task(
        "szp/partial-update", image_entry, NULL);
    if (rc != H2_PAL_OK) {
        printf("H2_BOARD_ENTRY_FAIL board=szp image=partial-update code=%d\n", rc);
    }
}
