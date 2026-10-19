#include "h2_pal_audio_e2e.h"

#include <assert.h>
#include <limits.h>
#include <string.h>

typedef struct fake_audio {
  h2_pal_audio_api_t api;
  uint32_t gain;
  uint32_t volume;
  unsigned gain_restores;
  unsigned volume_restores;
  int speaker_started;
  int track_enabled;
  int track_live;
  int close_consumes_on_error;
  unsigned close_failures;
  unsigned close_calls;
  unsigned speaker_stop_calls;
  uint32_t track_volume;
  h2_pal_audio_track_t track;
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
  fake_audio_t *fake = user;
  ++fake->speaker_stop_calls;
  assert(!fake->track_live);
  fake->speaker_started = 0;
  return H2_AUDIO_OK;
}
static int mic_read(void *user, h2_audio_frame_t *frame, uint32_t timeout_ms) {
  (void)user;
  (void)frame;
  (void)timeout_ms;
  return H2_AUDIO_ERR_INVALID_STATE;
}
static int track_write(h2_pal_audio_track_t *track,
                       const h2_audio_frame_t *frame, uint32_t timeout_ms) {
  (void)track;
  (void)timeout_ms;
  if (frame->sample_rate_hz != 16000u || frame->channels != 1u ||
      frame->bytes != 320u * sizeof(int16_t) || frame->bytes > frame->capacity)
    return H2_AUDIO_ERR_INVALID_ARG;
  /* Fail a real case after creating a track, so terminal cleanup owns it. */
  return H2_AUDIO_ERR_IO;
}
static int track_close(h2_pal_audio_track_t *track) {
  fake_audio_t *fake = track->user;
  assert(fake->track_live);
  ++fake->close_calls;
  if (fake->close_consumes_on_error) {
    fake->track_live = 0;
    return H2_AUDIO_ERR_IO;
  }
  if (fake->close_failures != 0u) {
    --fake->close_failures;
    return H2_AUDIO_ERR_WOULD_BLOCK;
  }
  fake->track_live = 0;
  return H2_AUDIO_OK;
}
static int track_drain(h2_pal_audio_track_t *track, uint32_t timeout_ms) {
  (void)track;
  (void)timeout_ms;
  return H2_AUDIO_OK;
}
static int track_get_volume(h2_pal_audio_track_t *track, uint32_t *out) {
  *out = ((fake_audio_t *)track->user)->track_volume;
  return H2_AUDIO_OK;
}
static int track_set_volume(h2_pal_audio_track_t *track, uint32_t value) {
  ((fake_audio_t *)track->user)->track_volume = value;
  return H2_AUDIO_OK;
}
static int create_track(void *user, const h2_audio_track_config_t *config,
                        h2_pal_audio_track_t **out_track) {
  fake_audio_t *fake = user;
  *out_track = NULL;
  if (config->format.sample_format != H2_AUDIO_SAMPLE_S16LE)
    return H2_AUDIO_ERR_INVALID_ARG;
  if (!fake->track_enabled) return H2_AUDIO_ERR_UNSUPPORTED;
  assert(!fake->track_live);
  fake->track = (h2_pal_audio_track_t){
      .user = fake, .audio = &fake->api, .write = track_write,
      .close = track_close, .drain = track_drain,
      .get_volume_factor = track_get_volume,
      .set_volume_factor = track_set_volume};
  fake->track_volume = config->volume_factor_milli;
  fake->track_live = 1;
  *out_track = &fake->track;
  return H2_AUDIO_OK;
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

static void check_missing_track(void) {
  fake_audio_t fake = {.gain = 50u, .volume = 100u};
  fake.api = (h2_pal_audio_api_t){.user = &fake, .vtable = &vtable};
  const h2_pal_audio_e2e_config_t config = {.audio = &fake.api};
  h2_pal_audio_e2e_result_t result;
  assert(h2_pal_audio_e2e_run(&config, &result) != H2_AUDIO_OK);
  assert(result.cases[H2_PAL_AUDIO_E2E_TRACK_CREATE].blocked);
  assert(result.cases[H2_PAL_AUDIO_E2E_CLEANUP].passed);
  assert(fake.gain == 50u && fake.volume == 100u && !fake.speaker_started);
  assert(fake.gain_restores == 1u && fake.volume_restores == 1u);
}

static void check_close_failure(unsigned failures, int consumes_on_error) {
  fake_audio_t fake = {.gain = 50u, .volume = 100u, .track_enabled = 1,
                       .close_failures = failures,
                       .close_consumes_on_error = consumes_on_error};
  fake.api = (h2_pal_audio_api_t){.user = &fake, .vtable = &vtable};
  const h2_pal_audio_e2e_config_t config = {.audio = &fake.api};
  h2_pal_audio_e2e_result_t result;
  assert(h2_pal_audio_e2e_run(&config, &result) != H2_AUDIO_OK);
  assert(result.cases[H2_PAL_AUDIO_E2E_TRACK_WRITE].detail == H2_AUDIO_ERR_IO);
  assert(fake.gain_restores == 1u && fake.volume_restores == 1u);
  if (consumes_on_error) {
    /* Native close errors may consume their handle; never blindly retry IO. */
    assert(fake.close_calls == 1u && !fake.track_live);
    assert(!result.cases[H2_PAL_AUDIO_E2E_CLEANUP].passed);
    assert(fake.speaker_started && fake.speaker_stop_calls == 0u);
    assert(h2_pal_audio_stop_speaker(&fake.api) == H2_AUDIO_OK);
  } else if (failures < 3u) {
    assert(fake.close_calls == failures + 1u && !fake.track_live);
    assert(result.cases[H2_PAL_AUDIO_E2E_CLEANUP].passed);
    assert(!fake.speaker_started && fake.speaker_stop_calls == 1u);
  } else {
    assert(fake.close_calls == 3u && fake.track_live);
    assert(!result.cases[H2_PAL_AUDIO_E2E_CLEANUP].passed);
    assert(fake.speaker_started && fake.speaker_stop_calls == 0u);
    /* The provider still owns the handle after bounded retry exhaustion. */
    fake.close_failures = 0u;
    assert(h2_pal_audio_track_close(&fake.track) == H2_AUDIO_OK);
    assert(h2_pal_audio_stop_speaker(&fake.api) == H2_AUDIO_OK);
  }
}

int main(void) {
  check_missing_track();
  check_close_failure(1u, 0);
  check_close_failure(2u, 0);
  check_close_failure(UINT_MAX, 0);
  check_close_failure(0u, 1);
  return 0;
}
