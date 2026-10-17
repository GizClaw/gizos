#include "confirmation.h"

#include "h2_esp_board.h"
#include "h2_esp_h2loader_ble.h"
#include "h2_esp_h2loader_runtime.h"
#include "h2_libco_smoke.h"
#include "h2_esp_target_task_policy.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_cpu.h"
#include "esp_heap_caps.h"
#include "esp_system.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

_Static_assert(sizeof(StackType_t) == 1u,
               "ESP-IDF stack high-water marks must use bytes");

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

static bool h2_libco_smoke_checkpoint(const char *stage) {
  const uintptr_t stack_start = (uintptr_t)xTaskGetStackStart(NULL);
  const uintptr_t stack_pointer = (uintptr_t)esp_cpu_get_sp();
  const bool heap_valid = heap_caps_check_integrity_all(true);
  const UBaseType_t stack_min_free = uxTaskGetStackHighWaterMark(NULL);
  const size_t stack_free = stack_pointer > stack_start && stack_start != 0u
                                ? stack_pointer - stack_start
                                : 0u;
  const int result = heap_valid && stack_free != 0u && stack_min_free != 0u
                         ? H2_PAL_OK
                         : H2_PAL_ERR_INVALID_STATE;
  const int printed =
      printf("H2_ESP_LIBCO_CHECKPOINT stage=%s heap_valid=%u "
             "main_stack_free=%zu main_stack_min_free=%u rc=%d\n",
             stage, (unsigned)heap_valid, stack_free,
             (unsigned)stack_min_free, result);
  const int flushed = fflush(stdout);
  return result == H2_PAL_OK && printed >= 0 && flushed == 0;
}

void app_main(void) {
  if (h2_esp_target_task_policy_install() != H2_PAL_OK) {
    return;
  }
  h2_runtime_config_t runtime_config = {0};
  h2_runtime_t *runtime = NULL;
  BaseType_t task_core = xTaskGetCoreID(NULL);
  BaseType_t current_core = xPortGetCoreID();
  printf("H2_ESP_LIBCO_ROOT task=main task_core=%d current_core=%d "
         "stack=%u iterations=%u\n",
         (int)task_core, (int)current_core,
         (unsigned int)H2_LIBCO_SMOKE_DEFAULT_STACK_SIZE,
         (unsigned int)H2_LIBCO_SMOKE_DEFAULT_SWITCH_ITERATIONS);
  fflush(stdout);
  if (task_core != current_core || task_core == tskNO_AFFINITY) {
    h2_libco_smoke_fail("root_core", H2_PAL_ERR_INVALID_STATE, false);
  }
  const char *startup_checkpoint_failure =
      h2_libco_smoke_checkpoint("boot") ? NULL : "boot_checkpoint";
  int result = h2_esp_board_runtime_config(&runtime_config);
  if (result != H2_PAL_OK) {
    h2_libco_smoke_fail("runtime_config", result, false);
  }
  runtime_config.system_event = h2_pal_unsupported_system_event_api();
  const h2_esp_h2loader_app_commands_config_t commands = {
      .active_name = "libco-smoke",
      .hardware_capabilities = H2_LOADER_CAPABILITY_UART,
      .h2loader_partition_id = 1u,
      .coredump_partition_id = 3u,
  };
  result = h2_esp_h2loader_app_commands_prepare_serial_with_config(
      &runtime_config, &commands);
  if (result != H2_PAL_OK) {
    h2_libco_smoke_fail("command_prepare", result, false);
  }
  result = h2_runtime_init(&runtime_config, &runtime);
  if (result != H2_PAL_OK) {
    h2_libco_smoke_fail("runtime_init", result, true);
  }
  if (!h2_libco_smoke_checkpoint("runtime") &&
      startup_checkpoint_failure == NULL) {
    startup_checkpoint_failure = "runtime_checkpoint";
  }
  /* Keep startup checkpoint failures until the recovery transport is live. */
  if (startup_checkpoint_failure != NULL) {
    h2_libco_smoke_fail(startup_checkpoint_failure,
                        H2_PAL_ERR_INVALID_STATE, true);
  }
  if (!h2_libco_smoke_checkpoint("before_libco")) {
    h2_libco_smoke_fail("before_libco_checkpoint", H2_PAL_ERR_INVALID_STATE,
                        true);
  }
  result = h2_libco_smoke_run(runtime, NULL);
  if (!h2_libco_smoke_checkpoint("after_libco")) {
    h2_libco_smoke_fail("after_libco_checkpoint", H2_PAL_ERR_INVALID_STATE,
                        true);
  }
  if (result != H2_PAL_OK) {
    h2_libco_smoke_fail("portable", result, true);
  }
  if (xTaskGetCoreID(NULL) != task_core || xPortGetCoreID() != current_core) {
    h2_libco_smoke_fail("root_core_changed", H2_PAL_ERR_INVALID_STATE, true);
  }
  if (!h2_libco_smoke_checkpoint("before_confirm")) {
    h2_libco_smoke_fail("before_confirm_checkpoint", H2_PAL_ERR_INVALID_STATE,
                        true);
  }
  result = h2_libco_smoke_confirm(
      runtime, h2_esp_h2loader_app_commands_get_config,
      h2_esp_h2loader_app_confirm);
  const int confirm_printed = printf("H2_ESP_LIBCO_CONFIRM rc=%d\n", result);
  const int confirm_flushed = fflush(stdout);
  const bool after_confirm_healthy =
      h2_libco_smoke_checkpoint("after_confirm");
  if (result != H2_PAL_OK) {
    h2_libco_smoke_fail("confirm", result, true);
  }
  if (!after_confirm_healthy) {
    h2_libco_smoke_fail("after_confirm_checkpoint", H2_PAL_ERR_INVALID_STATE,
                        true);
  }
  if (confirm_printed < 0 || confirm_flushed != 0) {
    h2_libco_smoke_fail("confirm_output", H2_PAL_ERR_WRITE, true);
  }
  printf("H2_ESP_LIBCO_SMOKE_READY rc=0\n");
  fflush(stdout);
  h2_libco_smoke_hold();
}
