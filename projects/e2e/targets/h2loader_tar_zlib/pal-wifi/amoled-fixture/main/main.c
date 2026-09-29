#include "esp_app_desc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "h2_esp_board.h"
#include "h2_esp_h2loader_runtime.h"
#include "h2_esp_target_task_policy.h"
#include "h2_pal_wifi_device.h"
#include <stdio.h>
static h2_runtime_t *runtime;
static void hold(void) {
    for (;;)
        vTaskDelay(pdMS_TO_TICKS(1000u));
}
static void fail(const char *stage, int rc) {
    printf("H2_WIFI_SETUP_FAIL stage=%s rc=%d\n", stage, rc);
    fflush(stdout);
    hold();
}
static void run(void *user) {
    (void)user;
    vTaskDelay(pdMS_TO_TICKS(3000u));
    int rc = h2_wifi_fixture_run(runtime);
    int confirm = rc == H2_PAL_OK ? h2_esp_h2loader_app_confirm(runtime) : H2_PAL_ERR_INVALID_STATE;
    printf("H2_WIFI_READY board=amoled-fixture rc=%d confirm=%d\n", rc, confirm);
    fflush(stdout);
    hold();
}
void app_main(void) {
    int rc = h2_esp_target_task_policy_install();
    if (rc)
        fail("task-policy", rc);
    h2_runtime_config_t config = {0};
    rc = h2_esp_board_runtime_config(&config);
    if (rc)
        fail("board", rc);
    config.event_queue_capacity = 64;
    rc = h2_esp_h2loader_app_commands_prepare_serial(&config, "pal-wifi-fixture", 1u, 3u);
    if (rc)
        fail("command", rc);
    rc = h2_runtime_init(&config, &runtime);
    if (rc)
        fail("runtime", rc);
    printf("H2_WIFI_LAUNCHER version=%s board=amoled-fixture\n",
           esp_app_get_description()->version);
    fflush(stdout);
    const h2_pal_task_options_t options = {.name = h2_wifi_device_runner_task_name,
                                           .min_stack_size = 65536};
    h2_pal_task_t *runner = NULL;
    rc = h2_pal_task_start(runtime->task, &options, run, NULL, &runner);
    if (rc)
        fail("runner", rc);
}
