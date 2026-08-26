#include "h2_esp_board.h"
#include "h2_esp_h2loader_ble.h"
#include "h2_esp_h2loader_runtime.h"
#include "h2_loader_boot.h"
#include "h2_smoke_ble_advertising.h"
#include "h2_esp_target_task_policy.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdio.h>

static int smoke_confirm_app(h2_runtime_t *runtime) {
    return h2_esp_h2loader_app_confirm(runtime);
}

static void image_entry(void *user) {
    (void)user;
    h2_runtime_config_t runtime_config = { 0 };
    h2_runtime_t *runtime = NULL;
    int rc = h2_esp_board_runtime_config(&runtime_config);
    if (rc == H2_PAL_OK) {
        rc = h2_esp_h2loader_app_commands_prepare_serial(
            &runtime_config, "ble-broadcaster", 1u, 3u);
    }
    if (rc == H2_PAL_OK) {
        rc = h2_runtime_init(&runtime_config, &runtime);
    }
    if (rc == H2_PAL_OK) {
        rc = h2_esp_h2loader_app_commands_start(
            runtime, "ble-broadcaster", 1u, 3u);
    }
    if (rc == H2_PAL_OK) {
        rc = h2_smoke_ble_advertising_run(runtime);
    }
    if (rc == H2_PAL_OK) {
        rc = smoke_confirm_app(runtime);
    }
    printf("H2_ESP_SMOKE_BLE_ADVERTISING_DONE rc=%d\n", rc);
    fflush(stdout);
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000u));
    }
}

void app_main(void) {
    if (h2_esp_target_task_policy_install() != H2_PAL_OK) {
        return;
    }
    h2_pal_result_t rc = h2_esp_board_start_entry_task(
        "szp/ble-adv", image_entry, NULL);
    if (rc != H2_PAL_OK) {
        printf(
            "H2_BOARD_ENTRY_FAIL board=szp image=ble-broadcaster code=%d\n",
            rc);
    }
}
