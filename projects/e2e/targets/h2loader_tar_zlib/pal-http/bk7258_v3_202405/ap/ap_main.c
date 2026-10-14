#include "h2_pal_e2e_application_host.h"
#include "h2_bk7258_board.h"
#include "h2_bk_h2loader.h"
#include "h2_bk_target_task_policy.h"
#include "h2_pal_http_device.h"
#include "bk_private/bk_init.h"
#include <os/os.h>
#include <stdio.h>

static h2_runtime_t *runtime;
static h2_pal_http_e2e_result_t result;
static void hold(void) { for (;;) rtos_delay_milliseconds(1000u); }
static void fail(const char *stage, int rc) {
    printf("H2_PAL_HTTP_SETUP_FAIL stage=%s rc=%d\n", stage, rc);
    fflush(stdout);
    hold();
}
static h2_pal_result_t run(h2_application_host_t *host, h2_runtime_t *app_runtime, void *user) {
    runtime = app_runtime;
    (void)user;
    rtos_delay_milliseconds(5000u);
    int rc;
    for (;;) {
        rc = h2_pal_http_device_run(runtime, &result);
        if (result.passed + result.failed + result.blocked != 0u) break;
        if (rc != H2_PAL_ERR_NOT_FOUND && rc != H2_PAL_ERR_UNAVAILABLE &&
            rc != H2_PAL_ERR_TIMEOUT && rc != H2_PAL_ERR_BUSY) break;
        printf("H2_PAL_HTTP_SETUP_WAIT network=not_ready rc=%d cases_started=0\n", rc);
        fflush(stdout);
        rtos_delay_milliseconds(3000u);
    }
    if (result.passed + result.failed + result.blocked != H2_PAL_HTTP_E2E_CASE_COUNT)
        fail("fixture_or_network", rc);
    int confirm = rc == H2_PAL_OK ? h2_bk_h2loader_confirm_current_app(runtime) : H2_PAL_ERR_INVALID_STATE;
    while (!h2_application_host_should_stop(host)) {
        h2_pal_http_device_report(runtime, &result);
        printf("H2_PAL_HTTP_READY board=bk7258 rc=%d confirm=%d\n", rc, confirm);
        fflush(stdout);
        rtos_delay_milliseconds(5000u);
    }

    return h2_pal_e2e_application_host_finish(host, runtime, rc);
}

static void entry(void *user) {
  (void)user;

  const h2_application_host_config_t app = {.entry = run};
  int host_rc = h2_pal_e2e_application_host_run(
      "pal-http", H2_LOADER_CAPABILITY_UART | H2_LOADER_CAPABILITY_WIFI, &app,
      NULL);
  if (host_rc != H2_PAL_OK)
    fail("application_host", host_rc);
}
int main(void) {
    int rc = h2_bk_target_task_policy_install();
    if (rc != H2_PAL_OK) return -1;
    bk_init();
    rc = h2_bk7258_board_start_entry_task("bk/pal-http", entry, NULL);
    if (rc != H2_PAL_OK) fail("entry", rc);
    return 0;
}
