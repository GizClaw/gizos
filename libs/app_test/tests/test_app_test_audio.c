#include "h2_app_test_audio.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

typedef struct fake_audio {
  h2_pal_audio_api_t api;
  unsigned int start_mic;
  unsigned int stop_mic;
  unsigned int reads;
  unsigned int writes;
  unsigned int drains;
  unsigned int closes;
  int mic_read_rc;
  int stop_mic_rc;
  int drain_rc;
  int close_rc;
  h2_audio_pcm_format_t mic_format;
} fake_audio_t;

typedef struct fake_time {
  h2_pal_time_api_t api;
  uint64_t now_ms;
  h2_app_test_audio_t *pause_on_sleep;
  bool resume_on_sleep;
  const h2_app_test_audio_fixture_t *replace_on_sleep;
} fake_time_t;

static void *test_alloc(void *user, size_t size) {
  (void)user;
  return malloc(size);
}
static void *test_realloc(void *user, void *pointer, size_t size) {
  (void)user;
  return realloc(pointer, size);
}
static void test_free(void *user, void *pointer) {
  (void)user;
  free(pointer);
}

static h2_pal_result_t fake_get_monotonic_ms(void *user, uint64_t *out_ms) {
  fake_time_t *time = user;
  *out_ms = time->now_ms;
  return H2_PAL_OK;
}

static h2_pal_result_t fake_sleep_ms(void *user, uint32_t ms) {
  fake_time_t *time = user;
  time->now_ms += ms;
  if (time->pause_on_sleep != NULL) {
    h2_app_test_audio_set_capture_active(time->pause_on_sleep, false);
    if (time->replace_on_sleep != NULL) {
      assert(h2_app_test_audio_set_fixture(time->pause_on_sleep,
                                           time->replace_on_sleep) == H2_PAL_OK);
      time->replace_on_sleep = NULL;
    }
    if (time->resume_on_sleep) {
      h2_app_test_audio_set_capture_active(time->pause_on_sleep, true);
    }
    time->pause_on_sleep = NULL;
  }
  return H2_PAL_OK;
}

static int fake_get_info(void *user, h2_audio_info_t *info) {
  const fake_audio_t *audio = user;
  *info = (h2_audio_info_t){
      .available = 1,
      .mic_supported = 1,
      .playback_supported = 1,
      .mic_format = audio->mic_format,
      .playback_format = {16000u, 4u, 1u, H2_AUDIO_SAMPLE_S16LE},
  };
  return H2_PAL_OK;
}
static int fake_start_mic(void *user) {
  ((fake_audio_t *)user)->start_mic++;
  return H2_PAL_OK;
}
static int fake_stop_mic(void *user) {
  fake_audio_t *audio = user;
  audio->stop_mic++;
  return audio->stop_mic_rc;
}
static int fake_start_speaker(void *user) {
  (void)user;
  return H2_PAL_OK;
}
static int fake_stop_speaker(void *user) {
  (void)user;
  return H2_PAL_OK;
}
static int fake_mic_read(void *user, h2_audio_frame_t *frame,
                         uint32_t timeout_ms) {
  fake_audio_t *audio = user;
  (void)timeout_ms;
  audio->reads++;
  if (audio->mic_read_rc != H2_PAL_OK) {
    return audio->mic_read_rc;
  }
  memset(frame->data, 0x7f, 8u);
  frame->bytes = 8u;
  return H2_PAL_OK;
}
static int fake_track_write(h2_pal_audio_track_t *track,
                            const h2_audio_frame_t *frame,
                            uint32_t timeout_ms) {
  fake_audio_t *audio = track->user;
  (void)frame;
  (void)timeout_ms;
  audio->writes++;
  return H2_PAL_OK;
}
static int fake_track_close(h2_pal_audio_track_t *track) {
  fake_audio_t *audio = track->user;
  audio->closes++;
  if (audio->close_rc == H2_PAL_OK) {
    free(track);
  }
  return audio->close_rc;
}
static int fake_track_get(h2_pal_audio_track_t *track, uint32_t *out_value) {
  (void)track;
  *out_value = 1000u;
  return H2_PAL_OK;
}
static int fake_track_set(h2_pal_audio_track_t *track, uint32_t value) {
  (void)track;
  (void)value;
  return H2_PAL_OK;
}
static int fake_track_drain(h2_pal_audio_track_t *track, uint32_t timeout_ms) {
  (void)timeout_ms;
  fake_audio_t *audio = track->user;
  audio->drains++;
  return audio->drain_rc;
}
static int fake_create_track(void *user, const h2_audio_track_config_t *config,
                             h2_pal_audio_track_t **out_track) {
  fake_audio_t *audio = user;
  (void)config;
  h2_pal_audio_track_t *track = malloc(sizeof(*track));
  assert(track != NULL);
  *track = (h2_pal_audio_track_t){
      .user = audio,
      .audio = &audio->api,
      .write = fake_track_write,
      .close = fake_track_close,
      .get_volume_factor = fake_track_get,
      .set_volume_factor = fake_track_set,
      .drain = fake_track_drain,
  };
  *out_track = track;
  return H2_PAL_OK;
}
static int fake_get_volume(void *user, uint32_t *out_percent) {
  (void)user;
  *out_percent = 50u;
  return H2_PAL_OK;
}
static int fake_set_volume(void *user, uint32_t percent) {
  (void)user;
  (void)percent;
  return H2_PAL_OK;
}

