#include "h2_fruit_slice_audio.h"

#include "h2_fruit_slice.h"
#include "h2_fruit_slice_audio_bank.h"
#include "h2_fruit_slice_audio_worker.h"

#include <stdio.h>
#include <string.h>

#define PREF_KEY "audio_v1"
#define PREF_MAGIC 0x41550100u
#define POSITION_ONE 65536u
#define GAIN_ONE 65536

static void report_mute(h2_fruit_slice_audio_t* audio, const char* what,
                        int rc);

static unsigned priority(unsigned sound) {
  static const uint8_t priorities[H2_FRUIT_SLICE_SOUND_COUNT] = {
      1, 2, 6, 10, 3, 7, 7, 2, 6, 4, 4, 5, 8, 5};
  if (h2_fruit_slice_sound_is_hit((h2_fruit_slice_sound_t)sound))
    sound = H2_FRUIT_SLICE_SOUND_SLICE;
  return priorities[sound];
}

static uint8_t load_mute(h2_runtime_t* runtime) {
  h2_pal_pref_namespace_t* ns = NULL;
  uint32_t value = 0;
  if (h2_pal_pref_open(runtime->pref, H2_FRUIT_SLICE_PREF_NAMESPACE,
                       H2_PAL_PREF_OPEN_READ_ONLY, &ns) == H2_PAL_OK &&
      ns) {
    if (ns->get_u32 == NULL || ns->get_u32(ns, PREF_KEY, &value) != H2_PAL_OK)
      value = 0;
    if (ns->close != NULL) (void)ns->close(ns);
  }
  return (value & ~3u) == PREF_MAGIC ? (uint8_t)(value & 3u) : 0;
}

int h2_fruit_slice_audio_init(h2_fruit_slice_audio_t* audio,
                              h2_runtime_t* runtime) {
  if (audio == NULL || runtime == NULL) return H2_PAL_ERR_INVALID_ARG;
  memset(audio, 0, sizeof(*audio));
  audio->runtime = runtime;
  audio->mute = load_mute(runtime);
  audio->sfx_gain = (audio->mute & H2_FRUIT_SLICE_MUTE_SFX) ? 0 : GAIN_ONE;
  audio->music_gain = (audio->mute & H2_FRUIT_SLICE_MUTE_MUSIC) ? 0 : GAIN_ONE;
  h2_audio_info_t info = {0};
  int rc = h2_pal_audio_get_info(runtime->audio, &info);
  if (rc != H2_PAL_OK) return rc;
  audio->format = info.playback_format;
  const h2_audio_pcm_format_t* format = &audio->format;
  if (!info.available || !info.playback_supported ||
      format->sample_format != H2_AUDIO_SAMPLE_S16LE ||
      format->sample_rate_hz < 8000u || format->sample_rate_hz > 48000u ||
      format->channels == 0u || format->channels > 2u ||
      format->frame_samples_per_channel == 0u ||
      format->frame_samples_per_channel > H2_FRUIT_SLICE_AUDIO_MAX_SAMPLES)
    return H2_PAL_ERR_UNSUPPORTED;
  const size_t bytes = (size_t)format->frame_samples_per_channel *
                       format->channels * sizeof(int16_t);
  audio->pcm = h2_pal_mem_alloc(runtime->mem, bytes);
  if (!audio->pcm) return H2_PAL_ERR_NO_MEMORY;
  /* Queue capacity is an upper bound on how much accepted PCM can remain.
   * Allow the provider's scheduling cushion, but never wait indefinitely for
   * an unavailable consumer (including a suspended Web AudioContext). */
  uint64_t budget =
      (uint64_t)format->frame_samples_per_channel * 1000u *
          (info.track_queue_frames > 2u ? info.track_queue_frames : 2u) /
          format->sample_rate_hz +
      100u;
  audio->mute_timeout_ms = (uint32_t)(budget < 200u    ? 200u
                                      : budget > 1000u ? 1000u
                                                       : budget);
  audio->available = true;
  return H2_PAL_OK;
}

