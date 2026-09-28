#include "h2_pal_audio_e2e.h"

#include <limits.h>
#include <string.h>

const char h2_pal_audio_e2e_runner_task_name[] = "pal-audio/e2e/runner";

static const char *const case_ids[] = {
#define H2_PAL_AUDIO_CASE(symbol, id) id,
#include "h2_pal_audio_cases.inc"
#undef H2_PAL_AUDIO_CASE
};

typedef struct state {
  const h2_pal_audio_e2e_config_t *config;
  h2_pal_audio_e2e_result_t *result;
  h2_audio_info_t info;
  h2_pal_audio_track_t *track;
  h2_audio_track_config_t track_config;
  int speaker_started;
  int mic_started;
  int saved_speaker_valid;
  int saved_gain_valid;
  uint32_t saved_speaker;
  uint32_t saved_gain;
  unsigned line;
  int16_t playback[4096];
  int16_t capture[4096];
} state_t;

static int reject(state_t *state, int detail, unsigned line) {
  state->line = line;
  return detail;
}
#define REQUIRE(state, condition)                                            \
  do {                                                                       \
    if (!(condition))                                                         \
      return reject((state), H2_PAL_ERR_INVALID_STATE, __LINE__);            \
  } while (0)
#define SUCCESS(state, call)                                                 \
  do {                                                                       \
    const int h2_audio_call_result = (call);                                  \
    if (h2_audio_call_result != H2_AUDIO_OK)                                   \
      return reject((state), h2_audio_call_result, __LINE__);                 \
  } while (0)

static size_t format_bytes(h2_audio_pcm_format_t format) {
  if (format.sample_rate_hz == 0u || format.frame_samples_per_channel == 0u ||
      format.channels == 0u || format.sample_format != H2_AUDIO_SAMPLE_S16LE)
    return 0u;
  return (size_t)format.frame_samples_per_channel * format.channels *
         sizeof(int16_t);
}

static int case_wrapper_arguments(state_t *state) {
  const h2_pal_audio_api_t *audio = state->config->audio;
  h2_audio_info_t info;
  uint32_t value = 777u;
  h2_pal_audio_track_t *track = (h2_pal_audio_track_t *)state;
  h2_audio_track_config_t config = {0};
  int16_t sample = 1;
  h2_audio_frame_t frame = h2_audio_frame_for_buffer(
      &sample, sizeof(sample), (h2_audio_pcm_format_t){16000u, 1u, 1u,
                                                       H2_AUDIO_SAMPLE_S16LE});
  frame.bytes = sizeof(sample);
  REQUIRE(state, h2_pal_audio_get_info(NULL, &info) == H2_AUDIO_ERR_INVALID_ARG);
  REQUIRE(state, h2_pal_audio_get_info(audio, NULL) == H2_AUDIO_ERR_INVALID_ARG);
  REQUIRE(state, h2_pal_audio_start_mic(NULL) == H2_AUDIO_ERR_INVALID_ARG);
  REQUIRE(state, h2_pal_audio_stop_mic(NULL) == H2_AUDIO_ERR_INVALID_ARG);
  REQUIRE(state, h2_pal_audio_start_speaker(NULL) == H2_AUDIO_ERR_INVALID_ARG);
  REQUIRE(state, h2_pal_audio_stop_speaker(NULL) == H2_AUDIO_ERR_INVALID_ARG);
  REQUIRE(state, h2_pal_audio_mic_read(audio, NULL, 0u) == H2_AUDIO_ERR_INVALID_ARG);
  frame.data = NULL;
  REQUIRE(state, h2_pal_audio_mic_read(audio, &frame, 0u) == H2_AUDIO_ERR_INVALID_ARG);
  frame.data = &sample;
  REQUIRE(state, h2_pal_audio_create_track(audio, NULL, &track) == H2_AUDIO_ERR_INVALID_ARG);
  REQUIRE(state, h2_pal_audio_create_track(audio, &config, NULL) == H2_AUDIO_ERR_INVALID_ARG);
  REQUIRE(state, h2_pal_audio_create_track(NULL, &config, &track) == H2_AUDIO_ERR_INVALID_ARG &&
                     track == (h2_pal_audio_track_t *)state);
  REQUIRE(state, h2_pal_audio_get_speaker_volume_percent(audio, NULL) == H2_AUDIO_ERR_INVALID_ARG);
  REQUIRE(state, h2_pal_audio_set_speaker_volume_percent(NULL, 50u) == H2_AUDIO_ERR_INVALID_ARG);
  REQUIRE(state, h2_pal_audio_get_mic_gain_percent(audio, NULL) == H2_AUDIO_ERR_INVALID_ARG);
  REQUIRE(state, h2_pal_audio_get_mic_gain_percent(NULL, &value) == H2_AUDIO_ERR_INVALID_ARG &&
                     value == 0u);
  REQUIRE(state, h2_pal_audio_set_mic_gain_percent(NULL, 50u) == H2_AUDIO_ERR_INVALID_ARG);
  REQUIRE(state, h2_pal_audio_track_write(NULL, &frame, 0u) == H2_AUDIO_ERR_INVALID_ARG);
  REQUIRE(state, h2_pal_audio_track_close(NULL) == H2_AUDIO_ERR_INVALID_ARG);
  REQUIRE(state, h2_pal_audio_track_drain(NULL, 0u) == H2_AUDIO_ERR_INVALID_ARG);
  REQUIRE(state, h2_pal_audio_track_get_volume_factor(NULL, &value) == H2_AUDIO_ERR_INVALID_ARG);
  REQUIRE(state, h2_pal_audio_track_set_volume_factor(NULL, 500u) == H2_AUDIO_ERR_INVALID_ARG);
  return H2_AUDIO_OK;
}

