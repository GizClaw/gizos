#include "bk_private/bk_init.h"
#include "h2_bk7258_board.h"
#include "h2_bk_h2loader.h"
#include "h2_bk_platform_core.h"
#include "h2_bk_target_task_policy.h"
#include "h2_pal_wifi_device.h"
#include <os/os.h>
#include <stdio.h>
static h2_runtime_t *runtime;
static void hold(void) {
    for (;;)
        rtos_delay_milliseconds(1000u);
}
static void fail(const char *stage, int rc) {
    printf("H2_WIFI_SETUP_FAIL stage=%s rc=%d\n", stage, rc);
    fflush(stdout);
    hold();
}
static void run(void *user) {
    (void)user;
    rtos_delay_milliseconds(3000u);
    h2_pal_firmware_info_t info = {0};
    int rc = h2_pal_firmware_info_get_current(runtime->firmware_info, &info);
    if (rc)
        fail("firmware-info", rc);
    rc = h2_wifi_device_run(runtime, info.version, "bk7258");
    int confirm =
        rc == H2_PAL_OK ? h2_bk_h2loader_confirm_current_app(runtime) : H2_PAL_ERR_INVALID_STATE;
    printf("H2_WIFI_READY board=bk7258 rc=%d confirm=%d\n", rc, confirm);
    fflush(stdout);
    hold();
}
static void entry(void *user) {
    (void)user;
    h2_runtime_config_t config = {0};
    int rc = h2_bk7258_board_runtime_config(&config);
    if (rc)
        fail("board", rc);
    config.event_queue_capacity = 64;
    rc = h2_runtime_init(&config, &runtime);
    if (rc)
        fail("runtime", rc);
    rc = h2_bk_h2loader_start_app_iostreamikcp_with_capabilities(
        runtime, "pal-wifi", H2_LOADER_CAPABILITY_UART | H2_LOADER_CAPABILITY_WIFI);
    if (rc)
        fail("commands", rc);
    const h2_pal_task_options_t options = {.name = h2_wifi_device_runner_task_name};
    h2_pal_task_t *runner = NULL;
    rc = h2_pal_task_start(runtime->task, &options, run, NULL, &runner);
    if (rc)
        fail("runner", rc);
    hold();
}
int main(void) {
    int rc = h2_bk_target_task_policy_install();
    if (rc)
        return -1;
    bk_init();
    rc = h2_bk7258_board_start_entry_task("bk/pal-wifi", entry, NULL);
    if (rc)
        fail("entry", rc);
    return 0;
}
