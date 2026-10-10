#include "h2_mosaico_loader_usb.h"
#include "h2_esp_board.h"
#include "h2_esp_h2loader_runtime.h"
#include "h2_esp_target_task_policy.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>

static void installing(void *user) {
    (void)user;
    printf("H2_MOSAICO_LOADER_INSTALL stack_free=%u\n",
           (unsigned)uxTaskGetStackHighWaterMark(NULL));
    fflush(stdout);
}

static void launching(void *user) {
    (void)user;
    printf("H2_MOSAICO_LOADER_LAUNCH stack_free=%u\n",
           (unsigned)uxTaskGetStackHighWaterMark(NULL));
    fflush(stdout);
}

/* Startup decompression runs on an artifact-owned stack, as on DevKit. */
static void entry(void *unused) {
    (void)unused;
    h2_runtime_config_t config;
    h2_runtime_t *runtime = NULL;
    h2_pal_result_t rc = h2_esp_board_runtime_config(&config);
    if (rc == H2_PAL_OK) rc = h2_runtime_init(&config, &runtime);
    if (rc == H2_PAL_OK) {
        const h2_esp_h2loader_config_t loader = {
            .board = "esp_mosaico", .show_installing = installing,
            .show_launching = launching,
        };
        h2_esp_h2loader_run_with_config(runtime, &loader);
    }
    printf("H2_MOSAICO_LOADER_STOP rc=%d\n", rc);
    fflush(stdout);
    /* Keep the Runtime alive for component-owned services after a failure. */
    for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
}

void app_main(void) {
    if (h2_mosaico_loader_usb_init() != H2_PAL_OK) return;
    vTaskDelay(pdMS_TO_TICKS(1500));
    printf("H2_MOSAICO_LOADER_BOOT reset_reason=%d entry_stack=65536\n",
           (int)esp_reset_reason());
    fflush(stdout);
    if (h2_esp_target_task_policy_install() != H2_PAL_OK) return;
    if (xTaskCreatePinnedToCoreWithCaps(entry, "esp_mosaico/h2loader", 65536,
            NULL, tskIDLE_PRIORITY + 4, NULL, tskNO_AFFINITY,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        puts("H2_MOSAICO_LOADER_STOP stage=entry rc=-5");
        fflush(stdout);
    }
}
