#include "bk_private/bk_init.h"
#include "h2_bk7258_board.h"
#include "h2_bk_h2loader.h"
#include "h2_bk_platform_core.h"
#include "h2_bk_target_task_policy.h"
#include "h2_pal_webrtc_device.h"
#include <os/os.h>
#include <stdio.h>

static h2_runtime_t *runtime;
static h2_pal_webrtc_e2e_result_t result;
static void hold(void) {
  for (;;)
    rtos_delay_milliseconds(1000u);
}
static void fail(const char *stage, int rc) {
  printf("H2_PAL_WEBRTC_SETUP_FAIL stage=%s rc=%d\n", stage, rc);
  fflush(stdout);
  hold();
}
static void run(void *user) {
  (void)user;
  rtos_delay_milliseconds(5000u);
  /* H2Peer's normal peer arena is PSRAM. The conformance allocator must wrap
   * that region too; the Board's small default SRAM heap is for control state.
   * Keep the command service on its original Runtime and allocator. */
  h2_runtime_t test_runtime = *runtime;
  test_runtime.mem = h2_bk_platform_psram_allocator();
  int rc = h2_pal_webrtc_device_run(&test_runtime, &result);
  if (result.passed + result.failed + result.blocked !=
      H2_PAL_WEBRTC_E2E_CASE_COUNT)
    fail("fixture_or_network", rc);
  int confirm = rc == H2_PAL_OK ? h2_bk_h2loader_confirm_current_app(runtime)
                                : H2_PAL_ERR_INVALID_STATE;
  for (;;) {
    h2_pal_webrtc_device_report(runtime, &result);
    printf("H2_PAL_WEBRTC_READY board=bk7258 rc=%d confirm=%d\n", rc, confirm);
    fflush(stdout);
    rtos_delay_milliseconds(5000u);
  }
}
static void entry(void *user) {
  (void)user;
  h2_runtime_config_t config = {0};
  int rc = h2_bk7258_board_runtime_config(&config);
  if (rc != H2_PAL_OK)
    fail("board", rc);
  rc = h2_runtime_init(&config, &runtime);
  if (rc != H2_PAL_OK)
    fail("runtime", rc);
  rc = h2_bk_h2loader_start_app_iostreamikcp_with_capabilities(
      runtime, "pal-webrtc",
      H2_LOADER_CAPABILITY_UART | H2_LOADER_CAPABILITY_WIFI);
  if (rc != H2_PAL_OK)
    fail("commands", rc);
  const h2_pal_task_options_t options = {
      .name = h2_pal_webrtc_device_runner_task_name};
  h2_pal_task_t *runner = NULL;
  rc = h2_pal_task_start(runtime->task, &options, run, NULL, &runner);
  if (rc != H2_PAL_OK)
    fail("runner", rc);
  hold();
}
int main(void) {
  int rc = h2_bk_target_task_policy_install();
  if (rc != H2_PAL_OK)
    return -1;
  bk_init();
  rc = h2_bk7258_board_start_entry_task("bk/pal-webrtc", entry, NULL);
  if (rc != H2_PAL_OK)
    fail("entry", rc);
  return 0;
}
