#include "h2_esp_board.h"
#include "h2_esp_h2loader_ble.h"
#include "h2_esp_h2loader_runtime.h"
#include "h2_esp_target_task_policy.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdio.h>

static void hold(void) {
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000u));
    }
}

static void image_entry(void *user) {
    (void)user;
    h2_runtime_config_t runtime_config = {0};
    h2_runtime_t *runtime = NULL;
    int rc = h2_esp_board_runtime_config(&runtime_config);
    if (rc == H2_PAL_OK) {
        rc = h2_esp_h2loader_app_commands_prepare_serial(
            &runtime_config, "h2loader-e2e", 1u, 3u);
    }
    if (rc == H2_PAL_OK) {
        rc = h2_runtime_init(&runtime_config, &runtime);
    }
    if (rc == H2_PAL_OK) {
        rc = h2_esp_h2loader_app_commands_start(
            runtime, "h2loader-e2e", 1u, 3u);
    }
    if (rc == H2_PAL_OK) {
        rc = h2_esp_h2loader_app_confirm(runtime);
    }
    printf("H2LOADER_E2E_APP_READY status=%s rc=%d\n",
           rc == H2_PAL_OK ? "PASS" : "FAIL", rc);
    fflush(stdout);
    hold();
}

void app_main(void) {
    if (h2_esp_target_task_policy_install() != H2_PAL_OK) {
        return;
    }
    h2_pal_result_t rc = h2_esp_board_start_entry_task(
        "devkit/h2loader-e2e", image_entry, NULL);
    if (rc != H2_PAL_OK) {
        printf("H2_BOARD_ENTRY_FAIL board=devkit image=h2loader-e2e code=%d\n",
               rc);
        fflush(stdout);
    }
}
