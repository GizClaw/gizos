#include "h2_mosaico_loader_usb.h"
#include "h2_esp_board.h"
#include "h2_esp_h2loader_runtime.h"
#include "h2_esp_target_task_policy.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>

#ifdef H2_MOSAICO_OTA_DIAGNOSTIC
#include "nvs_flash.h"
#include "nvs.h"
#include "h2_esp_platform_safe_call.h"
#include <string.h>
typedef struct diagnostic_observation {
    char phase[24];
    uint32_t boot;
    int result;
    int persisted;
} diagnostic_observation_t;
/* Dedicated test namespace; never erase configuration or crash records. */
static void diagnostic_persist(void *user) {
    diagnostic_observation_t *data = user;
    nvs_handle_t handle;
    esp_err_t rc = nvs_flash_init();
    if (rc != ESP_OK) { data->persisted = rc; return; }
    rc = nvs_open("mos_ota_diag", NVS_READWRITE, &handle);
    if (rc != ESP_OK) { data->persisted = rc; return; }
    if (data->boot == 0u) {
        rc = nvs_get_u32(handle, "boot", &data->boot);
        if (rc != ESP_OK && rc != ESP_ERR_NVS_NOT_FOUND) {
            nvs_close(handle); data->persisted = rc; return;
        }
        ++data->boot;
    }
    rc = nvs_set_u32(handle, "boot", data->boot);
    if (rc == ESP_OK) rc = nvs_set_str(handle, "phase", data->phase);
    if (rc == ESP_OK) rc = nvs_set_i32(handle, "result", data->result);
    if (rc == ESP_OK) rc = nvs_set_i32(handle, "reset", (int)esp_reset_reason());
    if (rc == ESP_OK) rc = nvs_commit(handle);
    nvs_close(handle);
    data->persisted = rc;
}
static void diagnostic_phase(const char *phase, int result) {
    static uint32_t boot;
    diagnostic_observation_t data = {.boot = boot, .result = result};
    (void)snprintf(data.phase, sizeof(data.phase), "%s", phase);
    int rc = h2_esp_platform_safe_call(diagnostic_persist, &data, sizeof(data), 8192u);
    if (rc == H2_PAL_OK && data.persisted == ESP_OK) boot = data.boot;
    printf("H2_MOSAICO_OTA_DEBUG boot=%lu phase=%s result=%d safe=%d persist=%d\n",
           (unsigned long)data.boot, phase, result, rc, data.persisted);
    fflush(stdout);
}
#else
#define diagnostic_phase(phase, result) ((void)0)
#endif

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
    diagnostic_phase("entry", 0);
    h2_runtime_config_t config;
    h2_runtime_t *runtime = NULL;
    h2_pal_result_t rc = h2_esp_board_runtime_config(&config);
    diagnostic_phase("board", rc);
    if (rc == H2_PAL_OK) rc = h2_runtime_init(&config, &runtime);
    diagnostic_phase("runtime", rc);
    if (rc == H2_PAL_OK) {
        const h2_esp_h2loader_config_t loader = {
            .board = "esp_mosaico", .show_installing = installing,
            .show_launching = launching,
        };
        diagnostic_phase("run", 0);
        h2_esp_h2loader_run_with_config(runtime, &loader);
    }
    printf("H2_MOSAICO_LOADER_STOP rc=%d\n", rc);
    fflush(stdout);
    /* Keep the Runtime alive for component-owned services after a failure. */
    for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
}

void app_main(void) {
    diagnostic_phase("usb.begin", 0);
    int usb_rc = h2_mosaico_loader_usb_init();
    diagnostic_phase("usb.done", usb_rc);
    if (usb_rc != H2_PAL_OK) return;
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