static int case_info(state_t *state) {
  const h2_pal_audio_api_t *audio = state->config->audio;
  if (audio == NULL || audio->vtable == NULL ||
      audio->vtable->get_info == NULL || audio->vtable->start_mic == NULL ||
      audio->vtable->stop_mic == NULL || audio->vtable->start_speaker == NULL ||
      audio->vtable->stop_speaker == NULL || audio->vtable->mic_read == NULL ||
      audio->vtable->create_track == NULL ||
      audio->vtable->get_speaker_volume_percent == NULL ||
      audio->vtable->set_speaker_volume_percent == NULL ||
      audio->vtable->get_mic_gain_percent == NULL ||
      audio->vtable->set_mic_gain_percent == NULL)
    return reject(state, H2_AUDIO_ERR_UNSUPPORTED, __LINE__);
  SUCCESS(state, h2_pal_audio_get_info(audio, &state->info));
  REQUIRE(state, state->info.available && state->info.mic_supported &&
                     state->info.playback_supported);
  REQUIRE(state, format_bytes(state->info.mic_format) > 0u &&
                     format_bytes(state->info.mic_format) <= sizeof(state->capture));
  REQUIRE(state, format_bytes(state->info.playback_format) > 0u &&
                     format_bytes(state->info.playback_format) <= sizeof(state->playback));
  REQUIRE(state, state->info.mic_queue_frames > 0u &&
                     state->info.track_queue_frames > 0u &&
                     state->info.max_tracks > 0u);
  return H2_AUDIO_OK;
}

static h2_audio_frame_t playback_frame(state_t *state) {
  h2_audio_frame_t frame = h2_audio_frame_for_buffer(
      state->playback, sizeof(state->playback), state->info.playback_format);
  frame.bytes = format_bytes(state->info.playback_format);
  return frame;
}

static h2_audio_frame_t capture_frame(state_t *state) {
  return h2_audio_frame_for_buffer(state->capture, sizeof(state->capture),
                                   state->info.mic_format);
}

