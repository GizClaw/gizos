#include "h2_jieli_ac791n_devkit.h"
#include "h2_runtime.h"
#include "h2_smoke_audio_system.h"

#include <stdio.h>

int h2_jieli_target_application_run(void) {
  h2_runtime_config_t config;
  h2_runtime_t *runtime = NULL;
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
  printf("H2_JIELI_AUDIO_SYSTEM stage=run result=%d\n", result);
  if (result == H2_AUDIO_OK) {
    printf("H2_JIELI_AUDIO_SYSTEM_READY mic=1 speaker=1 aec=dac-software-ref\n");
  }
  return result;
}
