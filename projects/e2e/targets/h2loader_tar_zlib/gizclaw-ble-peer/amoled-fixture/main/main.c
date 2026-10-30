#include "esp_app_desc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "h2_esp_board.h"
#include "h2_esp_h2loader_ble.h"
#include "h2_esp_h2loader_runtime.h"
#include "h2_esp_target_task_policy.h"
#include "h2_gizclaw_ble_peer.h"
#include <stdio.h>
static h2_runtime_t* runtime;
static void hold(void) {
  for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
}
static void fail(const char* stage, int rc) {
  printf("H2_BLE_PEER_SETUP_FAIL stage=%s rc=%d\n", stage, rc);
  fflush(stdout);
  hold();
}
static void run(void* user) {
  (void)user;
  int rc = h2_gizclaw_ble_peer_run(runtime, h2_ble_peer_dut_uid(),
                                   h2_ble_peer_origin(), 2u);
  int confirm =
      rc ? H2_PAL_ERR_INVALID_STATE : h2_esp_h2loader_app_confirm(runtime);
  printf("H2_BLE_PEER_END transfers=2 rc=%d confirm=%d\n", rc, confirm);
  fflush(stdout);
  hold();
}
void app_main(void) {
  int rc = h2_esp_target_task_policy_install();
  if (rc) fail("task-policy", rc);
  h2_runtime_config_t config = {0};
  rc = h2_esp_board_runtime_config(&config);
  if (rc) fail("board", rc);
  rc = h2_esp_h2loader_app_commands_prepare_serial(&config, "gizclaw-ble-peer",
                                                   1u, 3u);
  if (rc) fail("serial", rc);
  rc = h2_runtime_init(&config, &runtime);
  if (rc) fail("runtime", rc);
  printf("H2_BLE_PEER_BOOT board=amoled version=%s target_uid=%s\n",
         esp_app_get_description()->version, h2_ble_peer_dut_uid());
  fflush(stdout);
  const h2_pal_task_options_t options = {.name = "gizclaw-ble-peer/e2e/runner",
                                         .min_stack_size = 65536};
  h2_pal_task_t* task = NULL;
  rc = h2_pal_task_start(runtime->task, &options, run, NULL, &task);
  if (rc) fail("runner", rc);
}
