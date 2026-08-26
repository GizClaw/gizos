#include "h2_esp_board.h"
#include "h2_esp_h2loader_ble.h"
#include "h2_esp_platform_core.h"
#include "h2_loader_boot.h"
#include "h2_smoke_display.h"
#include "h2_esp_target_task_policy.h"

#include "esp_err.h"
#include "esp_ota_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdio.h>

static void smoke_confirm_app(h2_runtime_t *runtime) {
    h2_pal_result_t confirm_rc = h2_esp_platform_confirm_running_app();
    if (confirm_rc != H2_PAL_OK) {
        printf("H2_SMOKE_DISPLAY_FAIL stage=ota_confirm rc=%d\n", (int)confirm_rc);
        return;
    }
    int rc = h2_loader_mark_app_confirmed(runtime->pref);
    if (rc != H2_PAL_OK) {
        printf("H2_SMOKE_DISPLAY_FAIL stage=h2loader_confirm rc=%d\n", rc);
    }
}

static void image_entry(void *user) {
    (void)user;
    h2_runtime_config_t board_config = { 0 };
    h2_runtime_t *runtime = NULL;
    int rc = h2_esp_board_runtime_config(&board_config);
    if (rc != H2_PAL_OK) {
        printf("H2_SMOKE_DISPLAY_FAIL stage=runtime_init rc=%d\n", rc);
        return;
    }
    rc = h2_esp_h2loader_app_commands_prepare_serial(
        &board_config, "smoke-display", 1u, 3u);
    if (rc != H2_PAL_OK) {
        printf("H2_SMOKE_DISPLAY_FAIL stage=serial_recovery rc=%d\n", rc);
        return;
    }
    rc = h2_runtime_init(&board_config, &runtime);
    if (rc != H2_PAL_OK) {
        printf("H2_SMOKE_DISPLAY_FAIL stage=runtime_init rc=%d\n", rc);
        (void)h2_esp_board_runtime_deinit();
        return;
    }
    rc = h2_esp_h2loader_app_commands_start(
        runtime, "smoke-display", 1u, 3u);
    if (rc != H2_PAL_OK) {
        printf("H2_SMOKE_DISPLAY_FAIL stage=command_services rc=%d\n", rc);
        return;
    }

    if (runtime->display == NULL) {
        printf("H2_SMOKE_DISPLAY_FAIL stage=display_init rc=%d\n", H2_PAL_ERR_UNAVAILABLE);
        return;
    }

    rc = h2_smoke_display_run(runtime);
    if (rc != H2_DISPLAY_OK) {
        printf("H2_SMOKE_DISPLAY_FAIL stage=run rc=%d\n", rc);
    } else {
        smoke_confirm_app(runtime);
    }

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void app_main(void) {
    if (h2_esp_target_task_policy_install() != H2_PAL_OK) {
        return;
    }
    h2_pal_result_t rc = h2_esp_board_start_entry_task(
        "amoled/display", image_entry, NULL);
    if (rc != H2_PAL_OK) {
        printf(
            "H2_BOARD_ENTRY_FAIL board=amoled image=display code=%d\n",
            rc);
    }
}
