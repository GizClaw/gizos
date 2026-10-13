#include "h2_application_host_e2e.h"
#include "h2_esp_application_host.h"
#include "h2_esp_board.h"
#include "h2_esp_platform_core.h"
#include "h2_esp_target_task_policy.h"
#include <stdio.h>

static h2_pal_result_t startup(void *user, h2_runtime_config_t *out) {
    (void)user;
    return h2_esp_board_runtime_config(out);
}
static h2_pal_result_t shutdown(void *user) {
    (void)user;
    return h2_esp_board_runtime_deinit();
}
static void run_host(void *user) {
    (void)user;
    static h2_esp_application_host_t provider;
    const h2_application_host_sdk_config_t sdk = {.startup = startup,
                                                  .shutdown = shutdown};
    h2_pal_result_t rc = h2_esp_application_host_init(&provider, &sdk);
    h2_application_host_t *host = NULL;
    h2_application_host_result_t result = {0};
    if (rc == H2_PAL_OK) {
        const h2_application_host_config_t config = {
            .allocator = h2_esp_platform_default_allocator(),
            .provider = &provider.api,
            .entry = h2_application_host_e2e_run,
            .run_ms = 100u,
            .stack_size = 8192u,
            .control_stack_size = 8192u};
        rc = h2_application_host_create(&config, &host);
    }
    if (rc == H2_PAL_OK)
        rc = h2_application_host_run(host, &result);
    if (!result.retained)
        (void)h2_application_host_destroy(&host);
    printf("APPLICATION_HOST_SDK rc=%d retained=%d\n", rc, result.retained);
    // Returning from a BSP entry can release its providers. Keep the owner
    // task/context alive when the shared Host cannot prove safe destruction.
    while (result.retained)
        (void)h2_pal_time_sleep_ms(h2_esp_platform_time_api(), 1000u);
}
void app_main(void) {
    if (h2_esp_target_task_policy_install() == H2_PAL_OK)
        run_host(NULL);
}