void h2_fruit_slice_audio_emit(h2_fruit_slice_audio_t* audio,
                               h2_fruit_slice_sound_t sound, uint64_t now_ms) {
  if (audio == NULL || !audio->available ||
      (unsigned)sound >= H2_FRUIT_SLICE_SOUND_COUNT ||
      (audio->mute & H2_FRUIT_SLICE_MUTE_SFX))
    return;
  if (audio->worker != NULL) {
    h2_fruit_slice_audio_worker_emit(audio, sound, now_ms);
    return;
  }
  const bool hit = h2_fruit_slice_sound_is_hit(sound);
  const unsigned cooldown_slot =
      hit ? H2_FRUIT_SLICE_SOUND_SLICE : (unsigned)sound;
  const uint32_t bit = 1u << cooldown_slot;
  const uint32_t cooldown = hit                                   ? 35u
                            : sound == H2_FRUIT_SLICE_SOUND_SWING ? 100u
                                                                  : 180u;
  if ((audio->seen & bit) && now_ms >= audio->last_sound_ms[cooldown_slot] &&
      now_ms - audio->last_sound_ms[cooldown_slot] < cooldown)
    return;
  audio->seen |= bit;
  audio->last_sound_ms[cooldown_slot] = now_ms;
  if (sound == H2_FRUIT_SLICE_SOUND_BOMB)
    memset(audio->voices, 0, sizeof(audio->voices));
  unsigned slot = 0;
  for (unsigned i = 0; i < H2_FRUIT_SLICE_AUDIO_VOICES; ++i) {
    if (!audio->voices[i].active) {
      slot = i;
      break;
    }
    if (priority(audio->voices[i].sound) < priority(audio->voices[slot].sound))
      slot = i;
  }
  if (audio->voices[slot].active &&
      priority(audio->voices[slot].sound) > priority((unsigned)sound))
    return;
  audio->voices[slot] =
      (h2_fruit_slice_audio_voice_t){.sound = (uint8_t)sound, .active = true};
}

int h2_fruit_slice_audio_suspend(h2_fruit_slice_audio_t* audio) {
  if (audio == NULL) return H2_PAL_ERR_INVALID_ARG;
  const int stopped = h2_fruit_slice_audio_worker_stop(audio);
  if (stopped != H2_PAL_OK) return stopped;
  int first_error = H2_PAL_OK;
  if (audio->track != NULL) {
    first_error = h2_pal_audio_track_close(audio->track);
    /* Runtime track wrappers are consumed by close, including on error. */
    audio->track = NULL;
  }
  if (audio->speaker_started) {
    const int rc = h2_pal_audio_stop_speaker(audio->runtime->audio);
    if (rc == H2_PAL_OK) audio->speaker_started = false;
    if (first_error == H2_PAL_OK) first_error = rc;
  }
  audio->closing = audio->track != NULL || audio->speaker_started;
  /* Suspension can interrupt a queued full-mute command before its first
   * accepted frame. Once hardware is quiescent there is no remaining fade to
   * submit on resume; prepare deliberately does not open a fully muted track.
   */
  if (!audio->closing && audio->mute == 3u) {
    audio->sfx_gain = audio->music_gain = 0;
    audio->sfx_step = audio->music_step = 0;
    audio->ramp_left = 0u;
  }
  audio->mute_fade_after_ms = audio->mute_drain_after_ms = 0u;
  memset(audio->voices, 0, sizeof(audio->voices));
  audio->seen = 0;
  audio->started_ms = audio->submitted_samples = 0;
  audio->pumped = audio->timeline_started = false;
  return first_error;
}

int h2_fruit_slice_audio_save_mute(h2_runtime_t* runtime, uint8_t mute) {
  h2_pal_pref_namespace_t* ns = NULL;
  int rc = h2_pal_pref_open(runtime->pref, H2_FRUIT_SLICE_PREF_NAMESPACE,
                            H2_PAL_PREF_OPEN_READ_WRITE, &ns);
  if (rc != H2_PAL_OK || ns == NULL)
    return rc != H2_PAL_OK ? rc : H2_PAL_ERR_IO;
  rc = ns->set_u32 != NULL ? ns->set_u32(ns, PREF_KEY, PREF_MAGIC | mute)
                           : H2_PAL_ERR_UNSUPPORTED;
  if (rc == H2_PAL_OK && ns->commit != NULL) rc = ns->commit(ns);
  if (ns->close != NULL) {
    int closed = ns->close(ns);
    if (rc == H2_PAL_OK) rc = closed;
  }
  return rc;
}