static int case_mic_stopped_read(state_t *state) {
  h2_audio_frame_t frame = capture_frame(state);
  const int rc = h2_pal_audio_mic_read(state->config->audio, &frame, 0u);
  REQUIRE(state, (rc == H2_AUDIO_ERR_INVALID_STATE ||
                  rc == H2_AUDIO_ERR_WOULD_BLOCK ||
                  rc == H2_PAL_ERR_CLOSED) && frame.bytes == 0u);
  return H2_AUDIO_OK;
}

static int case_mic_gain_bounds(state_t *state) {
  uint32_t value = 999u;
  REQUIRE(state, h2_pal_audio_set_mic_gain_percent(state->config->audio, 101u) ==
                     H2_AUDIO_ERR_INVALID_ARG);
  SUCCESS(state, h2_pal_audio_get_mic_gain_percent(state->config->audio, &value));
  REQUIRE(state, value <= 100u);
  state->saved_gain = value;
  state->saved_gain_valid = 1;
  return H2_AUDIO_OK;
}

static int case_mic_gain_roundtrip(state_t *state) {
  uint32_t value = 999u;
  SUCCESS(state, h2_pal_audio_set_mic_gain_percent(state->config->audio, 30u));
  SUCCESS(state, h2_pal_audio_get_mic_gain_percent(state->config->audio, &value));
  REQUIRE(state, value <= 100u && value >= 20u && value <= 40u);
  SUCCESS(state, h2_pal_audio_set_mic_gain_percent(state->config->audio, 80u));
  SUCCESS(state, h2_pal_audio_get_mic_gain_percent(state->config->audio, &value));
  REQUIRE(state, value <= 100u && value >= 70u && value <= 90u);
  return H2_AUDIO_OK;
}

static int case_speaker_volume_bounds(state_t *state) {
  uint32_t value = 999u;
  SUCCESS(state, h2_pal_audio_get_speaker_volume_percent(state->config->audio, &value));
  REQUIRE(state, value <= 100u);
  state->saved_speaker = value;
  state->saved_speaker_valid = 1;
  REQUIRE(state, h2_pal_audio_set_speaker_volume_percent(state->config->audio, 101u) ==
                     H2_AUDIO_ERR_INVALID_ARG);
  return H2_AUDIO_OK;
}

static int case_speaker_volume_roundtrip(state_t *state) {
  uint32_t value = 999u;
  SUCCESS(state, h2_pal_audio_set_speaker_volume_percent(state->config->audio, 20u));
  SUCCESS(state, h2_pal_audio_get_speaker_volume_percent(state->config->audio, &value));
  REQUIRE(state, value >= 10u && value <= 30u);
  SUCCESS(state, h2_pal_audio_set_speaker_volume_percent(state->config->audio, 40u));
  SUCCESS(state, h2_pal_audio_get_speaker_volume_percent(state->config->audio, &value));
  REQUIRE(state, value >= 30u && value <= 50u);
  return H2_AUDIO_OK;
}

static int case_track_format_bounds(state_t *state) {
  h2_audio_track_config_t config = {
      .name = "pal-audio-e2e", .format = state->info.playback_format,
      .volume_factor_milli = 300u, .buffer_frames = 4u};
  h2_pal_audio_track_t *track = (h2_pal_audio_track_t *)state;
  config.format.sample_format = (h2_audio_sample_format_t)0;
  const int rc = h2_pal_audio_create_track(state->config->audio, &config, &track);
  REQUIRE(state, rc != H2_AUDIO_OK && track == NULL);
  return H2_AUDIO_OK;
}

static int case_speaker_start(state_t *state) {
  SUCCESS(state, h2_pal_audio_start_speaker(state->config->audio));
  state->speaker_started = 1;
  const int repeated = h2_pal_audio_start_speaker(state->config->audio);
  REQUIRE(state, repeated == H2_AUDIO_OK || repeated == H2_AUDIO_ERR_INVALID_STATE);
  return H2_AUDIO_OK;
}

