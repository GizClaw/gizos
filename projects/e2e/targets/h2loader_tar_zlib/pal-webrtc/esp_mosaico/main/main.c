#include "h2_mosaico_loader_usb.h"
#include "esp_app_desc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "esp_heap_caps.h"
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
  int rc = h2_pal_webrtc_device_run(runtime, 20000u, 0u, &result);
  if (result.passed + result.failed + result.blocked !=
      H2_PAL_WEBRTC_E2E_CASE_COUNT)
    fail("fixture_or_network", rc);
  int confirm = rc == H2_PAL_OK ? h2_esp_h2loader_app_confirm(runtime)
                                : H2_PAL_ERR_INVALID_STATE;
  for (;;) {
    h2_pal_webrtc_device_report(runtime, &result);
    printf("H2_PAL_WEBRTC_READY board=esp_mosaico rc=%d confirm=%d\n", rc, confirm);
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
  printf("H2_PAL_WEBRTC_BOOT board=esp_mosaico version=%s\n",
         esp_app_get_description()->version);
  fflush(stdout);
  const h2_pal_task_options_t options = {
      .name = h2_pal_webrtc_device_runner_task_name};
  h2_pal_task_t *runner = NULL;
  rc = h2_pal_task_start(runtime->task, &options, run, NULL, &runner);
  if (rc != H2_PAL_OK)
    fail("runner", rc);
  hold();
}
/* Artifact-owned startup stack policy; no board-owned application task. */
static void entry_task(void *unused) {
  (void)unused;
  entry(NULL);
  (void)h2_esp_board_runtime_deinit();
  vTaskDeleteWithCaps(NULL);
}
void app_main(void) {
  int usb_rc = h2_mosaico_loader_usb_init();
  if (usb_rc != H2_PAL_OK) {
    printf("H2_MOSAICO_USB_FAIL rc=%d\n", usb_rc);
    return;
  }
  /* Keep the initial executing BOOT observable after CDC enumeration. */
  vTaskDelay(pdMS_TO_TICKS(3000u));
  int rc = h2_esp_target_task_policy_install();
  if (rc != H2_PAL_OK)
    fail("task_policy", rc);
  rc = (xTaskCreatePinnedToCoreWithCaps(entry_task, "esp_mosaico/pal-webrtc", 65536u, NULL,
          tskIDLE_PRIORITY + 4u, NULL, tskNO_AFFINITY, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
       == pdPASS ? H2_PAL_OK : H2_PAL_ERR_TASK);
  if (rc != H2_PAL_OK)
    fail("entry", rc);
}
