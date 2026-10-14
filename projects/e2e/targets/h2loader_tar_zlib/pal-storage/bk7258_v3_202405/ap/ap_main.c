#include "h2_pal_e2e_application_host.h"
#include "bk_private/bk_init.h"
#include "device_runner.h"
#include "h2_bk7258_board.h"
#include "h2_bk_h2loader.h"
#include "h2_bk_platform_core.h"
#include "h2_bk_target_task_policy.h"
#include <os/os.h>
#include <stdio.h>

static h2_runtime_t *runtime;
static volatile int finished;
static void hold(void) {
  for (;;)
    rtos_delay_milliseconds(1000u);
}
static void fail(const char *stage, int rc) {
  printf("H2_STORAGE_LAUNCHER_FAIL stage=%s rc=%d\n", stage, rc);
  fflush(stdout);
  hold();
}
/* This diagnostic budget is specific to the real BK stress launcher. */
static void watchdog(void *unused) {
  (void)unused;
  rtos_delay_milliseconds(3600000u);
  if (!finished) {
    puts("H2_STORAGE_WATCHDOG timeout=3600s");
    fflush(stdout);
  }
  hold();
}
static h2_pal_result_t run(h2_application_host_t *host, h2_runtime_t *app_runtime, void *user) {
    runtime = app_runtime;
  (void)user;
  /* The CLI installs its native-log sink before serial SESSION_OPEN, but it
   * still needs a real startup reconnect/status window (20s wait + 15s read).
   * Keep rollback/control initialization while letting the subscriber settle.
   */
  rtos_delay_milliseconds(60000u);
  int rc = h2_bk_h2loader_stop_app_iostreamikcp();
  if (rc != H2_PAL_OK) {
    printf("H2_STORAGE_LAUNCHER_FAIL stage=commands-stop rc=%d\n", rc);
    printf("H2_STORAGE_READY rc=%d confirm=%d\n", rc, H2_PAL_ERR_INVALID_STATE);
    fflush(stdout);
    finished = 1;
    return rc;
  }
  puts("H2_STORAGE_CONTROL_QUIESCENT stop=0");
  fflush(stdout);
  /* Only run() in the shared device fixture emits the non-replay BOOT. No
   * replay is relabelled, and no fresh marker is repeated to repair loss. */
  rc = h2_storage_device_run(runtime, "/data/pal-storage", H2_STORAGE_VERSION);
  h2_storage_device_replay(runtime);
  fflush(stdout);
  /* SDK printf uses its asynchronous UART queue; fflush alone does not drain
   * it. A bounded quiet interval lets the last phase line leave before KCP
   * resumes on the same console. Case replay already paces each row at 90ms. */
  rtos_delay_milliseconds(200u);
  int commands = h2_bk_h2loader_start_app_iostreamikcp_with_capabilities(
      runtime, "pal-storage", H2_LOADER_CAPABILITY_UART);
  if (commands != H2_PAL_OK) {
    printf("H2_STORAGE_LAUNCHER_FAIL stage=commands-restart rc=%d\n", commands);
    printf("H2_STORAGE_READY rc=%d confirm=%d\n", commands,
           H2_PAL_ERR_INVALID_STATE);
    fflush(stdout);
    finished = 1;
    return rc;
  }
  puts("H2_STORAGE_CONTROL_RESTORED restart=0");
  fflush(stdout);
  int confirm = rc == H2_PAL_OK ? h2_bk_h2loader_confirm_current_app(runtime)
                                : H2_PAL_ERR_INVALID_STATE;
  finished = 1;
  printf("H2_STORAGE_READY rc=%d confirm=%d\n", rc, confirm);
  fflush(stdout);
  while (!h2_application_host_should_stop(host)) {
    rtos_delay_milliseconds(3000u);
    h2_storage_device_replay(runtime);
    printf("H2_STORAGE_READY rc=%d confirm=%d\n", rc, confirm);
    fflush(stdout);
  }

    return h2_pal_e2e_application_host_finish(host, runtime, rc);
}

static h2_pal_result_t configure(void *user, h2_runtime_config_t *config) {
  (void)user;
  config->system_event = h2_pal_unsupported_system_event_api();

  return H2_PAL_OK;
}
static h2_pal_result_t validate(void *user, h2_runtime_t *app_runtime) {
  (void)user;
  runtime = app_runtime;
  beken_thread_t timer;
  if (rtos_core0_create_psram_thread(&timer, 7, "store_deadline", watchdog,
                                     4096, NULL) != kNoErr)
    fail("watchdog", -12);

  return H2_PAL_OK;
}
static void entry(void *user) {
  (void)user;

  const h2_application_host_config_t app = {
      .entry = run, .configure = configure, .validate = validate};
  int host_rc = h2_pal_e2e_application_host_run(
      "pal-storage", H2_LOADER_CAPABILITY_UART, &app, NULL);
  if (host_rc != H2_PAL_OK)
    fail("application_host", host_rc);
}
int main(void) {
  int rc = h2_bk_target_task_policy_install();
  if (rc)
    return -1;
  bk_init();
  rc = h2_bk7258_board_start_entry_task("bk/pal-storage", entry, NULL);
  if (rc)
    fail("entry", rc);
  return 0;
}
