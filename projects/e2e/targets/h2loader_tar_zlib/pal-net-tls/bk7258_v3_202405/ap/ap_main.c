#include "bk_private/bk_init.h"
#include "h2_bk7258_board.h"
#include "h2_bk_h2loader.h"
#include "h2_bk_target_task_policy.h"
#include "h2_pal_net_tls_device.h"
#include <os/os.h>
#include <stdio.h>

static h2_runtime_t *runtime;
static h2_net_tls_result_t result;
static void hold(void) {
  for (;;)
    rtos_delay_milliseconds(1000u);
}
static void fail(const char *stage, int rc) {
  printf("H2_PAL_NET_TLS_SETUP_FAIL stage=%s rc=%d\n", stage, rc);
  fflush(stdout);
  hold();
}
static void run(void *user) {
  (void)user;
  rtos_delay_milliseconds(5000u);
  int rc;
  uint64_t setup_start = 0u;
  if (h2_pal_time_get_monotonic_ms(runtime->time, &setup_start) != H2_PAL_OK)
    fail("clock", H2_PAL_ERR_IO);
  for (;;) {
    rc = h2_pal_net_tls_device_run(runtime, &result);
    if (result.passed + result.failed + result.blocked + result.unsupported +
            result.not_assessed !=
        0u)
      break;
    if (rc != H2_PAL_ERR_NOT_FOUND && rc != H2_PAL_ERR_UNAVAILABLE &&
        rc != H2_PAL_ERR_TIMEOUT && rc != H2_PAL_ERR_BUSY)
      break;
    uint64_t current = 0u;
    if (h2_pal_time_get_monotonic_ms(runtime->time, &current) != H2_PAL_OK ||
        current - setup_start >= 120000u)
      break;
    printf(
        "H2_PAL_NET_TLS_SETUP_WAIT network=not_ready rc=%d cases_started=0\n",
        rc);
    fflush(stdout);
    rtos_delay_milliseconds(3000u);
  }
  if (result.passed + result.failed + result.blocked + result.unsupported +
          result.not_assessed !=
      H2_NET_TLS_CASE_COUNT)
    fail("fixture_or_network", rc);
  int confirm = rc == H2_PAL_OK ? h2_bk_h2loader_confirm_current_app(runtime)
                                : H2_PAL_ERR_INVALID_STATE;
  for (;;) {
    h2_pal_net_tls_device_report(runtime, &result);
    printf("H2_PAL_NET_TLS_READY board=bk7258 rc=%d confirm=%d\n", rc, confirm);
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
      runtime, "pal-net-tls",
      H2_LOADER_CAPABILITY_UART | H2_LOADER_CAPABILITY_WIFI);
  if (rc != H2_PAL_OK)
    fail("commands", rc);
  h2_pal_firmware_info_t image = {0};
  int version =
      h2_pal_firmware_info_get_current(runtime->firmware_info, &image);
  printf("H2_PAL_NET_TLS_BOOT board=bk7258 version=%s version_rc=%d\n",
         version == H2_PAL_OK ? image.version : "unavailable", version);
  const h2_pal_task_options_t options = {
      .name = h2_pal_net_tls_device_runner_task_name};
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
  rc = h2_bk7258_board_start_entry_task("bk/pal-net-tls", entry, NULL);
  if (rc != H2_PAL_OK)
    fail("entry", rc);
  return 0;
}