void h2_fruit_slice_audio_apply_mute(h2_fruit_slice_audio_t* audio,
                                     uint8_t mute) {
  if (audio->mute == mute) return;
  audio->mute = mute;
  audio->mute_fade_after_ms = audio->mute_drain_after_ms = 0u;
  const uint32_t count = audio->format.sample_rate_hz / 100u;
  audio->ramp_left = count != 0u ? count : 1u; /* 10 ms on a usable format. */
  if (mute == 3u && audio->track == NULL && !audio->speaker_started) {
    audio->sfx_gain = audio->music_gain = 0;
    audio->ramp_left = 0u;
    return;
  }
  const int32_t sfx = (mute & H2_FRUIT_SLICE_MUTE_SFX) ? 0 : GAIN_ONE;
  const int32_t music = (mute & H2_FRUIT_SLICE_MUTE_MUSIC) ? 0 : GAIN_ONE;
  audio->sfx_step = (sfx - audio->sfx_gain) / (int32_t)audio->ramp_left;
  audio->music_step = (music - audio->music_gain) / (int32_t)audio->ramp_left;
}

int h2_fruit_slice_audio_set_mute(h2_fruit_slice_audio_t* audio, uint8_t mute) {
  if (audio == NULL || mute > 3u) return H2_PAL_ERR_INVALID_ARG;
  if (audio->mute == mute && !audio->pref_dirty) return H2_PAL_OK;
  if (audio->available && audio->worker_requested && audio->worker == NULL) {
    const int rc = h2_fruit_slice_audio_worker_start(audio);
    if (rc != H2_PAL_OK) {
      report_mute(audio, "WORKER_FAILED", rc);
      /* Playback is optional; settings are not conditional on producer start.
       * Match the App's disabled-audio policy instead of allocating each frame.
       */
      const int closed = h2_fruit_slice_audio_suspend(audio);
      if (closed != H2_PAL_OK) report_mute(audio, "MUTE_CLOSE_RETRY", closed);
      audio->available = false;
    }
  }
  if (audio->worker != NULL)
    return h2_fruit_slice_audio_worker_mute(audio, mute);
  uint64_t now_ms = 0u;
  (void)h2_pal_time_get_monotonic_ms(audio->runtime->time, &now_ms);
  if (audio->mute == mute && now_ms < audio->pref_retry_ms) return H2_PAL_OK;
  h2_fruit_slice_audio_apply_mute(audio, mute);
  audio->pref_dirty = true;
  audio->pref_retry_ms = now_ms + 500u;
  const int rc = h2_fruit_slice_audio_save_mute(audio->runtime, mute);
  if (rc == H2_PAL_OK || rc == H2_PAL_ERR_UNSUPPORTED)
    audio->pref_dirty = false;
  return rc;
}

static int sample(const h2_fruit_slice_audio_clip_t* clip, uint32_t position) {
  const uint32_t index = position / POSITION_ONE;
  if (index >= clip->count) return 0;
  const int a = clip->samples[index];
  const int b = index + 1u < clip->count ? clip->samples[index + 1u] : 0;
  return a + (b - a) * (int)(position % POSITION_ONE) / (int)POSITION_ONE;
}

typedef struct render_cursor {
  h2_fruit_slice_audio_voice_t voices[H2_FRUIT_SLICE_AUDIO_VOICES];
  uint32_t music_position, ramp_left;
  int32_t sfx_gain, music_gain, sfx_step, music_step;
} render_cursor_t;

static render_cursor_t save_cursor(const h2_fruit_slice_audio_t* audio) {
  render_cursor_t cursor = {.music_position = audio->music_position,
                            .ramp_left = audio->ramp_left,
                            .sfx_gain = audio->sfx_gain,
                            .music_gain = audio->music_gain,
                            .sfx_step = audio->sfx_step,
                            .music_step = audio->music_step};
  memcpy(cursor.voices, audio->voices, sizeof(cursor.voices));
  return cursor;
}

