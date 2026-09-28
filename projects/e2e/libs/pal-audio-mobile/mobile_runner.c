#include "mobile_runner.h"

#include <stdio.h>

int h2_audio_mobile_run(const h2_pal_audio_api_t *audio,
                        const h2_pal_time_api_t *time,
                        h2_pal_audio_e2e_result_t *result) {
  const h2_pal_audio_e2e_config_t config = {
      .audio = audio, .time = time, .stability_ms = 30000u};
  return h2_pal_audio_e2e_run(&config, result);
}

int h2_audio_mobile_report(const char *path, const char *platform,
                           const char *version,
                           const h2_pal_audio_e2e_result_t *result,
                           int run_rc, int teardown_rc) {
  if (path == NULL || platform == NULL || version == NULL || result == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  FILE *file = fopen(path, "w");
  if (file == NULL) return H2_PAL_ERR_WRITE;
  fprintf(file, "{\"platform\":\"%s\",\"image_version\":\"%s\","
                "\"passed\":%u,\"failed\":%u,\"blocked\":%u,"
                "\"mic_frames\":%u,\"mic_peak\":%u,\"mic_energy\":%llu,"
                "\"speaker_frames\":%u,\"output_peak\":%u,"
                "\"stability_elapsed_ms\":%llu,"
                "\"run_rc\":%d,\"teardown_rc\":%d,\"cases\":[",
          platform, version, result->passed, result->failed, result->blocked,
          (unsigned)result->mic_frames, (unsigned)result->mic_peak,
          (unsigned long long)result->mic_energy,
          (unsigned)result->speaker_frames, (unsigned)result->output_peak,
          (unsigned long long)result->stability_elapsed_ms,
          run_rc, teardown_rc);
  for (size_t i = 0u; i < H2_PAL_AUDIO_E2E_CASE_COUNT; ++i) {
    const h2_pal_audio_e2e_case_result_t *item = &result->cases[i];
    fprintf(file, "%s{\"id\":\"%s\",\"status\":\"%s\","
                  "\"detail\":%d,\"line\":%u}",
            i == 0u ? "" : ",", item->id ? item->id : "missing",
            item->passed ? "PASS" : item->blocked ? "BLOCKED" : "FAIL",
            item->detail, item->line);
  }
  const int wrote = fputs("]}\n", file) >= 0;
  const int closed = fclose(file) == 0;
  if (!wrote || !closed) return H2_PAL_ERR_WRITE;
  return run_rc == H2_AUDIO_OK && teardown_rc == H2_PAL_OK &&
                 result->passed == H2_PAL_AUDIO_E2E_CASE_COUNT &&
                 result->failed == 0u && result->blocked == 0u &&
                 result->mic_frames >= 2u && result->speaker_frames >= 2u &&
                 result->stability_elapsed_ms >= 30000u
             ? H2_PAL_OK : H2_PAL_ERR_INVALID_STATE;
}
