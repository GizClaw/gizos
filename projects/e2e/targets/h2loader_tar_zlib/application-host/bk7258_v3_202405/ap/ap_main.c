#include "bk_private/bk_init.h"
#include "h2_application_host_e2e.h"
#include "h2_bk7258_board.h"
#include "h2_bk_application_host.h"
#include "h2_bk_platform_core.h"
#include "h2_bk_target_task_policy.h"
#include <stdio.h>

static h2_pal_result_t prepare(void *user, h2_runtime_config_t *out) {
    (void)user;
    return h2_bk7258_board_runtime_config(out);
}
static h2_pal_result_t release(void *user) {
    (void)user;
    return h2_bk7258_board_runtime_deinit();
}
static h2_pal_result_t activate(void *user, h2_runtime_t *runtime) {
    (void)user;
    (void)runtime;
    return H2_PAL_OK;
}
static h2_pal_result_t deactivate(void *user) {
    (void)user;
    return H2_PAL_OK;
}
static h2_pal_result_t poll(void *user, int *out_stop) {
    (void)user;
    *out_stop = 0;
    return H2_PAL_OK;
}
static void notify(void *user, h2_pal_application_host_event_t event) {
    (void)user;
    (void)event;
}
static const h2_pal_application_host_vtable_t platform_vtable = {
    .prepare = prepare,
    .activate = activate,
    .poll = poll,
    .deactivate = deactivate,
    .release = release,
    .notify = notify};

static void run_host(void *user) {
    (void)user;
    static h2_bk_application_host_t provider;
    const h2_pal_application_host_api_t platform = {.vtable = &platform_vtable};
    h2_pal_result_t rc = h2_bk_application_host_init(&provider, &platform);
    h2_application_host_t *host = NULL;
    h2_application_host_result_t result = {0};
    if (rc == H2_PAL_OK) {
        const h2_application_host_config_t config = {
            .allocator = h2_bk_platform_sram_allocator(),
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
        (void)h2_pal_time_sleep_ms(h2_bk_platform_time_api(), 1000u);
}
int main(void) {
    if (h2_bk_target_task_policy_install() != H2_PAL_OK)
        return -1;
    bk_init();
    return h2_bk7258_board_start_entry_task("bk/application-host", run_host,
                                            NULL);
}
