#include "h2_pal_e2e_application_host.h"
#include "h2_esp_board.h"
#include "h2_esp_h2loader_runtime.h"
#include "h2_esp_h2loader_ble.h"
#include "h2_esp_platform_core.h"
#include "h2_esp_target_task_policy.h"
#include "device_runner.h"
#include "esp_app_desc.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
static h2_runtime_t *runtime;
static esp_timer_handle_t watchdog;
static void fail(const char *stage,int rc) {printf("H2_STORAGE_LAUNCHER_FAIL stage=%s rc=%d\n",stage,rc);fflush(stdout);for(;;)vTaskDelay(pdMS_TO_TICKS(1000));}
static void deadline(void *unused) {(void)unused;puts("H2_STORAGE_WATCHDOG timeout=120s");fflush(stdout);}
static h2_pal_result_t run(h2_application_host_t *host, h2_runtime_t *app_runtime, void *user) {
    runtime = app_runtime;
  (void)user;vTaskDelay(pdMS_TO_TICKS(5000));
  int rc=h2_storage_device_run(runtime,"/data/pal-storage",esp_app_get_description()->version);
  esp_timer_stop(watchdog);esp_timer_delete(watchdog);
  int confirm=h2_esp_h2loader_app_confirm(runtime);
  printf("H2_STORAGE_READY rc=%d confirm=%d\n",rc,confirm);
  while (!h2_application_host_should_stop(host)) {h2_storage_device_replay(runtime);vTaskDelay(pdMS_TO_TICKS(3000));}

    return h2_pal_e2e_application_host_finish(host, runtime, rc);
}

static h2_pal_result_t configure(void *user, h2_runtime_config_t *config) {
  (void)user;
  config->system_event = h2_pal_unsupported_system_event_api();

  return H2_PAL_OK;
}
static h2_pal_result_t validate(void *user, h2_runtime_t *app_runtime) {
  (void)user;
  runtime = app_runtime;
  const esp_timer_create_args_t timer = {.callback = deadline,
                                         .name = "storage-watchdog"};
  if (esp_timer_create(&timer, &watchdog) != ESP_OK ||
      esp_timer_start_once(watchdog, 120000000u) != ESP_OK)
    fail("watchdog", -4);

  return H2_PAL_OK;
}
void app_main(void) {

  int rc = h2_esp_target_task_policy_install();
  if (rc)
    fail("task-policy", rc);
  const h2_application_host_config_t app = {
      .entry = run, .configure = configure, .validate = validate};
  int host_rc = h2_pal_e2e_application_host_run(
      "pal-storage", H2_LOADER_CAPABILITY_UART, &app, NULL);
  if (host_rc != H2_PAL_OK)
    fail("application_host", host_rc);
}