static const h2_pal_mem_vtable_t s_mem_vtable = {
    .alloc = test_alloc,
    .realloc = test_realloc,
    .free = test_free,
};
static const h2_pal_audio_vtable_t s_audio_vtable = {
    .get_info = fake_get_info,
    .start_mic = fake_start_mic,
    .stop_mic = fake_stop_mic,
    .start_speaker = fake_start_speaker,
    .stop_speaker = fake_stop_speaker,
    .mic_read = fake_mic_read,
    .create_track = fake_create_track,
    .get_speaker_volume_percent = fake_get_volume,
    .set_speaker_volume_percent = fake_set_volume,
};
static const h2_pal_time_vtable_t s_time_vtable = {
    .get_monotonic_ms = fake_get_monotonic_ms,
    .sleep_ms = fake_sleep_ms,
};
static void test_capture_gate(void) {
  const h2_pal_mem_api_t mem = {.vtable = &s_mem_vtable};
  fake_time_t time = {.now_ms = 1000u};
  time.api = (h2_pal_time_api_t){.user = &time, .vtable = &s_time_vtable};
  fake_audio_t fake = {
      .mic_format = {16000u, 160u, 1u, H2_AUDIO_SAMPLE_S16LE},
  };
  fake.api = (h2_pal_audio_api_t){.user = &fake, .vtable = &s_audio_vtable};
  uint8_t pcm[960];
  memset(pcm, 1, 320u);
  memset(pcm + 320u, 2, 320u);
  memset(pcm + 640u, 3, 320u);
  const h2_app_test_audio_fixture_t fixture = {pcm, sizeof(pcm), fake.mic_format};
  h2_app_test_audio_t *audio = NULL;
  assert(h2_app_test_audio_create(&mem, &time.api, &fake.api, &fixture,
                                &audio) == H2_PAL_OK);
  const h2_pal_audio_api_t *api = h2_app_test_audio_api(audio);
  uint8_t output[320];
  h2_audio_frame_t frame =
      h2_audio_frame_for_buffer(output, sizeof(output), fake.mic_format);
  h2_app_test_audio_evidence_t evidence;
  h2_app_test_audio_set_capture_active(NULL, false);
  h2_app_test_audio_set_capture_active(audio, false);
  assert(h2_pal_audio_start_mic(api) == H2_PAL_OK);
  time.now_ms += 60000u;
  const uint64_t paused_ms = time.now_ms;
  memset(output, 0x55, sizeof(output));
  assert(h2_pal_audio_mic_read(api, &frame, 100u) == H2_PAL_ERR_WOULD_BLOCK);
  assert(frame.bytes == 0u && time.now_ms == paused_ms);
  assert(output[0] == 0x55);
  fake.mic_read_rc = H2_PAL_ERR_IO;
  assert(h2_pal_audio_mic_read(api, &frame, 0u) == H2_PAL_ERR_WOULD_BLOCK);
  assert(h2_app_test_audio_copy_evidence(audio, &evidence) == H2_PAL_OK);
  assert(evidence.real_capture_frames == 1u);
  assert(evidence.real_capture_first_error == H2_PAL_ERR_IO);
  assert(evidence.fixture_bytes_emitted == 0u && !evidence.fixture_complete);
  assert(evidence.mic_read_count == 0u);
  fake.mic_read_rc = H2_PAL_OK;

  h2_app_test_audio_set_capture_active(audio, true);
  assert(h2_pal_audio_mic_read(api, &frame, 0u) == H2_PAL_OK);
  assert(memcmp(output, pcm, sizeof(output)) == 0);
  h2_app_test_audio_set_capture_active(audio, true);
  assert(h2_pal_audio_mic_read(api, &frame, 0u) == H2_PAL_ERR_WOULD_BLOCK);

  /* Pause/resume with no intervening read preserves the next PCM frame and
   * resets pacing, instead of releasing a backlog after the long pause. */
  h2_app_test_audio_set_capture_active(audio, false);
  time.now_ms += 60000u;
  h2_app_test_audio_set_capture_active(audio, true);
  assert(h2_pal_audio_mic_read(api, &frame, 0u) == H2_PAL_OK);
  assert(memcmp(output, pcm + 320u, sizeof(output)) == 0);
  assert(h2_pal_audio_mic_read(api, &frame, 0u) == H2_PAL_ERR_WOULD_BLOCK);

  /* The observation callback can pause while the mic reader sleeps. */
  time.pause_on_sleep = audio;
  assert(h2_pal_audio_mic_read(api, &frame, 10u) == H2_PAL_ERR_WOULD_BLOCK);
  assert(frame.bytes == 0u);
  assert(h2_app_test_audio_copy_evidence(audio, &evidence) == H2_PAL_OK);
  assert(evidence.fixture_bytes_emitted == 640u && !evidence.fixture_complete);
  h2_app_test_audio_set_capture_active(audio, true);
  assert(h2_pal_audio_mic_read(api, &frame, 0u) == H2_PAL_OK);
  assert(memcmp(output, pcm + 640u, sizeof(output)) == 0);
  assert(h2_app_test_audio_copy_evidence(audio, &evidence) == H2_PAL_OK);
  assert(evidence.fixture_complete && evidence.fixture_bytes_emitted == 960u);

  /* EOF remains EOF across a pause/resume inside the pacing wait. */
  time.pause_on_sleep = audio;
  time.resume_on_sleep = true;
  assert(h2_pal_audio_mic_read(api, &frame, 10u) == H2_PAL_OK);
  for (size_t index = 0u; index < sizeof(output); ++index) {
    assert(output[index] == 0u);
  }
  assert(h2_pal_audio_mic_read(api, &frame, 0u) == H2_PAL_ERR_WOULD_BLOCK);

  /* A resident mic pump can change samples without stopping the delegate. */
  uint8_t replacement_pcm[320];
  memset(replacement_pcm, 4, sizeof(replacement_pcm));
  h2_app_test_audio_fixture_t replacement = {
      replacement_pcm, sizeof(replacement_pcm), fake.mic_format};
  assert(h2_app_test_audio_set_fixture(audio, &replacement) ==
         H2_PAL_ERR_INVALID_STATE);
  h2_app_test_audio_set_capture_active(audio, false);
  replacement.format.sample_rate_hz = 48000u;
  assert(h2_app_test_audio_set_fixture(audio, &replacement) == H2_PAL_ERR_FORMAT);
  assert(h2_app_test_audio_copy_evidence(audio, &evidence) == H2_PAL_OK);
  assert(evidence.fixture_complete && evidence.fixture_bytes_emitted == 960u);
  replacement.format = fake.mic_format;
  const uint64_t healthy_frames = evidence.real_capture_frames;
  assert(h2_app_test_audio_set_fixture(audio, &replacement) == H2_PAL_OK);
  assert(h2_app_test_audio_copy_evidence(audio, &evidence) == H2_PAL_OK);
  assert(evidence.fixture_bytes_emitted == 0u && !evidence.fixture_complete);
  assert(evidence.real_capture_frames == healthy_frames);
  assert(fake.start_mic == 1u && fake.stop_mic == 0u);
  assert(h2_pal_audio_mic_read(api, &frame, 0u) == H2_PAL_ERR_WOULD_BLOCK);
  h2_app_test_audio_set_capture_active(audio, true);
  assert(h2_pal_audio_mic_read(api, &frame, 0u) == H2_PAL_OK);
  assert(memcmp(output, replacement_pcm, sizeof(output)) == 0);

  /* Replacement while the reader is sleeping must rewind before returning. */
  time.pause_on_sleep = audio;
  time.replace_on_sleep = &fixture;
  assert(h2_pal_audio_mic_read(api, &frame, 10u) == H2_PAL_OK);
  assert(memcmp(output, pcm, sizeof(output)) == 0);
  assert(h2_pal_audio_mic_read(api, &frame, 0u) == H2_PAL_ERR_WOULD_BLOCK);
  assert(h2_app_test_audio_copy_evidence(audio, &evidence) == H2_PAL_OK);
  assert(evidence.fixture_bytes_emitted == 320u && !evidence.fixture_complete);

  h2_app_test_audio_set_capture_active(audio, false);
  assert(h2_pal_audio_stop_mic(api) == H2_PAL_OK);
  assert(h2_app_test_audio_set_fixture(audio, &fixture) == H2_PAL_OK);
  assert(h2_pal_audio_start_mic(api) == H2_PAL_OK);
  assert(h2_pal_audio_mic_read(api, &frame, 0u) == H2_PAL_ERR_WOULD_BLOCK);
  h2_app_test_audio_set_capture_active(audio, true);
  assert(h2_pal_audio_mic_read(api, &frame, 0u) == H2_PAL_OK);
  assert(memcmp(output, pcm, sizeof(output)) == 0);
  /* A delayed caller must not drain its missed frames in a catch-up burst. */
  time.now_ms += 10000u;
  assert(h2_pal_audio_mic_read(api, &frame, 0u) == H2_PAL_OK);
  assert(output[0] == 2u);
  assert(h2_pal_audio_mic_read(api, &frame, 0u) == H2_PAL_ERR_WOULD_BLOCK);
  time.now_ms += 10u;
  assert(h2_pal_audio_mic_read(api, &frame, 0u) == H2_PAL_OK);
  assert(output[0] == 3u);
  assert(h2_pal_audio_stop_mic(api) == H2_PAL_OK);
  assert(h2_app_test_audio_destroy(audio) == H2_PAL_OK);
}