static void restore_cursor(h2_fruit_slice_audio_t* audio,
                           const render_cursor_t* cursor) {
  memcpy(audio->voices, cursor->voices, sizeof(audio->voices));
  audio->music_position = cursor->music_position;
  audio->ramp_left = cursor->ramp_left;
  audio->sfx_gain = cursor->sfx_gain;
  audio->music_gain = cursor->music_gain;
  audio->sfx_step = cursor->sfx_step;
  audio->music_step = cursor->music_step;
}

static void mix_frame(h2_fruit_slice_audio_t* audio) {
  const uint32_t step = (H2_FRUIT_SLICE_AUDIO_BANK_RATE * POSITION_ONE) /
                        audio->format.sample_rate_hz;
  const h2_fruit_slice_audio_clip_t* music =
      &h2_fruit_slice_audio_bank[H2_FRUIT_SLICE_AUDIO_MUSIC];
  const uint32_t music_end = (uint32_t)music->count * POSITION_ONE;
  for (unsigned i = 0; i < audio->format.frame_samples_per_channel; ++i) {
    int32_t effects = 0;
    bool duck = false, release_duck = false;
    for (unsigned v = 0; v < H2_FRUIT_SLICE_AUDIO_VOICES; ++v) {
      h2_fruit_slice_audio_voice_t* voice = &audio->voices[v];
      if (!voice->active) continue;
      const h2_fruit_slice_audio_clip_t* clip =
          &h2_fruit_slice_audio_bank[voice->sound];
      effects += sample(clip, voice->position) * 150;
      voice->position += step;
      if (voice->position / POSITION_ONE >= clip->count) voice->active = false;
      duck = true;
    }
    if (audio->ramp_left != 0u) {
      audio->sfx_gain += audio->sfx_step;
      audio->music_gain += audio->music_step;
      if (--audio->ramp_left == 0u) {
        audio->sfx_gain =
            (audio->mute & H2_FRUIT_SLICE_MUTE_SFX) ? 0 : GAIN_ONE;
        audio->music_gain =
            (audio->mute & H2_FRUIT_SLICE_MUTE_MUSIC) ? 0 : GAIN_ONE;
        if (audio->mute & H2_FRUIT_SLICE_MUTE_SFX) {
          memset(audio->voices, 0, sizeof(audio->voices));
          /* The current sample still uses its one x10 duck multiplier.
           * Carry that level into x24 only after mixing this sample. */
          release_duck = duck && !(audio->mute & H2_FRUIT_SLICE_MUTE_MUSIC);
        }
      }
    }
    const int music_sample =
        sample(music, audio->music_position) * (duck ? 10 : 24);
    int32_t mixed = (int32_t)(((int64_t)effects * audio->sfx_gain +
                               (int64_t)music_sample * audio->music_gain) /
                              GAIN_ONE);
    audio->music_position += step;
    if (audio->music_position >= music_end) audio->music_position -= music_end;
    if (mixed > 32767) mixed = 32767;
    if (mixed < -32768) mixed = -32768;
    for (unsigned c = 0; c < audio->format.channels; ++c)
      audio->pcm[(size_t)i * audio->format.channels + c] = (int16_t)mixed;
    if (release_duck) {
      audio->music_gain = audio->music_gain * 10 / 24;
      audio->ramp_left = audio->format.sample_rate_hz / 100u;
      audio->music_step =
          (GAIN_ONE - audio->music_gain) / (int32_t)audio->ramp_left;
      audio->sfx_step = 0;
    }
  }
}

int h2_fruit_slice_audio_prepare(h2_fruit_slice_audio_t* audio) {
  if (audio == NULL) return H2_PAL_ERR_INVALID_ARG;
  if (!audio->available) return H2_PAL_ERR_UNSUPPORTED;
  if (audio->mute == 3u || audio->track != NULL || audio->worker != NULL)
    return H2_PAL_OK;
  int rc = h2_pal_audio_start_speaker(audio->runtime->audio);
  if (rc != H2_PAL_OK) return rc;
  audio->speaker_started = true;
  const h2_audio_track_config_t config = {.name = "fruit-slice",
                                          .format = audio->format,
                                          .volume_factor_milli = 1000u,
                                          .buffer_frames = 2u,
                                          .allocator = audio->runtime->mem};
  rc = h2_pal_audio_create_track(audio->runtime->audio, &config, &audio->track);
  if (rc != H2_PAL_OK) (void)h2_fruit_slice_audio_suspend(audio);
  return rc;
}

