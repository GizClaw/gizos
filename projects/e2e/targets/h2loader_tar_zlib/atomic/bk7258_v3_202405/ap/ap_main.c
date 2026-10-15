#include "bk_private/bk_init.h"
#include "device_runner.h"
#include "h2_bk7258_board.h"
#include "h2_bk_h2loader.h"
#include "h2_bk_platform_core.h"
#include "h2_bk_target_task_policy.h"
#include <os/os.h>
#include <stdio.h>
#include <string.h>
static h2_runtime_t *runtime;
static bool commands_started;
static int start_commands(void) {
  if (commands_started)
    return H2_PAL_OK;
  int rc = h2_bk_h2loader_start_app_iostreamikcp_with_capabilities(
      runtime, "atomic", H2_LOADER_CAPABILITY_UART);
  if (!rc)
    commands_started = true;
  return rc;
}
static void hold(void) {
  for (;;)
    rtos_delay_milliseconds(1000);
}
static void fail(const char *stage, int rc) {
  printf("H2_ATOMIC_FAIL stage=%s rc=%d confirm=not-attempted\n", stage, rc);
  if (runtime && !commands_started)
    (void)start_commands();
  hold();
}
static int core(void *unused) {
  (void)unused;
  return (int)rtos_get_core_id() - 1;
}
static bool external_address(uintptr_t p) {
  return p >= 0x60000000u && p < 0x60800000u;
}
static bool internal_address(uintptr_t p) {
  return p >= 0x28000000u && p < 0x280a0000u;
}
static int placement(uintptr_t wrapper, uintptr_t storage, bool is_static,
                     void *user) {
  bool psram = *(bool *)user;
  bool valid = (is_static || !psram ? internal_address(wrapper)
                                    : external_address(wrapper)) &&
               internal_address(storage);
  printf(
      "ATOMIC_PLACEMENT wrapper=%p storage=%p static=%u psram=%u verdict=%s\n",
      (void *)wrapper, (void *)storage, is_static, psram,
      valid ? "PASS" : "FAIL");
  return valid ? 0 : H2_PAL_ERR_INVALID_STATE;
}
static void run(void *unused) {
  (void)unused;
  rtos_delay_milliseconds(1500);
  h2_bk_platform_resource_stats_t before = {0}, after = {0};
  int rc = h2_bk_platform_get_resource_stats(&before);
  if (rc)
    fail("stats_before", rc);
  h2_atomic_qualification_config_t c = {.mem = h2_bk_platform_sram_allocator(),
                                        .task = runtime->task,
                                        .time = runtime->time,
                                        .current_core = core,
                                        .expected_core = {0, 1},
                                        .require_distinct_workers = true,
                                        .check_placement = placement};
  static h2_pal_firmware_info_t firmware;
  rc = h2_pal_firmware_info_get_current(runtime->firmware_info, &firmware);
  if (rc)
    fail("firmware_info", rc);
  rc = h2_atomic_device_run(runtime, firmware.version, &c,
                            h2_bk_platform_psram_allocator());
  int stats = h2_bk_platform_get_resource_stats(&after);
  int cleanup = stats ? stats
                : memcmp(&before, &after, sizeof(before))
                    ? H2_PAL_ERR_INVALID_STATE
                    : 0;
  printf("H2_ATOMIC_CLEANUP rc=%d tasks=%zu/%zu allocations=%zu/%zu "
         "bytes=%zu/%zu\n",
         cleanup, before.live_tasks, after.live_tasks, before.allocations,
         after.allocations, before.allocation_bytes, after.allocation_bytes);
  if (rc || cleanup)
    fail("qualification", rc ? rc : cleanup);
  rc = start_commands();
  if (rc)
    fail("commands", rc);
  int confirm = h2_bk_h2loader_confirm_current_app(runtime);
  if (confirm)
    fail("confirm", confirm);
  h2_atomic_device_admit(cleanup, confirm);
  puts("H2_ATOMIC_READY rc=0 confirm=0");
  for (;;) {
    h2_atomic_device_replay();
    rtos_delay_milliseconds(3000);
  }
}
static void entry(void *unused) {
  (void)unused;
  h2_runtime_config_t c;
  int rc = h2_bk7258_board_runtime_config(&c);
  if (rc)
    fail("board", rc);
  c.system_event = h2_pal_unsupported_system_event_api();
  rc = h2_runtime_init(&c, &runtime);
  if (rc)
    fail("runtime", rc);
  const h2_pal_task_options_t options = {.name = "atomic/e2e/runner",
                                         .min_stack_size = 65536};
  h2_pal_task_t *task = NULL;
  rc = h2_pal_task_start(runtime->task, &options, run, NULL, &task);
  if (rc)
    fail("runner", rc);
  hold();
}
int main(void) {
  int rc = h2_bk_target_task_policy_install();
  if (rc)
    return -1;
  bk_init();
  rc = h2_bk7258_board_start_entry_task("bk/atomic", entry, NULL);
  if (rc)
    fail("entry", rc);
  return 0;
}
