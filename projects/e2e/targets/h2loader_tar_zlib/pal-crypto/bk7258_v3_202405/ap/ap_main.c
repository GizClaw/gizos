#include "h2_pal_e2e_application_host.h"
#include "device_runner.h"
#include "h2_bk_h2loader.h"
#include "h2_bk_platform_core.h"
#include <os/os.h>
#include <stdio.h>
static volatile int finished;

static void watchdog(void *unused) {
  (void)unused;
  rtos_delay_milliseconds(600000);
  if (!finished)
    puts("H2_CRYPTO_WATCHDOG timeout=600s");
  h2_pal_e2e_application_host_park();
}
static h2_pal_result_t run(h2_application_host_t *host, h2_runtime_t *runtime, void *user) {
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

  return rc;
}

static h2_pal_result_t configure(void *user, h2_runtime_config_t *config) {
  (void)user;
  config->system_event = h2_pal_unsupported_system_event_api();

  return H2_PAL_OK;
}
static h2_pal_result_t validate(void *user, h2_runtime_t *runtime) {
  (void)user;
  (void)runtime;
  beken_thread_t timer;
  if (rtos_core0_create_psram_thread(&timer, 7, "store_deadline", watchdog,
                                     4096, NULL) != kNoErr)
    h2_pal_e2e_application_host_fail("watchdog", -12);

  return H2_PAL_OK;
}

int main(void) {
    const h2_application_host_config_t app = {
        .entry = run, .configure = configure, .validate = validate};
    return h2_pal_e2e_application_host_start(
        "pal-crypto", H2_LOADER_CAPABILITY_UART, &app);
}
