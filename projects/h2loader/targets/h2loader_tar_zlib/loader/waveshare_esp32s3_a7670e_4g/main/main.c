#include "h2_esp_board.h"
#include "h2_esp_h2loader_ble.h"
#include "h2_esp_h2loader_runtime.h"
#include "h2_esp_target_task_policy.h"

#include <stdio.h>

static void image_entry(void *user) {
    (void)user;
    h2_runtime_config_t runtime_config;
    h2_runtime_t *runtime = NULL;
    h2_pal_result_t rc = h2_esp_board_runtime_config(&runtime_config);
    if (rc != H2_PAL_OK) {
        printf("H2_LOADER_READY target=esp status=runtime_config_fail code=%d\n", rc);
        return;
    }
    rc = h2_runtime_init(&runtime_config, &runtime);
    if (rc != H2_PAL_OK) {
        printf("H2_LOADER_READY target=esp status=runtime_init_fail code=%d\n", rc);
        (void)h2_esp_board_runtime_deinit();
        return;
    }
    const h2_esp_h2loader_config_t loader_config = {
        .board = "waveshare_esp32s3_a7670e_4g",
    };
    h2_esp_h2loader_run_with_ble_config(runtime, &loader_config);
}

void app_main(void) {
    if (h2_esp_target_task_policy_install() != H2_PAL_OK) {
        return;
    }
    h2_pal_result_t rc = h2_esp_board_start_entry_task(
        "waveshare-a7670e/h2loader", image_entry, NULL);
    if (rc != H2_PAL_OK) {
        printf(
            "H2_BOARD_ENTRY_FAIL board=waveshare_esp32s3_a7670e_4g image=h2loader code=%d\n",
            rc);
    }
}
