#include "esp_app_desc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "h2_esp_board.h"
#include "h2_esp_h2loader_ble.h"
#include "h2_esp_h2loader_runtime.h"
#include "h2_esp_target_task_policy.h"
#include "h2_pal_webrtc_device.h"
#include <stdio.h>

static h2_runtime_t *runtime;
static h2_pal_webrtc_e2e_result_t result;
static void hold(void) {
  for (;;)
    vTaskDelay(pdMS_TO_TICKS(1000u));
}
static void fail(const char *stage, int rc) {
  printf("H2_PAL_WEBRTC_SETUP_FAIL stage=%s rc=%d\n", stage, rc);
  fflush(stdout);
  hold();
}
static void run(void *user) {
  (void)user;
  vTaskDelay(pdMS_TO_TICKS(5000u));
  int rc = h2_pal_webrtc_device_run(runtime, &result);
  if (result.passed + result.failed + result.blocked !=
      H2_PAL_WEBRTC_E2E_CASE_COUNT)
    fail("fixture_or_network", rc);
  int confirm = rc == H2_PAL_OK ? h2_esp_h2loader_app_confirm(runtime)
                                : H2_PAL_ERR_INVALID_STATE;
  for (;;) {
    h2_pal_webrtc_device_report(runtime, &result);
    printf("H2_PAL_WEBRTC_READY board=devkit rc=%d confirm=%d\n", rc, confirm);
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(5000u));
  }
}
static void entry(void *user) {
  (void)user;
  int rc;
  h2_runtime_config_t config = {0};
  rc = h2_esp_board_runtime_config(&config);
  if (rc != H2_PAL_OK)
    fail("board", rc);
  rc = h2_esp_h2loader_app_commands_prepare_serial(&config, "pal-webrtc", 1u,
                                                   3u);
  if (rc != H2_PAL_OK)
    fail("command_prepare", rc);
  rc = h2_runtime_init(&config, &runtime);
  if (rc != H2_PAL_OK)
    fail("runtime", rc);
  printf("H2_PAL_WEBRTC_BOOT board=devkit version=%s\n",
         esp_app_get_description()->version);
  const h2_pal_task_options_t options = {
      .name = h2_pal_webrtc_device_runner_task_name};
  h2_pal_task_t *runner = NULL;
  rc = h2_pal_task_start(runtime->task, &options, run, NULL, &runner);
  if (rc != H2_PAL_OK)
    fail("runner", rc);
  hold();
}
void app_main(void) {
  int rc = h2_esp_target_task_policy_install();
  if (rc != H2_PAL_OK)
    fail("task_policy", rc);
  rc = h2_esp_board_start_entry_task("devkit/pal-webrtc", entry, NULL);
  if (rc != H2_PAL_OK)
    fail("entry", rc);
}
