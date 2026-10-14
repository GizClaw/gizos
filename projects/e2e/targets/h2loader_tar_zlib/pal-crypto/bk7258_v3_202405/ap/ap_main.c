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
    rtos_delay_milliseconds(1000);
}
static void fail(const char *stage, int rc) {
  printf("H2_CRYPTO_LAUNCHER_FAIL stage=%s rc=%d\n", stage, rc);
  hold();
}
static void watchdog(void *unused) {
  (void)unused;
  rtos_delay_milliseconds(600000);
  if (!finished)
    puts("H2_CRYPTO_WATCHDOG timeout=600s");
  hold();
}
static h2_pal_result_t run(h2_application_host_t *host, h2_runtime_t *app_runtime, void *user) {
    runtime = app_runtime;
  (void)user;
  rtos_delay_milliseconds(5000);
  int rc = h2_crypto_device_run(runtime, H2_CRYPTO_VERSION);
  finished = 1;
  int confirm = h2_bk_h2loader_confirm_current_app(runtime);
  printf("H2_CRYPTO_READY rc=%d confirm=%d\n", rc, confirm);
  while (!h2_application_host_should_stop(host)) {
    h2_crypto_device_replay(runtime);
    rtos_delay_milliseconds(3000);
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
      "pal-crypto", H2_LOADER_CAPABILITY_UART, &app, NULL);
  if (host_rc != H2_PAL_OK)
    fail("application_host", host_rc);
}
int main(void) {
  int rc = h2_bk_target_task_policy_install();
  if (rc)
    return -1;
  bk_init();
  rc = h2_bk7258_board_start_entry_task("bk/pal-crypto", entry, NULL);
  if (rc)
    fail("entry", rc);
  return 0;
}
