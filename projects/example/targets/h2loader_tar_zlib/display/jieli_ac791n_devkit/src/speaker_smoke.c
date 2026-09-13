#include "app_config.h"

#include "asm/includes.h"
#include "asm/wdt.h"
#include "h2_jieli_ac791n_devkit.h"
#include "os/os_api.h"

#include <stdint.h>

extern int printf(const char *format, ...);

enum {
  kSampleRate = 16000,
  kFrameSamples = 320,
};

static int16_t pcm[kFrameSamples];

static void stage(const char *name, int result) {
  printf("H2_JIELI_SPEAKER stage=%s rc=%d\r\n", name, result);
}

void app_main(void) {
  /* A stalled audio-server call must remain observable over USB instead of
   * turning into an uninformative watchdog-reset loop. */
  wdt_close();
  stage("boot", 0);

  const h2_pal_audio_api_t *audio = h2_jieli_ac791n_devkit_audio_api();
  h2_audio_info_t info = {0};
  int result = h2_pal_audio_get_info(audio, &info);
  stage("get-info", result);
  if (result != H2_AUDIO_OK) goto idle;

  result = h2_pal_audio_start_speaker(audio);
  stage("start-speaker", result);
  if (result != H2_AUDIO_OK) goto idle;

  const h2_audio_track_config_t config = {
      .name = "speaker-smoke",
      .format = info.playback_format,
      .volume_factor_milli = 500u,
      .buffer_frames = 8u,
  };
  h2_pal_audio_track_t *track = NULL;
  result = h2_pal_audio_create_track(audio, &config, &track);
  stage("create-track", result);
  if (result != H2_AUDIO_OK) goto idle;

  for (unsigned index = 0u; index < kFrameSamples; ++index) {
    pcm[index] = (index % 32u) < 16u ? 5000 : -5000;
  }
  h2_audio_frame_t frame =
      h2_audio_frame_for_buffer(pcm, sizeof(pcm), info.playback_format);
  frame.bytes = sizeof(pcm);
  for (unsigned repeat = 0u; repeat < 50u; ++repeat) {
    result = h2_pal_audio_track_write(track, &frame, 1000u);
    if (result != H2_AUDIO_OK) break;
  }
  stage("write-pcm", result);

idle:
  for (unsigned heartbeat = 0u;; ++heartbeat) {
    printf("H2_JIELI_SPEAKER heartbeat=%u last_rc=%d\r\n", heartbeat,
           result);
    os_time_dly(100u);
  }
}
