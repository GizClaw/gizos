#include "h2_pal_audio_e2e.h"
#include "h2_web_application_host.h"

#include <emscripten/threading.h>
#include <stdio.h>

static void report(void *user, const h2_pal_audio_e2e_case_result_t *item) {
  (void)user;
  printf("H2_PAL_AUDIO_CASE {\"id\":\"%s\",\"status\":\"%s\",\"detail\":%d,\"line\":%u}\n",
         item->id, item->passed ? "PASS" : item->blocked ? "BLOCKED" : "FAIL",
         item->detail, item->line);
  fflush(stdout);
}

static h2_pal_audio_e2e_result_t result;
static h2_pal_result_t run(h2_application_host_t *host, h2_runtime_t *runtime, void *user) {
  (void)host; (void)user;
  const h2_pal_audio_e2e_config_t test_config = {
      .audio = runtime->audio,
      .time = runtime->time,
      .stability_ms = 30000u,
      .report = report};
  const int rc = h2_pal_audio_e2e_run(&test_config, &result);
  printf("H2_PAL_AUDIO_SUMMARY {\"platform\":\"wasm-chromium\",\"worker\":1,"
         "\"passed\":%u,\"failed\":%u,\"blocked\":%u,"
         "\"mic_frames\":%u,\"mic_peak\":%u,\"mic_energy\":%llu,"
         "\"speaker_frames\":%u,\"output_peak\":%u,"
         "\"stability_elapsed_ms\":%llu,\"rc\":%d}\n",
         result.passed, result.failed, result.blocked, result.mic_frames,
         result.mic_peak, (unsigned long long)result.mic_energy,
         result.speaker_frames, result.output_peak,
         (unsigned long long)result.stability_elapsed_ms, rc);
  return rc;
}
int main(void) {
  const h2_web_application_host_config_t config = {
      .name = "pal-audio", .display_width = 1, .display_height = 1};
  const h2_application_host_config_t app = {.entry = run};
  h2_application_host_result_t lifecycle = {0};
  int rc = h2_web_application_host_run_app(&config, &app, &lifecycle);
  printf("H2_PAL_AUDIO_TEARDOWN rc=%d\n", lifecycle.cleanup);
  return rc == 0 && lifecycle.cleanup == H2_PAL_OK && !lifecycle.retained ? 0 : 1;
}