static int case_track_create(state_t *state) {
  state->track_config = (h2_audio_track_config_t){
      .name = "pal-audio-e2e", .format = state->info.playback_format,
      .volume_factor_milli = 300u, .buffer_frames = 4u};
  SUCCESS(state, h2_pal_audio_create_track(state->config->audio,
                                            &state->track_config, &state->track));
  REQUIRE(state, state->track != NULL && state->track->audio != NULL &&
                     state->track->write != NULL && state->track->drain != NULL &&
                     state->track->close != NULL &&
                     state->track->get_volume_factor != NULL &&
                     state->track->set_volume_factor != NULL);
  return H2_AUDIO_OK;
}

static int case_track_volume_bounds(state_t *state) {
  REQUIRE(state, state->track != NULL);
  REQUIRE(state, h2_pal_audio_track_get_volume_factor(state->track, NULL) ==
                     H2_AUDIO_ERR_INVALID_ARG);
  /* Zero is a valid mute boundary. Boost above 1000 is provider-specific. */
  SUCCESS(state, h2_pal_audio_track_set_volume_factor(state->track, 0u));
  uint32_t value = 9999u;
  SUCCESS(state, h2_pal_audio_track_get_volume_factor(state->track, &value));
  REQUIRE(state, value == 0u);
  SUCCESS(state, h2_pal_audio_track_set_volume_factor(state->track, 300u));
  return H2_AUDIO_OK;
}

static int case_track_volume_roundtrip(state_t *state) {
  uint32_t value = 9999u;
  REQUIRE(state, state->track != NULL);
  SUCCESS(state, h2_pal_audio_track_get_volume_factor(state->track, &value));
  REQUIRE(state, value == 300u);
  SUCCESS(state, h2_pal_audio_track_set_volume_factor(state->track, 500u));
  SUCCESS(state, h2_pal_audio_track_get_volume_factor(state->track, &value));
  REQUIRE(state, value == 500u);
  return H2_AUDIO_OK;
}

static int case_track_write_bounds(state_t *state) {
  REQUIRE(state, state->track != NULL);
  h2_audio_frame_t frame = playback_frame(state);
  frame.bytes = 0u;
  REQUIRE(state, h2_pal_audio_track_write(state->track, &frame, 0u) ==
                     H2_AUDIO_ERR_INVALID_ARG);
  frame = playback_frame(state);
  frame.sample_rate_hz++;
  REQUIRE(state, h2_pal_audio_track_write(state->track, &frame, 0u) != H2_AUDIO_OK);
  frame = playback_frame(state);
  frame.bytes = frame.capacity + 2u;
  REQUIRE(state, h2_pal_audio_track_write(state->track, &frame, 0u) != H2_AUDIO_OK);
  return H2_AUDIO_OK;
}

static void fill_playback(state_t *state) {
  const size_t samples = format_bytes(state->info.playback_format) / sizeof(int16_t);
  for (size_t i = 0u; i < samples; ++i) {
    const int phase = (int)(i % 64u);
    const int amplitude = phase < 32 ? phase * 72 - 1120 : (63 - phase) * 72 - 1120;
    state->playback[i] = (int16_t)amplitude;
    const uint32_t absolute = (uint32_t)(amplitude < 0 ? -amplitude : amplitude);
    if (absolute > state->result->output_peak)
      state->result->output_peak = absolute;
  }
}

static int case_track_write(state_t *state) {
  REQUIRE(state, state->track != NULL);
  fill_playback(state);
  h2_audio_frame_t frame = playback_frame(state);
  for (unsigned i = 0u; i < 4u; ++i) {
    SUCCESS(state, h2_pal_audio_track_write(state->track, &frame, 1000u));
    ++state->result->speaker_frames;
  }
  REQUIRE(state, state->result->output_peak > 1000u);
  return H2_AUDIO_OK;
}

static int case_track_drain(state_t *state) {
  REQUIRE(state, state->track != NULL);
  const int nonblocking = h2_pal_audio_track_drain(state->track, 0u);
  REQUIRE(state, nonblocking == H2_AUDIO_OK ||
                     nonblocking == H2_AUDIO_ERR_WOULD_BLOCK ||
                     nonblocking == H2_PAL_ERR_TIMEOUT);
  SUCCESS(state, h2_pal_audio_track_drain(state->track, 3000u));
  return H2_AUDIO_OK;
}

