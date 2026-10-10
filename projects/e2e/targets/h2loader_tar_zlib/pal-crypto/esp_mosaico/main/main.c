#include "h2_mosaico_loader_usb.h"
#include "device_runner.h"
#include "esp_app_desc.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "h2_esp_board.h"
#include "h2_esp_h2loader_ble.h"
#include "h2_esp_h2loader_runtime.h"
#include "h2_esp_platform_core.h"
#include "h2_esp_target_task_policy.h"
#include <stdio.h>
static h2_runtime_t *runtime;
static esp_timer_handle_t watchdog;
static void fail(const char *stage, int rc) {
  printf("H2_CRYPTO_LAUNCHER_FAIL stage=%s rc=%d\n", stage, rc);
  fflush(stdout);
  for (;;)
    vTaskDelay(pdMS_TO_TICKS(1000));
}
static void deadline(void *unused) {
  (void)unused;
  puts("H2_CRYPTO_WATCHDOG timeout=120s");
  fflush(stdout);
}
static void run(void *unused) {
  (void)unused;
  vTaskDelay(pdMS_TO_TICKS(5000));
  int rc = h2_crypto_device_run(runtime, esp_app_get_description()->version);
  esp_timer_stop(watchdog);
  esp_timer_delete(watchdog);
  int confirm = h2_esp_h2loader_app_confirm(runtime);
  printf("H2_CRYPTO_READY rc=%d confirm=%d\n", rc, confirm);
  for (;;) {
    h2_crypto_device_replay(runtime);
    vTaskDelay(pdMS_TO_TICKS(3000));
  }
}
void app_main(void) {
  int usb_rc = h2_mosaico_loader_usb_init();
  if (usb_rc != H2_PAL_OK) {
    printf("H2_MOSAICO_USB_FAIL rc=%d\n", usb_rc);
    return;
  }
  int rc = h2_esp_target_task_policy_install();
  if (rc)
    fail("task-policy", rc);
  h2_runtime_config_t config = {0};
  rc = h2_esp_board_runtime_config(&config);
  if (rc)
    fail("board", rc);
  config.system_event = h2_pal_unsupported_system_event_api();
  const h2_esp_h2loader_app_commands_config_t commands = {
      .active_name = "pal-crypto",
      .hardware_capabilities = H2_LOADER_CAPABILITY_UART,
      .h2loader_partition_id = 1,
      .coredump_partition_id = 3};
  rc = h2_esp_h2loader_app_commands_prepare_serial_with_config(&config,
                                                               &commands);
  if (rc)
    fail("commands", rc);
  rc = h2_runtime_init(&config, &runtime);
  if (rc)
    fail("runtime", rc);
  const esp_timer_create_args_t timer = {.callback = deadline,
                                         .name = "crypto-watchdog"};
  if (esp_timer_create(&timer, &watchdog) != ESP_OK ||
      esp_timer_start_once(watchdog, 120000000u) != ESP_OK)
    fail("watchdog", -4);
  const h2_pal_task_options_t options = {.name = "pal-crypto/e2e/runner",
                                         .min_stack_size = 65536};
  h2_pal_task_t *task = NULL;
  rc = h2_pal_task_start(runtime->task, &options, run, NULL, &task);
  if (rc)
    fail("runner", rc);
}
