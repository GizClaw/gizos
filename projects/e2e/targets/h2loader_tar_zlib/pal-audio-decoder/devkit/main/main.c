#include "h2_pal_e2e_application_host.h"
#include "device_runner.h"
#include "h2_esp_audio_decoder.h"
#include "esp_app_desc.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "h2_esp_h2loader_ble.h"
#include "h2_esp_h2loader_runtime.h"
#include "h2_esp_platform_core.h"
#include <stdio.h>
static esp_timer_handle_t watchdog;

static void deadline(void *unused) {
  (void)unused;
  puts("H2_ADEC_WATCHDOG timeout=120s");
  fflush(stdout);
}
static h2_pal_result_t run(h2_application_host_t *host, h2_runtime_t *runtime, void *user) {
  (void)user;
  vTaskDelay(pdMS_TO_TICKS(5000));
  int rc = h2_adec_device_run(runtime, esp_app_get_description()->version);
  esp_timer_stop(watchdog);
  esp_timer_delete(watchdog);
  if (rc != H2_PAL_OK) {
    printf("H2_ADEC_QUALIFICATION_FAIL rc=%d confirm=not-attempted\n", rc);
    h2_pal_e2e_application_host_fail("qualification", rc);
  }
  int confirm = h2_esp_h2loader_app_confirm(runtime);
  if (confirm != H2_PAL_OK)
    h2_pal_e2e_application_host_fail("confirm", confirm);
  puts("H2_ADEC_READY rc=0 confirm=0");
  while (!h2_application_host_should_stop(host)) {
    h2_adec_device_replay(runtime);
    vTaskDelay(pdMS_TO_TICKS(3000));
  }

  return rc;
}

static h2_pal_result_t configure(void *user, h2_runtime_config_t *config) {
  (void)user;
  config->audio_decoder = h2_esp_audio_decoder_api();
  config->system_event = h2_pal_unsupported_system_event_api();

  return H2_PAL_OK;
}
static h2_pal_result_t validate(void *user, h2_runtime_t *runtime) {
  (void)user;
  (void)runtime;
  const esp_timer_create_args_t timer = {.callback = deadline,
                                         .name = "adec-watchdog"};
  if (esp_timer_create(&timer, &watchdog) != ESP_OK ||
      esp_timer_start_once(watchdog, 120000000u) != ESP_OK)
    h2_pal_e2e_application_host_fail("watchdog", -4);

  return H2_PAL_OK;
}
void app_main(void) {
    const h2_application_host_config_t app = {
        .entry = run, .configure = configure, .validate = validate};
    (void)h2_pal_e2e_application_host_start(
        "pal-audio-decoder", H2_LOADER_CAPABILITY_UART, &app);
}