static int case_mic_start(state_t *state) {
  SUCCESS(state, h2_pal_audio_start_mic(state->config->audio));
  state->mic_started = 1;
  const int repeated = h2_pal_audio_start_mic(state->config->audio);
  REQUIRE(state, repeated == H2_AUDIO_OK || repeated == H2_AUDIO_ERR_INVALID_STATE);
  return H2_AUDIO_OK;
}

static int read_mic_frame(state_t *state, uint32_t timeout_ms) {
  h2_audio_frame_t frame = capture_frame(state);
  const int rc = h2_pal_audio_mic_read(state->config->audio, &frame, timeout_ms);
  if (rc != H2_AUDIO_OK)
    return reject(state, rc, __LINE__);
  REQUIRE(state, frame.bytes > 0u && frame.bytes <= frame.capacity &&
                     frame.bytes % (sizeof(int16_t) * frame.channels) == 0u &&
                     frame.sample_rate_hz == state->info.mic_format.sample_rate_hz &&
                     frame.channels == state->info.mic_format.channels &&
                     frame.sample_format == H2_AUDIO_SAMPLE_S16LE);
  ++state->result->mic_frames;
  const size_t count = frame.bytes / sizeof(int16_t);
  for (size_t i = 0u; i < count; ++i) {
    const int32_t sample = state->capture[i];
    const uint32_t magnitude = (uint32_t)(sample < 0 ? -sample : sample);
    if (magnitude > state->result->mic_peak)
      state->result->mic_peak = magnitude;
    state->result->mic_energy += (uint64_t)magnitude * magnitude;
  }
  return H2_AUDIO_OK;
}

static int case_mic_read(state_t *state) {
  REQUIRE(state, state->mic_started);
  h2_audio_frame_t immediate = capture_frame(state);
  const int nonblocking = h2_pal_audio_mic_read(state->config->audio,
                                                 &immediate, 0u);
  REQUIRE(state, nonblocking == H2_AUDIO_OK ||
                     nonblocking == H2_AUDIO_ERR_WOULD_BLOCK ||
                     nonblocking == H2_PAL_ERR_TIMEOUT);
  REQUIRE(state, nonblocking == H2_AUDIO_OK ? immediate.bytes > 0u
                                            : immediate.bytes == 0u);
  int16_t guard[2] = {123, 456};
  h2_audio_frame_t tiny = h2_audio_frame_for_buffer(
      guard, 1u, state->info.mic_format);
  const int small = h2_pal_audio_mic_read(state->config->audio, &tiny, 1000u);
  REQUIRE(state, small == H2_AUDIO_ERR_INVALID_ARG || small == H2_PAL_ERR_NO_SPACE);
  REQUIRE(state, guard[0] == 123 && guard[1] == 456);
  return read_mic_frame(state, 3000u);
}

static int case_mic_gain_live(state_t *state) {
  REQUIRE(state, state->mic_started);
  uint32_t value = 999u;
  SUCCESS(state, h2_pal_audio_set_mic_gain_percent(state->config->audio, 60u));
  SUCCESS(state, h2_pal_audio_get_mic_gain_percent(state->config->audio, &value));
  REQUIRE(state, value >= 50u && value <= 70u);
  return read_mic_frame(state, 3000u);
}

static int case_mic_stop(state_t *state) {
  REQUIRE(state, state->mic_started);
  SUCCESS(state, h2_pal_audio_stop_mic(state->config->audio));
  state->mic_started = 0;
  const int repeated = h2_pal_audio_stop_mic(state->config->audio);
  REQUIRE(state, repeated == H2_AUDIO_OK || repeated == H2_AUDIO_ERR_INVALID_STATE);
  return H2_AUDIO_OK;
}

static int case_track_close(state_t *state) {
  REQUIRE(state, state->track != NULL);
  SUCCESS(state, h2_pal_audio_track_close(state->track));
  state->track = NULL;
  return H2_AUDIO_OK;
}

