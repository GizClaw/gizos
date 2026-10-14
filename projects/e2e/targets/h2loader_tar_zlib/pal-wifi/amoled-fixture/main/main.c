#include "h2_pal_e2e_application_host.h"
#include "esp_app_desc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "h2_esp_h2loader_ble.h"
#include "h2_esp_h2loader_runtime.h"
#include "h2_pal_wifi_device.h"
#include <stdio.h>

static h2_pal_result_t run(h2_application_host_t *host, h2_runtime_t *runtime, void *user) {
    (void)user;
    vTaskDelay(pdMS_TO_TICKS(3000u));
    int rc = h2_wifi_fixture_run(runtime);
    int confirm = rc == H2_PAL_OK ? h2_esp_h2loader_app_confirm(runtime) : H2_PAL_ERR_INVALID_STATE;
    printf("H2_WIFI_READY board=amoled-fixture rc=%d confirm=%d\n", rc, confirm);
    fflush(stdout);
    h2_pal_result_t waited = h2_pal_e2e_application_host_wait(host, runtime);
  return rc == H2_PAL_OK ? waited : rc;
}

static h2_pal_result_t configure(void *user, h2_runtime_config_t *config) {
  (void)user;
  config->event_queue_capacity = 64;

  return H2_PAL_OK;
}
static h2_pal_result_t validate(void *user, h2_runtime_t *runtime) {
  (void)user;
  (void)runtime;
  printf("H2_WIFI_LAUNCHER version=%s board=amoled-fixture\n",
         esp_app_get_description()->version);
  fflush(stdout);

  return H2_PAL_OK;
}
void app_main(void) {
    const h2_application_host_config_t app = {
        .entry = run, .configure = configure, .validate = validate};
    (void)h2_pal_e2e_application_host_start(
        "pal-wifi-fixture", H2_LOADER_CAPABILITY_UART | H2_LOADER_CAPABILITY_WIFI | H2_LOADER_CAPABILITY_BLE, &app);
}
