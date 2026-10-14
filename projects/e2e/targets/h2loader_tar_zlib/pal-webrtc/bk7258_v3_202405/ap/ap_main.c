#include "h2_pal_e2e_application_host.h"
#include "bk_private/bk_init.h"
#include "h2_bk7258_board.h"
#include "h2_bk_h2loader.h"
#include "h2_bk_platform_core.h"
#include "h2_bk_target_task_policy.h"
#include "h2_pal_webrtc_device.h"
#include <components/system.h>
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
static h2_pal_result_t run(h2_application_host_t *host, h2_runtime_t *app_runtime, void *user) {
    runtime = app_runtime;
  (void)user;
  rtos_delay_milliseconds(5000u);
  /* H2Peer's normal peer arena is PSRAM. The conformance allocator must wrap
   * that region too; the Board's small default SRAM heap is for control state.
   * Keep the command service on its original Runtime and allocator. */
  h2_runtime_t test_runtime = *runtime;
  test_runtime.mem = h2_bk_platform_psram_allocator();
  int rc = h2_pal_webrtc_device_run(&test_runtime, 60000u, 600000u, &result);
  if (result.passed + result.failed + result.blocked !=
      H2_PAL_WEBRTC_E2E_CASE_COUNT)
    fail("fixture_or_network", rc);
  int confirm = rc == H2_PAL_OK ? h2_bk_h2loader_confirm_current_app(runtime)
                                : H2_PAL_ERR_INVALID_STATE;
  while (!h2_application_host_should_stop(host)) {
    h2_pal_webrtc_device_report(runtime, &result);
    printf("H2_PAL_WEBRTC_READY board=bk7258 rc=%d confirm=%d\n", rc, confirm);
    fflush(stdout);
    rtos_delay_milliseconds(5000u);
  }

    return h2_pal_e2e_application_host_finish(host, runtime, rc);
}

static h2_pal_result_t validate(void *user, h2_runtime_t *app_runtime) {
  (void)user;
  runtime = app_runtime;
  printf("H2_PAL_WEBRTC_BOOT board=bk7258 reset_reason=%u\n",
         (unsigned)bk_misc_get_reset_reason());
  fflush(stdout);

  return H2_PAL_OK;
}
static void entry(void *user) {
  (void)user;

  const h2_application_host_config_t app = {.entry = run, .validate = validate};
  int host_rc = h2_pal_e2e_application_host_run(
      "pal-webrtc", H2_LOADER_CAPABILITY_UART | H2_LOADER_CAPABILITY_WIFI, &app,
      NULL);
  if (host_rc != H2_PAL_OK)
    fail("application_host", host_rc);
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