int main(void) {
  test_capture_gate();
  const h2_pal_mem_api_t mem = {.vtable = &s_mem_vtable};
  fake_time_t fake_time = {.now_ms = 1000u};
  fake_time.api =
      (h2_pal_time_api_t){.user = &fake_time, .vtable = &s_time_vtable};
  fake_audio_t fake = {0};
  fake.mic_format =
      (h2_audio_pcm_format_t){16000u, 4u, 1u, H2_AUDIO_SAMPLE_S16LE};
  fake.api = (h2_pal_audio_api_t){.user = &fake, .vtable = &s_audio_vtable};
  const uint8_t fixture[] = {9u, 10u, 11u, 12u};
  h2_app_test_audio_fixture_t input = {fixture, sizeof(fixture),
                                       fake.mic_format};
  h2_app_test_audio_t *audio = NULL;
  assert(h2_app_test_audio_create(&mem, NULL, &fake.api, &input, &audio) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(audio == NULL);
  input.size = 3u;
  assert(h2_app_test_audio_create(&mem, &fake_time.api, &fake.api, &input,
                                  &audio) == H2_PAL_ERR_INVALID_ARG);
  input.size = sizeof(fixture);
  assert(h2_app_test_audio_create(&mem, &fake_time.api, &fake.api, &input,
                                  &audio) == H2_PAL_OK);
  const h2_pal_audio_api_t *api = h2_app_test_audio_api(audio);
  fake.mic_format.channels = 2u;
  assert(h2_pal_audio_start_mic(api) == H2_PAL_ERR_FORMAT);
  fake.mic_format.channels = 1u;
  assert(h2_app_test_audio_set_fixture(audio, &input) == H2_PAL_OK);
  assert(h2_pal_audio_start_mic(api) == H2_PAL_OK);
  assert(h2_app_test_audio_set_fixture(audio, &input) ==
         H2_PAL_ERR_INVALID_STATE);
  assert(h2_app_test_audio_destroy(audio) == H2_PAL_ERR_INVALID_STATE);
  h2_app_test_audio_evidence_t evidence;
  assert(h2_app_test_audio_copy_evidence(audio, &evidence) == H2_PAL_OK);
  assert(evidence.fixture_bytes_emitted == 0u);
  assert(!evidence.fixture_complete);
  uint8_t output[8] = {0};
  h2_audio_frame_t frame = h2_audio_frame_for_buffer(
      output, sizeof(output),
      (h2_audio_pcm_format_t){16000u, 4u, 1u, H2_AUDIO_SAMPLE_S16LE});
  assert(h2_pal_audio_mic_read(api, &frame, 100u) == H2_PAL_OK);
  assert(frame.bytes == sizeof(output));
  assert(memcmp(output, "\x09\x0a\x0b\x0c\x00\x00\x00\x00", 8u) == 0);
  assert(h2_app_test_audio_copy_evidence(audio, &evidence) == H2_PAL_OK);
  assert(evidence.fixture_bytes_emitted == sizeof(fixture));
  assert(evidence.fixture_complete);
  assert(h2_pal_audio_mic_read(api, &frame, 0u) == H2_PAL_ERR_WOULD_BLOCK);
  assert(frame.bytes == 0u);
  assert(h2_pal_audio_mic_read(api, &frame, 100u) == H2_PAL_OK);
  assert(frame.bytes == sizeof(output));
  assert(memcmp(output, "\x00\x00\x00\x00\x00\x00\x00\x00", 8u) == 0);
  assert(h2_pal_audio_stop_mic(api) == H2_PAL_OK);
  assert(h2_app_test_audio_set_fixture(audio, &input) == H2_PAL_OK);

  h2_pal_audio_track_t *track = NULL;
  const h2_audio_track_config_t track_config = {
      .name = "test",
      .format = {16000u, 4u, 1u, H2_AUDIO_SAMPLE_S16LE},
  };
  assert(h2_pal_audio_create_track(api, &track_config, &track) == H2_PAL_OK);
  assert(track->write(track, NULL, 100u) == H2_PAL_ERR_INVALID_ARG);
  h2_audio_frame_t invalid_frame = frame;
  invalid_frame.data = NULL;
  invalid_frame.bytes = invalid_frame.capacity;
  assert(track->write(track, &invalid_frame, 100u) == H2_PAL_ERR_INVALID_ARG);
  assert(fake.writes == 0u);
  frame.bytes = frame.capacity + 1u;
  assert(h2_pal_audio_track_write(track, &frame, 100u) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(fake.writes == 0u);
  frame.bytes = frame.capacity;
  assert(h2_pal_audio_track_write(track, &frame, 100u) == H2_PAL_OK);
  uint32_t volume = 0u;
  assert(h2_pal_audio_get_speaker_volume_percent(api, &volume) == H2_PAL_OK);
  assert(volume == 50u);
  assert(h2_pal_audio_set_speaker_volume_percent(api, 25u) == H2_PAL_OK);
  assert(h2_pal_audio_track_get_volume_factor(track, &volume) == H2_PAL_OK);
  assert(volume == 1000u);
  assert(h2_pal_audio_track_set_volume_factor(track, 500u) == H2_PAL_OK);
  assert(h2_pal_audio_track_drain(track, 100u) == H2_PAL_OK);
  assert(h2_pal_audio_track_close(track) == H2_PAL_OK);

  assert(h2_app_test_audio_copy_evidence(audio, &evidence) == H2_PAL_OK);
  assert(evidence.mic_start_count == 1u);
  assert(evidence.real_capture_frames == 3u);
  assert(evidence.mic_stop_count == 1u);
  assert(evidence.track_write_count == 1u);
  assert(evidence.track_drain_count == 1u);
  assert(evidence.track_close_count == 1u);
  assert(evidence.playback_bytes == sizeof(output));

  fake.mic_read_rc = H2_PAL_ERR_IO;
  assert(h2_pal_audio_start_mic(api) == H2_PAL_OK);
  assert(h2_pal_audio_mic_read(api, &frame, 100u) == H2_PAL_OK);
  assert(frame.bytes == sizeof(output));
  assert(h2_pal_audio_stop_mic(api) == H2_PAL_OK);
  assert(h2_app_test_audio_copy_evidence(audio, &evidence) == H2_PAL_OK);
  assert(evidence.real_capture_frames == 0u);
  assert(evidence.real_capture_first_error == H2_PAL_ERR_IO);
  assert(evidence.real_capture_last_error == H2_PAL_ERR_IO);
  fake.mic_read_rc = H2_PAL_OK;

  fake.mic_read_rc = H2_PAL_ERR_WOULD_BLOCK;
  assert(h2_pal_audio_start_mic(api) == H2_PAL_OK);
  assert(h2_pal_audio_mic_read(api, &frame, 100u) == H2_PAL_OK);
  assert(frame.bytes == sizeof(output));
  assert(h2_pal_audio_stop_mic(api) == H2_PAL_OK);
  assert(h2_app_test_audio_copy_evidence(audio, &evidence) == H2_PAL_OK);
  assert(evidence.real_capture_no_frame == 1u);
  fake.mic_read_rc = H2_PAL_OK;

  const uint64_t successful_stops = evidence.mic_stop_count;
  fake.stop_mic_rc = H2_PAL_ERR_IO;
  assert(h2_pal_audio_start_mic(api) == H2_PAL_OK);
  assert(h2_pal_audio_stop_mic(api) == H2_PAL_ERR_IO);
  assert(h2_app_test_audio_copy_evidence(audio, &evidence) == H2_PAL_OK);
  assert(evidence.mic_stop_count == successful_stops);
  fake.stop_mic_rc = H2_PAL_OK;
  assert(h2_pal_audio_stop_mic(api) == H2_PAL_OK);

  h2_pal_audio_track_t *failed_track = NULL;
  assert(h2_pal_audio_create_track(api, &track_config, &failed_track) ==
         H2_PAL_OK);
  const uint64_t successful_drains = evidence.track_drain_count;
  const uint64_t successful_closes = evidence.track_close_count;
  fake.drain_rc = H2_PAL_ERR_IO;
  assert(h2_pal_audio_track_drain(failed_track, 100u) == H2_PAL_ERR_IO);
  fake.close_rc = H2_PAL_ERR_IO;
  assert(h2_pal_audio_track_close(failed_track) == H2_PAL_ERR_IO);
  assert(h2_app_test_audio_copy_evidence(audio, &evidence) == H2_PAL_OK);
  assert(evidence.track_drain_count == successful_drains);
  assert(evidence.track_close_count == successful_closes);
  assert(evidence.active_track_count == 1u);
  fake.drain_rc = H2_PAL_OK;
  fake.close_rc = H2_PAL_OK;
  assert(h2_pal_audio_track_close(failed_track) == H2_PAL_OK);
  assert(h2_app_test_audio_copy_evidence(audio, &evidence) == H2_PAL_OK);
  assert(evidence.track_close_count == successful_closes + 1u);
  assert(evidence.active_track_count == 0u);

  h2_pal_audio_track_t *bounded[H2_APP_TEST_AUDIO_TRACKS_MAX] = {0};
  for (size_t index = 0u; index < H2_APP_TEST_AUDIO_TRACKS_MAX; ++index) {
    assert(h2_pal_audio_create_track(api, &track_config, &bounded[index]) ==
           H2_PAL_OK);
  }
  h2_pal_audio_track_t *overflow = NULL;
  assert(h2_pal_audio_create_track(api, &track_config, &overflow) ==
         H2_PAL_ERR_NO_SPACE);
  assert(overflow == NULL);
  assert(h2_app_test_audio_destroy(audio) == H2_PAL_ERR_INVALID_STATE);
  for (size_t index = 0u; index < H2_APP_TEST_AUDIO_TRACKS_MAX; ++index) {
    assert(h2_pal_audio_track_close(bounded[index]) == H2_PAL_OK);
  }
  assert(h2_app_test_audio_destroy(audio) == H2_PAL_OK);

  /* General PCM format and bounded pacing, independent of capture health. */
  fake.mic_format =
      (h2_audio_pcm_format_t){48000u, 480u, 2u, H2_AUDIO_SAMPLE_S16LE};
  input.format = fake.mic_format;
  assert(h2_app_test_audio_create(&mem, &fake_time.api, &fake.api, &input,
                                  &audio) == H2_PAL_OK);
  api = h2_app_test_audio_api(audio);
  uint8_t stereo[1920] = {0};
  frame = h2_audio_frame_for_buffer(stereo, sizeof(stereo), input.format);
  assert(h2_pal_audio_start_mic(api) == H2_PAL_OK);
  assert(h2_pal_audio_start_mic(api) == H2_PAL_ERR_INVALID_STATE);
  assert(h2_pal_audio_mic_read(api, &frame, 0u) == H2_PAL_OK);
  assert(frame.bytes == sizeof(stereo));
  assert(frame.channels == 2u && frame.sample_rate_hz == 48000u);
  assert(memcmp(stereo, fixture, sizeof(fixture)) == 0);
  uint64_t before = fake_time.now_ms;
  assert(h2_pal_audio_mic_read(api, &frame, 3u) == H2_PAL_ERR_TIMEOUT);
  assert(frame.bytes == 0u && fake_time.now_ms == before + 3u);
  assert(h2_pal_audio_mic_read(api, &frame, 7u) == H2_PAL_OK);
  assert(fake_time.now_ms == before + 10u);
  for (size_t index = 0; index < sizeof(stereo); ++index) {
    assert(stereo[index] == 0u);
  }
  assert(h2_pal_audio_stop_mic(api) == H2_PAL_OK);
  assert(h2_pal_audio_mic_read(api, &frame, 0u) == H2_PAL_ERR_INVALID_STATE);
  assert(h2_pal_audio_start_speaker(api) == H2_PAL_OK);
  assert(h2_app_test_audio_destroy(audio) == H2_PAL_ERR_INVALID_STATE);
  assert(h2_pal_audio_stop_speaker(api) == H2_PAL_OK);
  assert(h2_app_test_audio_destroy(audio) == H2_PAL_OK);

  /* An overflowing time epoch must not consume PCM on a failed read. */
  fake_time.now_ms = UINT64_MAX;
  assert(h2_app_test_audio_create(&mem, &fake_time.api, &fake.api, &input,
                                  &audio) == H2_PAL_OK);
  api = h2_app_test_audio_api(audio);
  assert(h2_pal_audio_start_mic(api) == H2_PAL_OK);
  assert(h2_pal_audio_mic_read(api, &frame, 0u) == H2_PAL_ERR_NO_SPACE);
  assert(frame.bytes == 0u);
  assert(h2_app_test_audio_copy_evidence(audio, &evidence) == H2_PAL_OK);
  assert(evidence.fixture_bytes_emitted == 0u && !evidence.fixture_complete);
  assert(h2_pal_audio_stop_mic(api) == H2_PAL_OK);
  assert(h2_app_test_audio_destroy(audio) == H2_PAL_OK);
  assert(h2_app_test_audio_destroy(NULL) == H2_PAL_OK);
  return 0;
}
