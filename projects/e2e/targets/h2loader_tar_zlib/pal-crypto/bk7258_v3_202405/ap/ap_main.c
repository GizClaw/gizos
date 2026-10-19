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
static void run(void *unused) {
  (void)unused;
  rtos_delay_milliseconds(5000);
  int rc = h2_crypto_device_run(runtime, H2_CRYPTO_VERSION);
  finished = 1;
  int confirm = h2_bk_h2loader_confirm_current_app(runtime);
  printf("H2_CRYPTO_READY rc=%d confirm=%d\n", rc, confirm);
  for (;;) {
    h2_crypto_device_replay(runtime);
    rtos_delay_milliseconds(3000);
  }
}
static void entry(void *unused) {
  (void)unused;
  h2_runtime_config_t config;
  int rc = h2_bk7258_board_runtime_config(&config);
  if (rc)
    fail("board", rc);
  config.system_event = h2_pal_unsupported_system_event_api();
  rc = h2_runtime_init(&config, &runtime);
  if (rc)
    fail("runtime", rc);
  rc = h2_bk_h2loader_start_app_iostreamikcp_with_capabilities(
      runtime, "pal-crypto", H2_LOADER_CAPABILITY_UART);
  if (rc)
    fail("commands", rc);
  beken_thread_t timer;
  if (rtos_core0_create_psram_thread(&timer, 7, "store_deadline", watchdog,
                                     4096, NULL) != kNoErr)
    fail("watchdog", -12);
  const h2_pal_task_options_t options = {.name = "pal-crypto/e2e/runner",
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
  rc = h2_bk7258_board_start_entry_task("bk/pal-crypto", entry, NULL);
  if (rc)
    fail("entry", rc);
  return 0;
}
