#include "h2_atomic_e2e.h"
#include "h2_esp_board.h"
#include "h2_esp_h2loader_ble.h"
#include "h2_esp_h2loader_runtime.h"
#include "h2_esp_target_task_policy.h"
#include "h2_esp_platform_core.h"
#include "h2_runtime.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <inttypes.h>
#include <stdio.h>

static void hold(void) {
  for (;;) vTaskDelay(pdMS_TO_TICKS(1000u));
}

static void fail(const char *stage, int rc) {
  printf("H2_ATOMIC_E2E_FAIL stage=%s rc=%d\n", stage, rc);
  fflush(stdout);
  hold();
}

void app_main(void) {
  int rc = h2_esp_target_task_policy_install();
  if (rc != H2_PAL_OK) fail("task_policy", rc);
  h2_runtime_config_t config = {0};
  h2_runtime_t *runtime = NULL;
  rc = h2_esp_board_runtime_config(&config);
  if (rc != H2_PAL_OK) fail("runtime_config", rc);
  rc = h2_esp_h2loader_app_commands_prepare_serial(&config,
                                                     "atomic-e2e", 1u, 3u);
  if (rc != H2_PAL_OK) fail("command_prepare", rc);
  rc = h2_runtime_init(&config, &runtime);
  if (rc != H2_PAL_OK) fail("runtime_init", rc);
  rc = h2_esp_h2loader_app_commands_start(runtime, "atomic-e2e", 1u, 3u);
  if (rc != H2_PAL_OK) fail("command_start", rc);

  const h2_atomic_e2e_backend_t *backends[] = {
      h2_atomic_e2e_h2_backend(), h2_atomic_e2e_c11_backend()};
  for (unsigned sample = 0u; sample < 3u; ++sample) {
    for (unsigned i = 0u; i < 2u; ++i) {
      const h2_atomic_e2e_backend_t *backend = backends[(sample + i) % 2u];
      h2_atomic_e2e_result_t result;
      rc = h2_atomic_e2e_run(h2_esp_platform_internal_allocator(),
                             h2_esp_platform_task_api(),
                             h2_esp_platform_time_api(), backend,
                             100000u, true, NULL, NULL, &result);
      printf("H2_ATOMIC_E2E backend=%s sample=%u concurrent=%u expected=%u "
             "incremented=%u compared=%u elapsed_us=%" PRIu64 " rc=%d\n",
             backend->name, sample, (unsigned)result.concurrent,
             result.expected, result.incremented, result.compared,
             result.elapsed_us, rc);
      fflush(stdout);
      if (rc != H2_PAL_OK) fail("workload", rc);
    }
  }
  rc = h2_esp_h2loader_app_confirm(runtime);
  if (rc != H2_PAL_OK) fail("confirm", rc);
  printf("H2_ATOMIC_E2E_READY rc=0\n");
  fflush(stdout);
  hold();
}
