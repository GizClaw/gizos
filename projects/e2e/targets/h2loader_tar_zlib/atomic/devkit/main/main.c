#include "h2_atomic_e2e.h"
#include "h2_esp_board.h"
#include "h2_esp_h2loader_ble.h"
#include "h2_esp_h2loader_runtime.h"
#include "h2_esp_target_task_policy.h"
#include "h2_esp_platform_core.h"
#include "h2_runtime.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_memory_utils.h"

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

static int current_core(void *user) {
  (void)user;
  return (int)xPortGetCoreID();
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
  /* Let a USB monitor attach and drain boot logs before the short workload. */
  (void)h2_pal_time_sleep_ms(h2_esp_platform_time_api(), 1500u);
  unsigned failures = 0u;
  h2_atomic_flag_e2e_result_t flag_result;
  rc = h2_atomic_flag_e2e_run(h2_esp_platform_psram_allocator(),
                              h2_esp_platform_task_api(),
                              h2_esp_platform_time_api(), 20000u,
                              current_core, NULL, &flag_result);
  const bool static_ok = flag_result.static_storage[0] !=
                             flag_result.static_storage[1] &&
                         esp_ptr_internal((const void *)flag_result.static_storage[0]) &&
                         esp_ptr_internal((const void *)flag_result.static_storage[1]);
  const bool dynamic_ok =
      esp_ptr_external_ram((const void *)flag_result.dynamic_wrapper) &&
      esp_ptr_internal((const void *)flag_result.dynamic_storage);
  const bool flag_passed = rc == H2_PAL_OK && static_ok && dynamic_ok &&
                           flag_result.worker_core[0] == 0 &&
                           flag_result.worker_core[1] == 0;
  printf("H2_ATOMIC_FLAG_E2E static_a=%p static_b=%p dynamic_wrapper=%p "
         "dynamic_storage=%p static_ok=%u dynamic_ok=%u cores=%d,%d "
         "operations=%u,%u busy=%u,%u verdict=%s rc=%d\n",
         (void *)flag_result.static_storage[0],
         (void *)flag_result.static_storage[1],
         (void *)flag_result.dynamic_wrapper,
         (void *)flag_result.dynamic_storage, (unsigned)static_ok,
         (unsigned)dynamic_ok, flag_result.worker_core[0],
         flag_result.worker_core[1], flag_result.operations[0],
         flag_result.operations[1], flag_result.busy_observations[0],
         flag_result.busy_observations[1], flag_passed ? "PASS" : "FAIL", rc);
  fflush(stdout);
  if (!flag_passed) ++failures;
  for (unsigned placement = 0u; placement < 2u; ++placement) {
    const bool psram = placement == 1u;
    const h2_pal_mem_api_t *mem = psram
        ? h2_esp_platform_psram_allocator()
        : h2_esp_platform_internal_allocator();
    for (unsigned sample = 0u; sample < 3u; ++sample) {
      for (unsigned i = 0u; i < 2u; ++i) {
        const h2_atomic_e2e_backend_t *backend = backends[(sample + i) % 2u];
        h2_atomic_e2e_result_t result;
        rc = h2_atomic_e2e_run(mem, h2_esp_platform_task_api(),
                               h2_esp_platform_time_api(), backend,
                               psram ? 20000u : 100000u, true, psram,
                               current_core, NULL, NULL, NULL, &result);
        bool wrapper_ok = psram
            ? esp_ptr_external_ram((const void *)result.wrapper_address)
            : esp_ptr_internal((const void *)result.wrapper_address);
        bool storage_ok = psram && backend == backends[1]
            ? esp_ptr_external_ram((const void *)result.storage_address)
            : esp_ptr_internal((const void *)result.storage_address);
        if (result.wrapper_address == 0u || result.storage_address == 0u) {
          wrapper_ok = false;
          storage_ok = false;
        }
        const bool passed = rc == H2_PAL_OK && wrapper_ok && storage_ok;
        printf("H2_ATOMIC_E2E backend=%s placement=%s sample=%u "
               "core0=%d core1=%d expected=%u incremented=%u compared=%u "
               "cas_failures=%u wrapper=%p storage=%p wrapper_ok=%u "
               "storage_ok=%u elapsed_us=%" PRIu64 " verdict=%s rc=%d\n",
               backend->name, psram ? "psram" : "internal", sample,
               result.worker_core[0], result.worker_core[1],
               result.expected, result.incremented, result.compared,
               result.cas_failures, (void *)result.wrapper_address,
               (void *)result.storage_address, (unsigned)wrapper_ok,
               (unsigned)storage_ok, result.elapsed_us,
               passed ? "PASS" : "FAIL", rc);
        fflush(stdout);
        (void)h2_pal_time_sleep_ms(h2_esp_platform_time_api(), 100u);
        if (!passed) ++failures;
      }
    }
  }
  rc = h2_esp_h2loader_app_confirm(runtime);
  if (rc != H2_PAL_OK) fail("confirm", rc);
  printf("H2_ATOMIC_E2E_READY aggregate_failures=%u rc=0\n", failures);
  fflush(stdout);
  hold();
}