static int case_speaker_stop(state_t *state) {
  REQUIRE(state, state->speaker_started);
  SUCCESS(state, h2_pal_audio_stop_speaker(state->config->audio));
  state->speaker_started = 0;
  const int repeated = h2_pal_audio_stop_speaker(state->config->audio);
  REQUIRE(state, repeated == H2_AUDIO_OK || repeated == H2_AUDIO_ERR_INVALID_STATE);
  return H2_AUDIO_OK;
}

static int case_resource_churn(state_t *state) {
  for (unsigned i = 0u; i < 3u; ++i) {
    SUCCESS(state, h2_pal_audio_start_speaker(state->config->audio));
    state->speaker_started = 1;
    SUCCESS(state, h2_pal_audio_create_track(state->config->audio,
                                              &state->track_config, &state->track));
    REQUIRE(state, state->track != NULL);
    SUCCESS(state, h2_pal_audio_track_close(state->track));
    state->track = NULL;
    SUCCESS(state, h2_pal_audio_stop_speaker(state->config->audio));
    state->speaker_started = 0;
    SUCCESS(state, h2_pal_audio_start_mic(state->config->audio));
    state->mic_started = 1;
    SUCCESS(state, h2_pal_audio_stop_mic(state->config->audio));
    state->mic_started = 0;
  }
  return H2_AUDIO_OK;
}

static int case_stability(state_t *state) {
  uint64_t start = 0u, now = 0u;
  const uint32_t duration = state->config->stability_ms == 0u ? 30000u
                                                                : state->config->stability_ms;
  SUCCESS(state, h2_pal_time_get_monotonic_ms(state->config->time, &start));
  SUCCESS(state, h2_pal_audio_start_speaker(state->config->audio));
  state->speaker_started = 1;
  SUCCESS(state, h2_pal_audio_create_track(state->config->audio,
                                            &state->track_config, &state->track));
  REQUIRE(state, state->track != NULL);
  SUCCESS(state, h2_pal_audio_start_mic(state->config->audio));
  state->mic_started = 1;
  fill_playback(state);
  h2_audio_frame_t frame = playback_frame(state);
  unsigned rounds = 0u;
  do {
    SUCCESS(state, h2_pal_audio_track_write(state->track, &frame, 1000u));
    ++state->result->speaker_frames;
    SUCCESS(state, read_mic_frame(state, 1000u));
    SUCCESS(state, h2_pal_time_get_monotonic_ms(state->config->time, &now));
    ++rounds;
    REQUIRE(state, rounds < 10000u);
  } while (now - start < duration);
  REQUIRE(state, rounds >= 2u && now - start >= duration);
  state->result->stability_elapsed_ms = now - start;
  SUCCESS(state, h2_pal_audio_track_drain(state->track, 3000u));
  SUCCESS(state, h2_pal_audio_stop_mic(state->config->audio));
  state->mic_started = 0;
  SUCCESS(state, h2_pal_audio_track_close(state->track));
  state->track = NULL;
  SUCCESS(state, h2_pal_audio_stop_speaker(state->config->audio));
  state->speaker_started = 0;
  return H2_AUDIO_OK;
}

static int restore(state_t *state) {
  int failure = H2_AUDIO_OK;
  if (state->mic_started) {
    const int rc = h2_pal_audio_stop_mic(state->config->audio);
    if (rc != H2_AUDIO_OK && failure == H2_AUDIO_OK) failure = rc;
    state->mic_started = 0;
  }
  if (state->track != NULL) {
    const int rc = h2_pal_audio_track_close(state->track);
    if (rc != H2_AUDIO_OK && failure == H2_AUDIO_OK) failure = rc;
    state->track = NULL;
  }
  if (state->speaker_started) {
    const int rc = h2_pal_audio_stop_speaker(state->config->audio);
    if (rc != H2_AUDIO_OK && failure == H2_AUDIO_OK) failure = rc;
    state->speaker_started = 0;
  }
  if (state->saved_gain_valid) {
    const int rc = h2_pal_audio_set_mic_gain_percent(state->config->audio,
                                                      state->saved_gain);
    if (rc != H2_AUDIO_OK && failure == H2_AUDIO_OK) failure = rc;
  }
  if (state->saved_speaker_valid) {
    const int rc = h2_pal_audio_set_speaker_volume_percent(state->config->audio,
                                                             state->saved_speaker);
    if (rc != H2_AUDIO_OK && failure == H2_AUDIO_OK) failure = rc;
  }
  return failure;
}