static void report_mute(h2_fruit_slice_audio_t* audio, const char* what,
                        int rc) {
  char message[96];
  (void)snprintf(message, sizeof(message), "H2_FRUIT_SLICE_AUDIO_%s rc=%d",
                 what, rc);
  (void)h2_pal_log_write(audio->runtime->log, H2_PAL_LOG_WARN, "fruit-slice",
                         message);
}

static int finish_mute(h2_fruit_slice_audio_t* audio, uint64_t now_ms) {
  if (audio->track != NULL && !audio->closing) {
    if (audio->mute_drain_after_ms == 0u)
      audio->mute_drain_after_ms = now_ms + audio->mute_timeout_ms;
    const int drained = audio->track->drain != NULL
                            ? h2_pal_audio_track_drain(audio->track, 0u)
                            : H2_PAL_ERR_UNSUPPORTED;
    if (drained != H2_PAL_OK && now_ms < audio->mute_drain_after_ms &&
        (drained == H2_PAL_ERR_WOULD_BLOCK || drained == H2_PAL_ERR_TIMEOUT ||
         drained == H2_PAL_ERR_UNSUPPORTED))
      return H2_PAL_OK;
    if (drained != H2_PAL_OK) report_mute(audio, "MUTE_DRAIN", drained);
  }
  const int closed = h2_fruit_slice_audio_suspend(audio);
  if (closed != H2_PAL_OK) report_mute(audio, "MUTE_CLOSE_RETRY", closed);
  return H2_PAL_OK; /* The retained closing state is retried by the owner. */
}

int h2_fruit_slice_audio_pump(h2_fruit_slice_audio_t* audio, uint64_t now_ms) {
  if (audio == NULL) return H2_PAL_ERR_INVALID_ARG;
  if (!audio->available) return H2_PAL_ERR_UNSUPPORTED;
  if (audio->worker != NULL) return h2_fruit_slice_audio_worker_poll(audio);
  if (audio->worker_requested && audio->mute != 3u) {
    const int started = h2_fruit_slice_audio_worker_start(audio);
    if (started != H2_PAL_OK) return started;
    return h2_fruit_slice_audio_worker_poll(audio);
  }
  if (audio->pref_dirty && now_ms >= audio->pref_retry_ms) {
    const int saved =
        h2_fruit_slice_audio_save_mute(audio->runtime, audio->mute);
    audio->pref_dirty = saved != H2_PAL_OK && saved != H2_PAL_ERR_UNSUPPORTED;
    audio->pref_retry_ms = now_ms + 500u;
  }
  if (audio->closing) {
    const int closed = h2_fruit_slice_audio_suspend(audio);
    if (closed != H2_PAL_OK) {
      report_mute(audio, "MUTE_CLOSE_RETRY", closed);
      return H2_PAL_OK;
    }
  }
  if (audio->mute == 3u) {
    if (audio->track == NULL || audio->ramp_left == 0u)
      return finish_mute(audio, now_ms);
    if (audio->mute_fade_after_ms == 0u)
      audio->mute_fade_after_ms = now_ms + audio->mute_timeout_ms;
    if (now_ms >= audio->mute_fade_after_ms) {
      /* A consumer that never accepts PCM cannot be faded digitally. Release
       * it with an explicit diagnostic instead of retaining hardware forever.
       */
      report_mute(audio, "MUTE_FADE_TIMEOUT", H2_PAL_ERR_TIMEOUT);
      const int closed = h2_fruit_slice_audio_suspend(audio);
      if (closed != H2_PAL_OK) report_mute(audio, "MUTE_CLOSE_RETRY", closed);
      return H2_PAL_OK;
    }
  }
  if (audio->pumped && now_ms >= audio->last_pump_ms) {
    const uint64_t gap = now_ms - audio->last_pump_ms;
    if (gap > audio->max_pump_gap_ms)
      audio->max_pump_gap_ms = gap > UINT32_MAX ? UINT32_MAX : (uint32_t)gap;
  }
  audio->last_pump_ms = now_ms;
  audio->pumped = true;
  const int prepared = h2_fruit_slice_audio_prepare(audio);
  if (prepared != H2_PAL_OK) return prepared;
  if (!audio->timeline_started) {
    audio->timeline_started = true;
    audio->started_ms = now_ms;
    audio->submitted_samples = 0;
  }
  uint64_t due = audio->started_ms + audio->submitted_samples * 1000u /
                                         audio->format.sample_rate_hz;
  if (now_ms > due && now_ms - due > 100u) {
    ++audio->rebases;
    audio->started_ms = now_ms;
    audio->submitted_samples = 0;
    due = now_ms;
  }
  /* Queue at most one native frame, capped at 20 ms, ahead. Waiting until a
   * frame is already due starves a 20 ms sink at a 25/30 ms game cadence.
   * Larger frames (e.g. Web's 60 ms) already provide headroom; do not add a
   * whole extra frame of interaction latency on those providers. */
  uint64_t ahead_ms =
      ((uint64_t)audio->format.frame_samples_per_channel * 1000u +
       audio->format.sample_rate_hz - 1u) /
      audio->format.sample_rate_hz;
  if (ahead_ms > 20u) ahead_ms = 20u;
  for (unsigned frame_index = 0; frame_index < 3u && due <= now_ms + ahead_ms;
       ++frame_index) {
    /* Mix speculatively into the one reusable PCM buffer. A rejection owns no
     * samples: do not advance BGM phase, voices or a mute/unmute ramp. A new
     * control can safely reverse that transition before the next attempt. */
    const render_cursor_t before = save_cursor(audio);
    mix_frame(audio);
    h2_audio_frame_t frame = h2_audio_frame_for_buffer(
        audio->pcm,
        (size_t)audio->format.frame_samples_per_channel *
            audio->format.channels * sizeof(int16_t),
        audio->format);
    frame.bytes = frame.capacity;
    const int rc = h2_pal_audio_track_write(audio->track, &frame, 0u);
    if (rc != H2_PAL_OK) {
      restore_cursor(audio, &before);
      if (rc != H2_PAL_ERR_WOULD_BLOCK && rc != H2_PAL_ERR_TIMEOUT) return rc;
      ++audio->blocked_writes;
      break;
    }
    ++audio->writes;
    audio->submitted_samples += audio->format.frame_samples_per_channel;
    due = audio->started_ms +
          audio->submitted_samples * 1000u / audio->format.sample_rate_hz;
    if (audio->mute == 3u && audio->ramp_left == 0u) break;
  }
  return H2_PAL_OK;
}

