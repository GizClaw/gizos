#include "device_runner.h"
#include "esp_memory_utils.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "h2_esp_board.h"
#include "h2_esp_h2loader_ble.h"
#include "h2_esp_h2loader_runtime.h"
#include "h2_esp_platform_core.h"
#include "h2_esp_target_task_policy.h"
#include <stdio.h>
#include <string.h>
static h2_runtime_t *runtime;
static void hold(void) {
  for (;;)
    vTaskDelay(pdMS_TO_TICKS(1000));
}
static void fail(const char *stage, int rc) {
  printf("H2_ATOMIC_FAIL stage=%s rc=%d confirm=not-attempted\n", stage, rc);
  fflush(stdout);
  hold();
}
static int core(void *unused) {
  (void)unused;
  return (int)xPortGetCoreID();
}
static int placement(uintptr_t wrapper, uintptr_t storage, bool is_static,
                     void *user) {
  bool psram = *(bool *)user;
  bool valid = (is_static || !psram ? esp_ptr_internal((void *)wrapper)
                                    : esp_ptr_external_ram((void *)wrapper)) &&
               esp_ptr_internal((void *)storage);
  printf(
      "ATOMIC_PLACEMENT wrapper=%p storage=%p static=%u psram=%u verdict=%s\n",
      (void *)wrapper, (void *)storage, is_static, psram,
      valid ? "PASS" : "FAIL");
  return valid ? 0 : H2_PAL_ERR_INVALID_STATE;
}
static void run(void *unused) {
  (void)unused;
  h2_pal_time_sleep_ms(runtime->time, 1500);
  h2_esp_platform_resource_stats_t before = {0}, after = {0};
  int rc = h2_esp_platform_get_resource_stats(&before);
  if (rc)
    fail("stats_before", rc);
  h2_atomic_qualification_config_t c = {
      .mem = h2_esp_platform_internal_allocator(),
      .task = runtime->task,
      .time = runtime->time,
      .current_core = core,
      .expected_core = {0, 1},
      .require_distinct_workers = true,
      .check_placement = placement};
  rc = h2_atomic_device_run(runtime, H2_ATOMIC_VERSION, &c,
                            h2_esp_platform_psram_allocator());
  if (rc)
    fail("qualification", rc);
  /* Direct C11 is a comparison only in the SDK's supported internal RAM.
   * Xtensa PSRAM is not an atomic backing contract; never execute that probe.
   */
  h2_atomic_e2e_result_t comparison;
  rc = h2_atomic_e2e_run(h2_esp_platform_internal_allocator(), runtime->task,
                         runtime->time, h2_atomic_e2e_c11_backend(), 100000,
                         true, false, core, NULL, NULL, NULL, &comparison);
  printf("H2_ATOMIC_C11_COMPARISON placement=internal expected=%u "
         "incremented=%u compared=%u verdict=%s rc=%d\n",
         comparison.expected, comparison.incremented, comparison.compared,
         rc ? "FAIL" : "PASS", rc);
  if (rc)
    fail("comparison", rc);
  puts("H2_ATOMIC_C11_COMPARISON placement=psram status=SKIP "
       "reason=unsupported-backing");
  h2_atomic_flag_e2e_result_t flags;
  bool flag_psram = true;
  rc = h2_atomic_flag_e2e_run(h2_esp_platform_psram_allocator(), runtime->task,
                              runtime->time, 20000, core, NULL, placement,
                              &flag_psram, &flags);
  bool valid = !rc && flags.worker_core[0] == 0 && flags.worker_core[1] == 0 &&
               flags.static_storage[0] != flags.static_storage[1] &&
               esp_ptr_internal((void *)flags.static_storage[0]) &&
               esp_ptr_internal((void *)flags.static_storage[1]) &&
               esp_ptr_external_ram((void *)flags.dynamic_wrapper) &&
               esp_ptr_internal((void *)flags.dynamic_storage);
  printf("H2_ATOMIC_FLAG_E2E static_a=%p static_b=%p dynamic_wrapper=%p "
         "dynamic_storage=%p cores=%d,%d operations=%u,%u busy=%u,%u "
         "verdict=%s rc=%d\n",
         (void *)flags.static_storage[0], (void *)flags.static_storage[1],
         (void *)flags.dynamic_wrapper, (void *)flags.dynamic_storage,
         flags.worker_core[0], flags.worker_core[1], flags.operations[0],
         flags.operations[1], flags.busy_observations[0],
         flags.busy_observations[1], valid ? "PASS" : "FAIL", rc);
  if (!valid)
    fail("flags", rc ? rc : H2_PAL_ERR_INVALID_STATE);
  h2_pal_time_sleep_ms(runtime->time, 100);
  int stats = h2_esp_platform_get_resource_stats(&after);
  int cleanup = stats ? stats
                : memcmp(&before, &after, sizeof(before))
                    ? H2_PAL_ERR_INVALID_STATE
                    : 0;
  printf("H2_ATOMIC_CLEANUP rc=%d tasks=%zu/%zu allocations=%zu/%zu "
         "bytes=%zu/%zu\n",
         cleanup, before.live_tasks, after.live_tasks, before.allocations,
         after.allocations, before.allocation_bytes, after.allocation_bytes);
  if (cleanup)
    fail("cleanup", cleanup);
  rc = h2_esp_h2loader_app_confirm(runtime);
  if (rc)
    fail("confirm", rc);
  puts("H2_ATOMIC_READY rc=0 confirm=0");
  fflush(stdout);
  for (;;) {
    h2_atomic_device_replay();
    h2_pal_time_sleep_ms(runtime->time, 3000);
  }
}
void app_main(void) {
  int rc = h2_esp_target_task_policy_install();
  if (rc)
    fail("policy", rc);
  h2_runtime_config_t c = {0};
  rc = h2_esp_board_runtime_config(&c);
  if (rc)
    fail("board", rc);
  rc = h2_esp_h2loader_app_commands_prepare_serial(&c, "atomic-e2e", 1, 4);
  if (rc)
    fail("prepare", rc);
  rc = h2_runtime_init(&c, &runtime);
  if (rc)
    fail("runtime", rc);
  rc = h2_esp_h2loader_app_commands_start(runtime, "atomic-e2e", 1, 4);
  if (rc)
    fail("commands", rc);
  const h2_pal_task_options_t options = {.name = "atomic/e2e/runner",
                                         .min_stack_size = 65536};
  h2_pal_task_t *task = NULL;
  rc = h2_pal_task_start(runtime->task, &options, run, NULL, &task);
  if (rc)
    fail("runner", rc);
  hold();
}