static int case_cleanup(state_t *state) {
  const int rc = restore(state);
  if (rc != H2_AUDIO_OK) return reject(state, rc, __LINE__);
  if (state->saved_gain_valid) {
    uint32_t gain = 999u;
    SUCCESS(state, h2_pal_audio_get_mic_gain_percent(state->config->audio, &gain));
    REQUIRE(state, gain <= 100u && gain + 10u >= state->saved_gain &&
                       gain <= state->saved_gain + 10u);
  }
  if (state->saved_speaker_valid) {
    uint32_t volume = 999u;
    SUCCESS(state, h2_pal_audio_get_speaker_volume_percent(state->config->audio, &volume));
    REQUIRE(state, volume <= 100u && volume + 10u >= state->saved_speaker &&
                       volume <= state->saved_speaker + 10u);
  }
  return H2_AUDIO_OK;
}

typedef int (*case_fn_t)(state_t *state);
static const case_fn_t case_functions[] = {
    case_wrapper_arguments, case_info, case_mic_stopped_read,
    case_mic_gain_bounds, case_mic_gain_roundtrip,
    case_speaker_volume_bounds, case_speaker_volume_roundtrip,
    case_track_format_bounds, case_speaker_start, case_track_create,
    case_track_volume_bounds, case_track_volume_roundtrip,
    case_track_write_bounds, case_track_write, case_track_drain,
    case_mic_start, case_mic_read, case_mic_gain_live, case_mic_stop,
    case_track_close, case_speaker_stop, case_resource_churn,
    case_stability, case_cleanup};

int h2_pal_audio_e2e_run(const h2_pal_audio_e2e_config_t *config,
                         h2_pal_audio_e2e_result_t *out_result) {
  if (config == NULL || out_result == NULL || config->audio == NULL)
    return H2_AUDIO_ERR_INVALID_ARG;
  memset(out_result, 0, sizeof(*out_result));
  state_t state = {.config = config, .result = out_result};
  const size_t count = sizeof(case_functions) / sizeof(case_functions[0]);
  if (count != H2_PAL_AUDIO_E2E_CASE_COUNT)
    return H2_PAL_ERR_INVALID_STATE;
  int dependency_failed = 0;
  for (size_t i = 0u; i < count; ++i) {
    int rc;
    if (dependency_failed && i != H2_PAL_AUDIO_E2E_CLEANUP) {
      rc = H2_PAL_ERR_UNAVAILABLE;
    } else {
      state.line = 0u;
      rc = case_functions[i](&state);
      if (rc != H2_AUDIO_OK && i != H2_PAL_AUDIO_E2E_CLEANUP)
        dependency_failed = 1;
    }
    h2_pal_audio_e2e_case_result_t *item = &out_result->cases[i];
    *item = (h2_pal_audio_e2e_case_result_t){
        .id = case_ids[i], .passed = rc == H2_AUDIO_OK,
        .blocked = rc == H2_PAL_ERR_UNAVAILABLE || rc == H2_PAL_ERR_UNSUPPORTED,
        .detail = rc, .line = state.line};
    if (item->passed) ++out_result->passed;
    else if (item->blocked) ++out_result->blocked;
    else ++out_result->failed;
    if (config->report != NULL)
      config->report(config->report_user, item);
  }
  (void)restore(&state);
  return out_result->failed == 0u && out_result->blocked == 0u &&
                 out_result->passed == count
             ? H2_AUDIO_OK : H2_PAL_ERR_INVALID_STATE;
}
