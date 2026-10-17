#include "confirmation.h"
#include "result_console.h"

#ifdef H2_LIBCO_SMOKE_HOST_TEST
#include "h2_libco_smoke_test_sdk.h"
#else
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
#endif

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

_Static_assert(sizeof(StackType_t) == 1u,
               "ESP-IDF stack high-water marks must use bytes");

static h2_esp_e2e_result_log_t result_log;

typedef struct h2_libco_smoke_checkpoint {
  bool heap_valid;
  size_t stack_free;
  UBaseType_t stack_min_free;
  int result;
} h2_libco_smoke_checkpoint_t;

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

static void h2_libco_smoke_sample(h2_libco_smoke_checkpoint_t *sample) {
  const uintptr_t stack_start = (uintptr_t)xTaskGetStackStart(NULL);
  const uintptr_t stack_pointer = (uintptr_t)esp_cpu_get_sp();
  sample->heap_valid = heap_caps_check_integrity_all(true);
  sample->stack_min_free = uxTaskGetStackHighWaterMark(NULL);
  sample->stack_free = stack_pointer > stack_start && stack_start != 0u
                           ? stack_pointer - stack_start
                           : 0u;
  sample->result = sample->heap_valid && sample->stack_free != 0u &&
                           sample->stack_min_free != 0u
                       ? H2_PAL_OK
                       : H2_PAL_ERR_INVALID_STATE;
}

static bool h2_libco_smoke_report_checkpoint(
    const char *stage, const h2_libco_smoke_checkpoint_t *sample) {
  const int printed =
      printf("H2_ESP_LIBCO_CHECKPOINT stage=%s heap_valid=%u "
             "main_stack_free=%zu main_stack_min_free=%u rc=%d\n",
             stage, (unsigned)sample->heap_valid, sample->stack_free,
             (unsigned)sample->stack_min_free, sample->result);
  const int flushed = fflush(stdout);
  return sample->result == H2_PAL_OK && printed >= 0 && flushed == 0;
}

static bool h2_libco_smoke_checkpoint(const char *stage) {
  static h2_libco_smoke_checkpoint_t sample;
  h2_libco_smoke_sample(&sample);
  return h2_libco_smoke_report_checkpoint(stage, &sample);
}

static bool h2_libco_smoke_root(BaseType_t task_core, BaseType_t current_core) {
  const int printed =
      printf("H2_ESP_LIBCO_ROOT task=main task_core=%d current_core=%d "
             "stack=%u iterations=%u\n",
             (int)task_core, (int)current_core,
             (unsigned int)H2_LIBCO_SMOKE_DEFAULT_STACK_SIZE,
             (unsigned int)H2_LIBCO_SMOKE_DEFAULT_SWITCH_ITERATIONS);
  const int flushed = fflush(stdout);
  return printed >= 0 && flushed == 0;
}

void app_main(void) {
  if (h2_esp_target_task_policy_install() != H2_PAL_OK) {
    return;
  }
  h2_runtime_config_t runtime_config = {0};
  h2_runtime_t *runtime = NULL;
  BaseType_t task_core = xTaskGetCoreID(NULL);
  BaseType_t current_core = xPortGetCoreID();
  if (task_core != current_core || task_core == tskNO_AFFINITY) {
    (void)h2_libco_smoke_root(task_core, current_core);
    h2_libco_smoke_fail("root_core", H2_PAL_ERR_INVALID_STATE, false);
  }
  /* Sample at the original boundaries. Delivery waits for serial recovery;
   * the delayed boot/runtime records retain these actual observations. */
  static h2_libco_smoke_checkpoint_t boot_sample, runtime_sample;
  h2_libco_smoke_sample(&boot_sample);
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
  h2_libco_smoke_sample(&runtime_sample);
  vTaskDelay(pdMS_TO_TICKS(5000u));
  if (!h2_libco_smoke_root(task_core, current_core)) {
    h2_libco_smoke_fail("root_output", H2_PAL_ERR_WRITE, true);
  }
  const char *startup_checkpoint_failure =
      h2_libco_smoke_report_checkpoint("boot", &boot_sample)
          ? NULL : "boot_checkpoint";
  if (!h2_libco_smoke_report_checkpoint("runtime", &runtime_sample) &&
      startup_checkpoint_failure == NULL) {
    startup_checkpoint_failure = "runtime_checkpoint";
  }
  /* Keep startup checkpoint failures until the recovery transport is live. */
  if (startup_checkpoint_failure != NULL) {
    h2_libco_smoke_fail(startup_checkpoint_failure,
                        H2_PAL_ERR_INVALID_STATE, true);
  }
  result = h2_esp_e2e_result_log_prepare(&result_log, runtime,
                                        "libco-smoke", NULL);
  if (result != H2_PAL_OK) {
    h2_libco_smoke_fail("result_console", result, true);
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
  const size_t records = h2_esp_e2e_result_log_record_count(&result_log);
  result = h2_esp_e2e_result_log_error(&result_log);
  if (result == H2_PAL_OK && records != 7u) {
    result = H2_PAL_ERR_INVALID_STATE;
  }
  if (result != H2_PAL_OK) {
    h2_libco_smoke_fail("result_output", result, true);
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
  const int ready_printed = printf("H2_ESP_LIBCO_SMOKE_READY rc=0\n");
  const int ready_flushed = fflush(stdout);
  if (ready_printed < 0 || ready_flushed != 0) {
    h2_libco_smoke_fail("ready_output", H2_PAL_ERR_WRITE, true);
  }
  h2_libco_smoke_hold();
}
