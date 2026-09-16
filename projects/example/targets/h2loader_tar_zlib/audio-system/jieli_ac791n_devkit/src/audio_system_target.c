#include "h2_jieli_ac791n_devkit.h"
#include "h2_runtime.h"
#include "h2_smoke_audio_system.h"

#include <stdio.h>

/* A started audio scene owns Runtime for this boot; the launcher never reloads
 * images in-process, so returning successfully must leave playback running. */
int h2_jieli_target_application_run(void) {
  h2_runtime_config_t config;
  h2_runtime_t *runtime = NULL;
  int scene_started = 0;
  int result = h2_jieli_ac791n_devkit_runtime_config(&config);
  printf("H2_JIELI_AUDIO_SYSTEM stage=runtime-config result=%d\n", result);
  if (result == H2_PAL_OK) {
    result = h2_runtime_init(&config, &runtime);
  }
  printf("H2_JIELI_AUDIO_SYSTEM stage=runtime-init result=%d\n", result);
  if (result != H2_PAL_OK) return result;

  const h2_smoke_audio_system_config_t audio_config = {
      .music_path = NULL,
      .speaker_volume_percent = 80u,
  };
  result = h2_smoke_audio_system_run(runtime, &audio_config);
  scene_started = 1; /* run() may leave a partially started scene on failure. */
  printf("H2_JIELI_AUDIO_SYSTEM stage=run result=%d\n", result);
  if (result == H2_AUDIO_OK) {
    printf("H2_JIELI_AUDIO_SYSTEM_READY mic=1 speaker=1 aec=dac-software-ref\n");
    return result;
  }
  if (scene_started && runtime != NULL) {
    /* Bound failed-start cleanup to 100 attempts, with 10 ms between retries. */
    int cleanup_result = H2_AUDIO_OK;
    for (unsigned attempt = 0u; attempt < 100u; ++attempt) {
      cleanup_result = h2_smoke_audio_system_stop();
      if (cleanup_result == H2_AUDIO_OK) break;
      if (attempt + 1u < 100u) {
        (void)h2_pal_time_sleep_ms(runtime->time, 10u);
      }
    }
    if (cleanup_result != H2_AUDIO_OK) {
      printf("H2_JIELI_AUDIO_SYSTEM cleanup did not complete result=%d attempts=100\n",
             cleanup_result);
    }
    h2_runtime_deinit(runtime);
  }
  return result;
}
