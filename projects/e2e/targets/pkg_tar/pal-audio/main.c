#include "h2_pal_audio_e2e.h"
#include "h2_web_platform.h"

#include <emscripten/threading.h>
#include <stdio.h>

static void report(void *user, const h2_pal_audio_e2e_case_result_t *item) {
  (void)user;
  printf("H2_PAL_AUDIO_CASE {\"id\":\"%s\",\"status\":\"%s\",\"detail\":%d,\"line\":%u}\n",
         item->id, item->passed ? "PASS" : item->blocked ? "BLOCKED" : "FAIL",
         item->detail, item->line);
  fflush(stdout);
}

int main(void) {
  if (emscripten_is_main_runtime_thread()) return 2;
  const h2_web_platform_config_t config = {.display_width = 1u,
                                           .display_height = 1u};
  h2_web_platform_t *platform = h2_web_platform_create(&config);
  if (platform == NULL) return 3;
  const h2_pal_audio_e2e_config_t test_config = {
      .audio = h2_web_platform_audio_api(platform),
      .time = h2_web_platform_time_api(platform),
      .stability_ms = 30000u,
      .report = report};
  h2_pal_audio_e2e_result_t result;
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
  const int teardown = h2_web_platform_destroy(platform);
  printf("H2_PAL_AUDIO_TEARDOWN rc=%d\n", teardown);
  return rc == H2_AUDIO_OK && teardown == H2_PAL_OK ? 0 : 1;
}
