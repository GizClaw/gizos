#include "h2_esp_board.h"
#include "h2_esp_h2loader_ble.h"
#include "h2_esp_h2loader_runtime.h"
#include "h2_libco_smoke.h"
#include "h2_esp_layout_task_policy.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

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

void app_main(void) {
    if (h2_esp_layout_task_policy_install() != H2_PAL_OK) {
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
  int result = h2_esp_board_runtime_config(&runtime_config);
  if (result != H2_PAL_OK) {
    h2_libco_smoke_fail("runtime_config", result, false);
  }
  result = h2_esp_h2loader_app_commands_prepare_serial(&runtime_config,
                                                       "libco-smoke", 1u, 3u);
  if (result != H2_PAL_OK) {
    h2_libco_smoke_fail("command_prepare", result, false);
  }
  result = h2_runtime_init(&runtime_config, &runtime);
  if (result != H2_PAL_OK) {
    h2_libco_smoke_fail("runtime_init", result, false);
  }
  result = h2_esp_h2loader_app_commands_start(runtime, "libco-smoke", 1u, 3u);
  if (result != H2_PAL_OK) {
    h2_libco_smoke_fail("command_start", result, false);
  }
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
