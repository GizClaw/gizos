#include "h2_mosaico_loader_usb.h"
#include "h2_esp_board.h"
#include "h2_esp_h2loader_ble.h"
#include "h2_esp_h2loader_runtime.h"
#include "h2_libco_smoke.h"
#include "h2_esp_target_task_policy.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "esp_heap_caps.h"

#include "esp_system.h"

#include <stdbool.h>
#include <stdio.h>

static void h2_libco_smoke_hold(void) {
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(1000u));
  }
}

static void h2_libco_smoke_fail(const char *stage, int result,
                                bool command_transport_started) {
  printf("H2_ESP_LIBCO_SMOKE_FAIL stage=%s rc=%d\n", stage, result);
  fflush(stdout);
  if (!command_transport_started) {
    esp_restart();
  }
  h2_libco_smoke_hold();
}

static void entry(void *unused) {
  (void)unused;
  /* Preserve the first executing phase ledger after USB enumeration. */
  vTaskDelay(pdMS_TO_TICKS(3000u));
  h2_runtime_config_t runtime_config = {0};
  h2_runtime_t *runtime = NULL;
  BaseType_t task_core = xTaskGetCoreID(NULL);
  BaseType_t current_core = xPortGetCoreID();
  printf("H2_ESP_LIBCO_ROOT task=%s task_core=%d current_core=%d "
         "stack=%u iterations=%u\n",
         pcTaskGetName(NULL), (int)task_core, (int)current_core,
         (unsigned int)H2_LIBCO_SMOKE_DEFAULT_STACK_SIZE,
         (unsigned int)H2_LIBCO_SMOKE_DEFAULT_SWITCH_ITERATIONS);
  fflush(stdout);
  if (task_core != current_core || task_core == tskNO_AFFINITY) {
    h2_libco_smoke_fail("root_core", H2_PAL_ERR_INVALID_STATE, false);
  }
  puts("H2_ESP_LIBCO_START stage=board_config");
  fflush(stdout);
  int result = h2_esp_board_runtime_config(&runtime_config);
  if (result != H2_PAL_OK) {
    h2_libco_smoke_fail("runtime_config", result, false);
  }
  puts("H2_ESP_LIBCO_START stage=command_prepare");
  fflush(stdout);
  /* Preparing serial already starts the Type-C management channel. */
  const h2_esp_h2loader_app_commands_config_t commands = {
      .active_name = "libco-smoke",
      .hardware_capabilities = H2_LOADER_CAPABILITY_UART,
      .h2loader_partition_id = 1u,
      .coredump_partition_id = 3u};
  result = h2_esp_h2loader_app_commands_prepare_serial_with_config(
      &runtime_config, &commands);
  if (result != H2_PAL_OK) {
    h2_libco_smoke_fail("command_prepare", result, false);
  }
  puts("H2_ESP_LIBCO_START stage=runtime_init");
  fflush(stdout);
  result = h2_runtime_init(&runtime_config, &runtime);
  if (result != H2_PAL_OK) {
    h2_libco_smoke_fail("runtime_init", result, true);
  }
  puts("H2_ESP_LIBCO_START stage=portable");
  fflush(stdout);
  result = h2_libco_smoke_run(runtime, NULL);
  if (result != H2_PAL_OK) {
    h2_libco_smoke_fail("portable", result, true);
  }
  if (xTaskGetCoreID(NULL) != task_core || xPortGetCoreID() != current_core) {
    h2_libco_smoke_fail("root_core_changed", H2_PAL_ERR_INVALID_STATE, true);
  }
  result = h2_esp_h2loader_app_confirm(runtime);
  if (result != H2_PAL_OK) {
    h2_libco_smoke_fail("confirm", result, true);
  }
  printf("H2_ESP_LIBCO_SMOKE_READY rc=0\n");
  fflush(stdout);
  h2_libco_smoke_hold();
}

/* An internal CPU0-affine native entry also covers direct SDK board startup.
 * Portable libco still owns only its independent 8 KiB coroutine stacks. */
void app_main(void) {
  int rc = h2_mosaico_loader_usb_init();
  if (rc != H2_PAL_OK) {
    printf("H2_MOSAICO_USB_FAIL rc=%d\n", rc);
    return;
  }
  rc = h2_esp_target_task_policy_install();
  if (rc != H2_PAL_OK)
    h2_libco_smoke_fail("task-policy", rc, false);
  if (xTaskCreatePinnedToCoreWithCaps(
          entry, "mosaico/libco", 65536u, NULL, tskIDLE_PRIORITY + 4u,
          NULL, 0, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) != pdPASS)
    h2_libco_smoke_fail("entry-task", H2_PAL_ERR_TASK, false);
}
