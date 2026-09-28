#include "h2_pal_audio_e2e.h"

#include <assert.h>
#include <string.h>

typedef struct fake_audio {
  h2_pal_audio_api_t api;
  uint32_t gain;
  uint32_t volume;
  unsigned gain_restores;
  unsigned volume_restores;
  int speaker_started;
} fake_audio_t;

static int get_info(void *user, h2_audio_info_t *info) {
  (void)user;
  *info = (h2_audio_info_t){
      .available = 1,
      .mic_supported = 1,
      .playback_supported = 1,
      .mic_format = {16000u, 320u, 1u, H2_AUDIO_SAMPLE_S16LE},
      .playback_format = {16000u, 320u, 1u, H2_AUDIO_SAMPLE_S16LE},
      .mic_queue_frames = 1u,
      .track_queue_frames = 1u,
      .max_tracks = 1u};
  return H2_AUDIO_OK;
}
static int mic_control(void *user) {
  (void)user;
  return H2_AUDIO_OK;
}
static int speaker_start(void *user) {
  ((fake_audio_t *)user)->speaker_started = 1;
  return H2_AUDIO_OK;
}
static int speaker_stop(void *user) {
  ((fake_audio_t *)user)->speaker_started = 0;
  return H2_AUDIO_OK;
}
static int mic_read(void *user, h2_audio_frame_t *frame, uint32_t timeout_ms) {
  (void)user;
  (void)frame;
  (void)timeout_ms;
  return H2_AUDIO_ERR_INVALID_STATE;
}
static int create_track(void *user, const h2_audio_track_config_t *config,
                        h2_pal_audio_track_t **out_track) {
  (void)user;
  *out_track = NULL;
  return config->format.sample_format == H2_AUDIO_SAMPLE_S16LE
             ? H2_AUDIO_ERR_UNSUPPORTED : H2_AUDIO_ERR_INVALID_ARG;
}
static int get_speaker(void *user, uint32_t *out) {
  *out = ((fake_audio_t *)user)->volume;
  return H2_AUDIO_OK;
}
static int set_speaker(void *user, uint32_t value) {
  fake_audio_t *fake = user;
  if (value > 100u) return H2_AUDIO_ERR_INVALID_ARG;
  if (value == 100u) ++fake->volume_restores;
  fake->volume = value;
  return H2_AUDIO_OK;
}
static int get_gain(void *user, uint32_t *out) {
  *out = ((fake_audio_t *)user)->gain;
  return H2_AUDIO_OK;
}
static int set_gain(void *user, uint32_t value) {
  fake_audio_t *fake = user;
  if (value > 100u) return H2_AUDIO_ERR_INVALID_ARG;
  if (value == 50u) ++fake->gain_restores;
  fake->gain = value;
  return H2_AUDIO_OK;
}

static const h2_pal_audio_vtable_t vtable = {
    .get_info = get_info,
    .start_mic = mic_control,
    .stop_mic = mic_control,
    .start_speaker = speaker_start,
    .stop_speaker = speaker_stop,
    .mic_read = mic_read,
    .create_track = create_track,
    .get_speaker_volume_percent = get_speaker,
    .set_speaker_volume_percent = set_speaker,
    .get_mic_gain_percent = get_gain,
    .set_mic_gain_percent = set_gain,
};

int main(void) {
  fake_audio_t fake = {.gain = 50u, .volume = 100u};
  fake.api = (h2_pal_audio_api_t){.user = &fake, .vtable = &vtable};
  const h2_pal_audio_e2e_config_t config = {.audio = &fake.api};
  h2_pal_audio_e2e_result_t result;
  assert(h2_pal_audio_e2e_run(&config, &result) != H2_AUDIO_OK);
  assert(result.cases[H2_PAL_AUDIO_E2E_TRACK_CREATE].blocked);
  assert(result.cases[H2_PAL_AUDIO_E2E_CLEANUP].passed);
  assert(fake.gain == 50u && fake.volume == 100u && !fake.speaker_started);
  assert(fake.gain_restores == 1u && fake.volume_restores == 1u);
  return 0;
}
