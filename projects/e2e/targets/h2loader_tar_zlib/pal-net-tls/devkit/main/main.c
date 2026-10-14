#include "h2_pal_e2e_application_host.h"
#include "esp_app_desc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "h2_esp_h2loader_ble.h"
#include "h2_esp_h2loader_runtime.h"
#include "h2_pal_net_tls_device.h"
#include <stdio.h>

#ifndef H2_PAL_NET_TLS_BOARD
#define H2_PAL_NET_TLS_BOARD "devkit"
#endif

static h2_net_tls_result_t result;

static h2_pal_result_t run(h2_application_host_t *host, h2_runtime_t *runtime, void *user) {
  (void)user;
  vTaskDelay(pdMS_TO_TICKS(5000u));
  int rc;
  uint64_t setup_start = 0u;
  if (h2_pal_time_get_monotonic_ms(runtime->time, &setup_start) != H2_PAL_OK)
    h2_pal_e2e_application_host_fail("clock", H2_PAL_ERR_IO);
  for (;;) {
    rc = h2_pal_net_tls_device_run(runtime, &result);
    if (result.passed + result.failed + result.blocked + result.unsupported +
            result.not_assessed !=
        0u)
      break;
    if (rc != H2_PAL_ERR_NOT_FOUND && rc != H2_PAL_ERR_UNAVAILABLE &&
        rc != H2_PAL_ERR_TIMEOUT && rc != H2_PAL_ERR_BUSY)
      break;
    uint64_t current = 0u;
    if (h2_pal_time_get_monotonic_ms(runtime->time, &current) != H2_PAL_OK ||
        current - setup_start >= 120000u)
      break;
    printf(
        "H2_PAL_NET_TLS_SETUP_WAIT network=not_ready rc=%d cases_started=0\n",
        rc);
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(3000u));
  }
  if (result.passed + result.failed + result.blocked + result.unsupported +
          result.not_assessed !=
      H2_NET_TLS_CASE_COUNT)
    h2_pal_e2e_application_host_fail("fixture_or_network", rc);
  int confirm = rc == H2_PAL_OK ? h2_esp_h2loader_app_confirm(runtime)
                                : H2_PAL_ERR_INVALID_STATE;
  while (!h2_application_host_should_stop(host)) {
    h2_pal_net_tls_device_report(runtime, &result);
    printf("H2_PAL_NET_TLS_READY board=%s rc=%d confirm=%d\n",
           H2_PAL_NET_TLS_BOARD, rc, confirm);
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(5000u));
  }

  return rc;
}

static h2_pal_result_t validate(void *user, h2_runtime_t *runtime) {
  (void)user;
  (void)runtime;
  printf("H2_PAL_NET_TLS_BOOT board=%s version=%s\n", H2_PAL_NET_TLS_BOARD,
         esp_app_get_description()->version);

  return H2_PAL_OK;
}
void app_main(void) {
    const h2_application_host_config_t app = {.entry = run, .validate = validate};
    (void)h2_pal_e2e_application_host_start(
        "pal-net-tls", H2_LOADER_CAPABILITY_UART | H2_LOADER_CAPABILITY_WIFI | H2_LOADER_CAPABILITY_BLE, &app);
}
