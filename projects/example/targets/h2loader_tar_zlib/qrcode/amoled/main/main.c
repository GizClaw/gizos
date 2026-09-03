#include "h2_esp_board.h"
#include "h2_esp_h2loader_ble.h"
#include "h2_esp_h2loader_runtime.h"
#include "h2_esp_platform_core.h"
#include "h2_esp_target_task_policy.h"
#include "h2_qrcode_example.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdio.h>

/* Payload of the presented symbol; scanning it opens the repository page. */
#define H2_QRCODE_EXAMPLE_TEXT "https://github.com/GizClaw/gizos"
#define H2_QRCODE_EXAMPLE_BRIGHTNESS_PERCENT 90u

static void qrcode_confirm_app(h2_runtime_t *runtime) {
    h2_pal_result_t confirm_rc = h2_esp_platform_confirm_running_app();
    if (confirm_rc != H2_PAL_OK) {
        printf("H2_QRCODE_EXAMPLE_FAIL stage=ota_confirm rc=%d\n", (int)confirm_rc);
        return;
    }
    int rc = h2_esp_h2loader_app_confirm(runtime);
    if (rc != H2_PAL_OK) {
        printf("H2_QRCODE_EXAMPLE_FAIL stage=h2loader_confirm rc=%d\n", rc);
    }
}

static void image_entry(void *user) {
    (void)user;
    h2_runtime_config_t board_config = { 0 };
    h2_runtime_t *runtime = NULL;
    int rc = h2_esp_board_runtime_config(&board_config);
    if (rc != H2_PAL_OK) {
        printf("H2_QRCODE_EXAMPLE_FAIL stage=runtime_init rc=%d\n", rc);
        return;
    }
    rc = h2_esp_h2loader_app_commands_prepare_serial(
        &board_config, "qrcode", 1u, 3u);
    if (rc != H2_PAL_OK) {
        printf("H2_QRCODE_EXAMPLE_FAIL stage=serial_recovery rc=%d\n", rc);
        return;
    }
    rc = h2_runtime_init(&board_config, &runtime);
    if (rc != H2_PAL_OK) {
        printf("H2_QRCODE_EXAMPLE_FAIL stage=runtime_init rc=%d\n", rc);
        (void)h2_esp_board_runtime_deinit();
        return;
    }
    rc = h2_esp_h2loader_app_commands_start(runtime, "qrcode", 1u, 3u);
    if (rc != H2_PAL_OK) {
        printf("H2_QRCODE_EXAMPLE_FAIL stage=command_services rc=%d\n", rc);
        return;
    }

    if (runtime->display == NULL) {
        printf("H2_QRCODE_EXAMPLE_FAIL stage=display_init rc=%d\n",
               H2_PAL_ERR_UNAVAILABLE);
        return;
    }

    const h2_qrcode_example_config_t config = {
        .text = H2_QRCODE_EXAMPLE_TEXT,
        .ecc = H2_QRCODE_ECC_MEDIUM,
        .max_version = 0,
        .quiet_modules = 0,
        .brightness_percent = H2_QRCODE_EXAMPLE_BRIGHTNESS_PERCENT,
        .on_ready_user = NULL,
        .on_ready = NULL,
    };
    rc = h2_qrcode_example_run(runtime, &config);
    if (rc != H2_PAL_OK) {
        printf("H2_QRCODE_EXAMPLE_FAIL stage=run rc=%d\n", rc);
    } else {
        printf("H2_QRCODE_EXAMPLE_READY text=%s\n", H2_QRCODE_EXAMPLE_TEXT);
        qrcode_confirm_app(runtime);
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
        "amoled/qrcode", image_entry, NULL);
    if (rc != H2_PAL_OK) {
        printf(
            "H2_BOARD_ENTRY_FAIL board=amoled image=qrcode code=%d\n",
            rc);
    }
}
