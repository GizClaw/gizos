#include "h2_pal_e2e_application_host.h"
#include "esp_app_desc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "h2_esp_h2loader_ble.h"
#include "h2_esp_h2loader_runtime.h"
#include "h2_pal_webrtc_device.h"
#include <stdio.h>

static h2_pal_webrtc_e2e_result_t result;

static h2_pal_result_t run(h2_application_host_t *host, h2_runtime_t *runtime, void *user) {
  (void)user;
  vTaskDelay(pdMS_TO_TICKS(5000u));
  int rc = h2_pal_webrtc_device_run(runtime, 20000u, 0u, &result);
  if (result.passed + result.failed + result.blocked !=
      H2_PAL_WEBRTC_E2E_CASE_COUNT)
    h2_pal_e2e_application_host_fail("fixture_or_network", rc);
  int confirm = rc == H2_PAL_OK ? h2_esp_h2loader_app_confirm(runtime)
                                : H2_PAL_ERR_INVALID_STATE;
  while (!h2_application_host_should_stop(host)) {
    h2_pal_webrtc_device_report(runtime, &result);
    printf("H2_PAL_WEBRTC_READY board=devkit rc=%d confirm=%d\n", rc, confirm);
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(5000u));
  }

  return rc;
}

static h2_pal_result_t validate(void *user, h2_runtime_t *runtime) {
  (void)user;
  (void)runtime;
  printf("H2_PAL_WEBRTC_BOOT board=devkit version=%s\n",
         esp_app_get_description()->version);

  return H2_PAL_OK;
}

void app_main(void) {
    const h2_application_host_config_t app = {.entry = run, .validate = validate};
    (void)h2_pal_e2e_application_host_start(
        "pal-webrtc", H2_LOADER_CAPABILITY_UART | H2_LOADER_CAPABILITY_WIFI | H2_LOADER_CAPABILITY_BLE, &app);
}
