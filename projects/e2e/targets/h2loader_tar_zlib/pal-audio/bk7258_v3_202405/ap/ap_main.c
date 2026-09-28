#include "bk_private/bk_init.h"
#include "h2_bk7258_board.h"
#include "h2_bk_h2loader.h"
#include "h2_bk_platform_core.h"
#include "h2_bk_target_task_policy.h"
#include "h2_pal_audio_e2e.h"

#include <components/system.h>
#include <os/os.h>
#include <stdio.h>

static h2_runtime_t *runtime;
static const h2_pal_audio_api_t *provider_audio;
static h2_pal_audio_e2e_result_t result;

static void hold(void) {
  for (;;) rtos_delay_milliseconds(1000u);
}

static void fail(const char *stage, int rc) {
  printf("H2_PAL_AUDIO_SETUP_FAIL board=bk7258 stage=%s rc=%d\n", stage, rc);
  fflush(stdout);
  hold();
}

static void report(void *user, const h2_pal_audio_e2e_case_result_t *item) {
  (void)user;
  printf("H2_PAL_AUDIO_CASE {\"id\":\"%s\",\"status\":\"%s\",\"detail\":%d,\"line\":%u}\n",
         item->id, item->passed ? "PASS" : item->blocked ? "BLOCKED" : "FAIL",
         item->detail, item->line);
  fflush(stdout);
}

static void run(void *user) {
  (void)user;
  rtos_delay_milliseconds(5000u);
  const h2_pal_audio_e2e_config_t config = {
      .audio = provider_audio, .time = runtime->time,
      .stability_ms = 30000u, .report = report};
  const int rc = h2_pal_audio_e2e_run(&config, &result);
  printf("H2_PAL_AUDIO_STAGE stage=run_done rc=%d\n", rc);
  fflush(stdout);
  int confirm = H2_PAL_ERR_INVALID_STATE;
  if (rc == H2_AUDIO_OK) {
    printf("H2_PAL_AUDIO_STAGE stage=confirm_begin\n");
    fflush(stdout);
    confirm = h2_bk_h2loader_confirm_current_app(runtime);
    printf("H2_PAL_AUDIO_STAGE stage=confirm_end rc=%d\n", confirm);
  } else {
    printf("H2_PAL_AUDIO_STAGE stage=confirm_skipped rc=%d\n", rc);
  }
  fflush(stdout);
  for (;;) {
    for (size_t i = 0u; i < H2_PAL_AUDIO_E2E_CASE_COUNT; ++i) {
      const h2_pal_audio_e2e_case_result_t *item = &result.cases[i];
      printf("H2_PAL_AUDIO_REPLAY {\"id\":\"%s\",\"status\":\"%s\",\"detail\":%d}\n",
             item->id, item->passed ? "PASS" : item->blocked ? "BLOCKED" : "FAIL",
             item->detail);
      fflush(stdout);
      rtos_delay_milliseconds(10u);
    }
    printf("H2_PAL_AUDIO_SUMMARY {\"platform\":\"bk7258\",\"passed\":%u,"
           "\"failed\":%u,\"blocked\":%u,\"mic_frames\":%u,"
           "\"mic_peak\":%u,\"mic_energy\":%llu,\"speaker_frames\":%u,"
           "\"output_peak\":%u,\"stability_elapsed_ms\":%llu,"
           "\"rc\":%d,\"confirm\":%d}\n",
           result.passed, result.failed, result.blocked,
           (unsigned)result.mic_frames, (unsigned)result.mic_peak,
           (unsigned long long)result.mic_energy,
           (unsigned)result.speaker_frames, (unsigned)result.output_peak,
           (unsigned long long)result.stability_elapsed_ms,
           rc, confirm);
    fflush(stdout);
    rtos_delay_milliseconds(5000u);
  }
}

static void entry(void *user) {
  (void)user;
  h2_runtime_config_t config = {0};
  int rc = h2_bk7258_board_runtime_config(&config);
  if (rc != H2_PAL_OK) fail("board", rc);
  provider_audio = config.audio;
  rc = h2_runtime_init(&config, &runtime);
  if (rc != H2_PAL_OK) fail("runtime", rc);
  rc = h2_bk_h2loader_start_app_iostreamikcp_with_capabilities(
      runtime, "pal-audio", H2_LOADER_CAPABILITY_UART);
  if (rc != H2_PAL_OK) fail("commands", rc);
  printf("H2_PAL_AUDIO_BOOT board=bk7258 reset_reason=%u\n",
         (unsigned)bk_misc_get_reset_reason());
  fflush(stdout);
  const h2_pal_task_options_t options = {
      .name = h2_pal_audio_e2e_runner_task_name};
  h2_pal_task_t *runner = NULL;
  rc = h2_pal_task_start(runtime->task, &options, run, NULL, &runner);
  if (rc != H2_PAL_OK) fail("runner", rc);
  hold();
}

int main(void) {
  const int rc = h2_bk_target_task_policy_install();
  if (rc != H2_PAL_OK) return -1;
  bk_init();
  const int entry_rc = h2_bk7258_board_start_entry_task("bk/pal-audio", entry, NULL);
  if (entry_rc != H2_PAL_OK) fail("entry", entry_rc);
  return 0;
}
