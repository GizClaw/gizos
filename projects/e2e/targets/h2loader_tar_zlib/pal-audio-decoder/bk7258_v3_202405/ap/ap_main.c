#include "h2_pal_e2e_application_host.h"
#include "device_runner.h"
#include "h2_bk_audio_decoder.h"
#include "h2_bk_h2loader.h"
#include "h2_bk_platform_core.h"
#include <os/os.h>
#include <stdio.h>
static volatile int finished;

static void watchdog(void *unused) {
  (void)unused;
  rtos_delay_milliseconds(600000);
  if (!finished)
    puts("H2_ADEC_WATCHDOG timeout=600s");
  h2_pal_e2e_application_host_park();
}
static h2_pal_result_t run(h2_application_host_t *host, h2_runtime_t *runtime, void *user) {
  (void)user;
  rtos_delay_milliseconds(5000);
  int rc = h2_adec_device_run(runtime, H2_ADEC_VERSION);
  finished = 1;
  if (rc != H2_PAL_OK) {
    printf("H2_ADEC_QUALIFICATION_FAIL rc=%d confirm=not-attempted\n", rc);
    h2_pal_e2e_application_host_fail("qualification", rc);
  }
  int confirm = h2_bk_h2loader_confirm_current_app(runtime);
  if (confirm != H2_PAL_OK)
    h2_pal_e2e_application_host_fail("confirm", confirm);
  puts("H2_ADEC_READY rc=0 confirm=0");
  while (!h2_application_host_should_stop(host)) {
    h2_adec_device_replay(runtime);
    rtos_delay_milliseconds(3000);
  }

  return rc;
}

static h2_pal_result_t configure(void *user, h2_runtime_config_t *config) {
  (void)user;
  config->audio_decoder = h2_bk_audio_decoder_api();
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
        "pal-audio-decoder", H2_LOADER_CAPABILITY_UART, &app);
}