static int close_audio(h2_fruit_slice_audio_t* audio, int final_mute) {
  if (audio == NULL) return H2_PAL_ERR_INVALID_ARG;
  int rc = h2_fruit_slice_audio_suspend(audio);
  if (rc != H2_PAL_OK) return rc;
  /* The UI can advance past a rejected playback command. Reconcile only after
   * the worker and its immutable Pref writer have joined, so no older accepted
   * value can overwrite the final intent. This never restarts playback. */
  if (final_mute >= 0 && audio->mute != (uint8_t)final_mute) {
    audio->mute = (uint8_t)final_mute;
    audio->pref_dirty = true;
  }
  if (audio->pref_dirty && audio->runtime != NULL) {
    /* A disabled/cooperative session may exit before its retry deadline. Make
     * one final attempt, without trapping teardown on persistent storage I/O.
     */
    const int saved =
        h2_fruit_slice_audio_save_mute(audio->runtime, audio->mute);
    audio->pref_dirty = saved != H2_PAL_OK && saved != H2_PAL_ERR_UNSUPPORTED;
    if (audio->pref_dirty) report_mute(audio, "PREF_SAVE", saved);
  }
  if (audio->runtime != NULL) h2_pal_mem_free(audio->runtime->mem, audio->pcm);
  audio->pcm = NULL;
  audio->available = false;
  return H2_PAL_OK;
}

int h2_fruit_slice_audio_close(h2_fruit_slice_audio_t* audio) {
  return close_audio(audio, -1);
}

int h2_fruit_slice_audio_close_with_mute(h2_fruit_slice_audio_t* audio,
                                         uint8_t final_mute) {
  if (final_mute > 3u) return H2_PAL_ERR_INVALID_ARG;
  return close_audio(audio, final_mute);
}
