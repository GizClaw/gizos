#include "result_console.h"
#include "h2_esp_board.h"
#include "h2_esp_board_config.h"
#include "h2_esp_h2loader_ble.h"
#include "h2_esp_h2loader_runtime.h"
#include "h2_esp_target_task_policy.h"
#include "h2_pal_http_device.h"
#include "esp_app_desc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>

static h2_runtime_t *runtime;
static h2_esp_e2e_result_log_t result_log;
static int confirm_app(void *user) { return h2_esp_h2loader_app_confirm(user); }
static h2_pal_http_e2e_result_t result;
static void hold(void) { for (;;) vTaskDelay(pdMS_TO_TICKS(1000u)); }
static void fail(const char *stage, int rc) {
    printf("H2_PAL_HTTP_SETUP_FAIL stage=%s rc=%d\n", stage, rc);
    fflush(stdout);
    hold();
}
static void run(void *user) {
    (void)user;
    vTaskDelay(pdMS_TO_TICKS(5000u));
    char boot[128];
    const int boot_size = snprintf(boot, sizeof(boot), "H2_PAL_HTTP_BOOT board=%s version=%s",
        H2_ESP_BOARD_NAME, esp_app_get_description()->version);
    if (boot_size <= 0 || (size_t)boot_size >= sizeof(boot))
        fail("boot_format", H2_PAL_ERR_TRUNCATED);
    const int boot_rc = h2_esp_e2e_console_record(&result_log.console, boot);
    if (boot_rc != H2_PAL_OK) fail("boot_output", boot_rc);
    int rc;
    for (;;) {
        rc = h2_pal_http_device_run(runtime, &result);
        if (result.passed + result.failed + result.blocked != 0u) break;
        if (rc != H2_PAL_ERR_NOT_FOUND && rc != H2_PAL_ERR_UNAVAILABLE &&
            rc != H2_PAL_ERR_TIMEOUT && rc != H2_PAL_ERR_BUSY) break;
        printf("H2_PAL_HTTP_SETUP_WAIT network=not_ready rc=%d cases_started=0\n", rc);
        fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(3000u));
    }
    if (result.passed + result.failed + result.blocked != H2_PAL_HTTP_E2E_CASE_COUNT)
        fail("fixture_or_network", rc);
    size_t before_report = h2_esp_e2e_result_log_record_count(&result_log);
    h2_pal_http_device_report(runtime, &result);
    int confirm = h2_esp_e2e_result_log_confirm(&result_log, rc,
        before_report + H2_PAL_HTTP_E2E_CASE_COUNT + 2u, confirm_app, runtime);
    for (;;) {
        printf("H2_PAL_HTTP_READY board=" H2_ESP_BOARD_NAME " rc=%d confirm=%d\n", rc, confirm);
        fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(5000u));
        h2_pal_http_device_report(runtime, &result);
    }
}
void app_main(void) {
    int rc = h2_esp_target_task_policy_install();
    if (rc != H2_PAL_OK) fail("task_policy", rc);
    h2_runtime_config_t config = {0};
    rc = h2_esp_board_runtime_config(&config);
    if (rc != H2_PAL_OK) fail("board", rc);
    rc = h2_esp_h2loader_app_commands_prepare_serial(&config, "pal-http", 1u, 3u);
    if (rc != H2_PAL_OK) fail("command_prepare", rc);
    rc = h2_runtime_init(&config, &runtime);
    if (rc != H2_PAL_OK) fail("runtime", rc);
    rc = h2_esp_e2e_result_log_prepare(&result_log, runtime, "pal-http", NULL);
    if (rc != H2_PAL_OK) fail("result_console", rc);
    const h2_pal_task_options_t options = {.name = h2_pal_http_device_runner_task_name};
    h2_pal_task_t *runner = NULL;
    rc = h2_pal_task_start(runtime->task, &options, run, NULL, &runner);
    if (rc != H2_PAL_OK) fail("runner", rc);
}
