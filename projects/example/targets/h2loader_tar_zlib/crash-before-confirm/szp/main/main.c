#include "h2_crash_before_confirm.h"
#include "h2_esp_board.h"
#include "h2_esp_h2loader_ble.h"
#include "h2_esp_h2loader_runtime.h"
#include "h2_esp_target_task_policy.h"

#include "esp_system.h"

#include <stdio.h>

static void crash_now(void *user) {
    (void)user;
    fflush(stdout);
    esp_system_abort("h2loader crash-before-confirm validation");
}

static void image_entry(void *user) {
    (void)user;
    h2_runtime_config_t runtime_config = { 0 };
    h2_runtime_t *runtime = NULL;
    int rc = h2_esp_board_runtime_config(&runtime_config);
    if (rc != H2_PAL_OK ||
        h2_esp_h2loader_app_commands_prepare_serial(
            &runtime_config, "crash-before-confirm", 1u, 3u) != H2_PAL_OK ||
        h2_runtime_init(&runtime_config, &runtime) != H2_PAL_OK) {
        esp_system_abort("h2loader crash-before-confirm runtime init failed");
    }
    rc = h2_esp_h2loader_app_commands_start(
        runtime, "crash-before-confirm", 1u, 3u);
    if (rc != H2_PAL_OK) {
        esp_system_abort("h2loader crash-before-confirm command services failed");
    }
    printf("H2_CRASH_BEFORE_CONFIRM_READY action=crash\n");
    h2_crash_before_confirm_config_t config = {
        .crash = crash_now,
    };
    (void)h2_crash_before_confirm_run(runtime, &config);
    esp_system_abort("h2loader crash-before-confirm returned");
}

void app_main(void) {
    if (h2_esp_target_task_policy_install() != H2_PAL_OK) {
        return;
    }
    h2_pal_result_t rc = h2_esp_board_start_entry_task(
        "szp/crash", image_entry, NULL);
    if (rc != H2_PAL_OK) {
        printf(
            "H2_BOARD_ENTRY_FAIL board=szp image=crash-before-confirm code=%d\n",
            rc);
    }
}
