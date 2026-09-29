#ifndef H2_PAL_AUDIO_E2E_H
#define H2_PAL_AUDIO_E2E_H

#include "h2/pal/hal/h2_pal_audio.h"
#include "h2/pal/os/h2_pal_time.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum h2_pal_audio_e2e_case {
#define H2_PAL_AUDIO_CASE(symbol, id) H2_PAL_AUDIO_E2E_##symbol,
#include "h2_pal_audio_cases.inc"
#undef H2_PAL_AUDIO_CASE
  H2_PAL_AUDIO_E2E_CASE_COUNT
} h2_pal_audio_e2e_case_t;

extern const char h2_pal_audio_e2e_runner_task_name[];

typedef struct h2_pal_audio_e2e_case_result {
  const char *id;
  int passed;
  int blocked;
  int detail;
  unsigned line;
} h2_pal_audio_e2e_case_result_t;

typedef struct h2_pal_audio_e2e_result {
  unsigned passed;
  unsigned failed;
  unsigned blocked;
  uint32_t mic_frames;
  uint32_t mic_peak;
  uint64_t mic_energy;
  uint32_t speaker_frames;
  uint32_t output_peak;
  uint64_t stability_elapsed_ms;
  h2_pal_audio_e2e_case_result_t cases[H2_PAL_AUDIO_E2E_CASE_COUNT];
} h2_pal_audio_e2e_result_t;

typedef struct h2_pal_audio_e2e_config {
  /** Borrowed real platform Audio PAL. It must remain alive through return. */
  const h2_pal_audio_api_t *audio;
  const h2_pal_time_api_t *time;
  /** Zero disables the additional sustained record/playback run. */
  uint32_t stability_ms;
  void (*report)(void *user, const h2_pal_audio_e2e_case_result_t *result);
  void *report_user;
} h2_pal_audio_e2e_config_t;

/** Runs the complete mandatory case registry. Missing provider operations fail.
 * Restores initial gain and speaker volume. Terminal cleanup retries an
 * incomplete track close at most three times, retaining its handle between
 * attempts. A cleanup failure prevents qualification; the launcher must then
 * tear down the provider, which may still own an attached track. */
int h2_pal_audio_e2e_run(const h2_pal_audio_e2e_config_t *config,
                         h2_pal_audio_e2e_result_t *out_result);

#ifdef __cplusplus
}
#endif
#endif
