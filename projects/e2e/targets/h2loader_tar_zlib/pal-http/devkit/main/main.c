#include "h2_pal_e2e_application_host.h"
#include "h2_esp_board.h"
#include "h2_esp_h2loader_ble.h"
#include "h2_esp_h2loader_runtime.h"
#include "h2_esp_target_task_policy.h"
#include "h2_pal_http_device.h"
#include "esp_app_desc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>

static h2_runtime_t *runtime;
static h2_pal_http_e2e_result_t result;
static void hold(void) { for (;;) vTaskDelay(pdMS_TO_TICKS(1000u)); }
static void fail(const char *stage, int rc) {
    printf("H2_PAL_HTTP_SETUP_FAIL stage=%s rc=%d\n", stage, rc);
    fflush(stdout);
    hold();
}
static h2_pal_result_t run(h2_application_host_t *host, h2_runtime_t *app_runtime, void *user) {
    runtime = app_runtime;
    (void)user;
    vTaskDelay(pdMS_TO_TICKS(5000u));
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
    int confirm = rc == H2_PAL_OK ? h2_esp_h2loader_app_confirm(runtime) : H2_PAL_ERR_INVALID_STATE;
    while (!h2_application_host_should_stop(host)) {
        h2_pal_http_device_report(runtime, &result);
        printf("H2_PAL_HTTP_READY board=devkit rc=%d confirm=%d\n", rc, confirm);
        fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(5000u));
    }

    return h2_pal_e2e_application_host_finish(host, runtime, rc);
}

static h2_pal_result_t validate(void *user, h2_runtime_t *app_runtime) {
  (void)user;
  runtime = app_runtime;
  printf("H2_PAL_HTTP_BOOT board=devkit version=%s\n",
         esp_app_get_description()->version);

  return H2_PAL_OK;
}
void app_main(void) {

  int rc = h2_esp_target_task_policy_install();
  if (rc != H2_PAL_OK)
    fail("task_policy", rc);
  const h2_application_host_config_t app = {.entry = run, .validate = validate};
  int host_rc = h2_pal_e2e_application_host_run("pal-http",
                                                H2_LOADER_CAPABILITY_UART |
                                                    H2_LOADER_CAPABILITY_WIFI |
                                                    H2_LOADER_CAPABILITY_BLE,
                                                &app, NULL);
  if (host_rc != H2_PAL_OK)
    fail("application_host", host_rc);
}
