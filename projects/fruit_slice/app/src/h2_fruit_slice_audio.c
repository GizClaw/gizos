#include "h2_fruit_slice_audio.h"

#include "h2_fruit_slice.h"
#include "h2_fruit_slice_audio_bank.h"
#include "h2_fruit_slice_audio_worker.h"

#include <string.h>

#define PREF_KEY "audio_v1"
#define PREF_MAGIC 0x41550100u
#define POSITION_ONE 65536u

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
  memset(audio->voices, 0, sizeof(audio->voices));
  audio->seen = 0;
  audio->started_ms = audio->submitted_samples = 0;
  audio->pumped = audio->timeline_started = false;
  return first_error;
}

int h2_fruit_slice_audio_set_mute(h2_fruit_slice_audio_t* audio, uint8_t mute) {
  if (audio == NULL || mute > 3u) return H2_PAL_ERR_INVALID_ARG;
  if (audio->mute == mute) return H2_PAL_OK;
  int rc = h2_fruit_slice_audio_suspend(audio);
  if (rc != H2_PAL_OK) return rc;
  /* Keep the previous applied state until shutdown really completes. The
   * App retries a differing requested mute on its next frame. */
  audio->mute = mute;
  h2_pal_pref_namespace_t* ns = NULL;
  rc = h2_pal_pref_open(audio->runtime->pref, H2_FRUIT_SLICE_PREF_NAMESPACE,
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

static int sample(const h2_fruit_slice_audio_clip_t* clip, uint32_t position) {
  const uint32_t index = position / POSITION_ONE;
  if (index >= clip->count) return 0;
  const int a = clip->samples[index];
  const int b = index + 1u < clip->count ? clip->samples[index + 1u] : 0;
  return a + (b - a) * (int)(position % POSITION_ONE) / (int)POSITION_ONE;
}

static void mix_frame(h2_fruit_slice_audio_t* audio) {
  const uint32_t step = (H2_FRUIT_SLICE_AUDIO_BANK_RATE * POSITION_ONE) /
                        audio->format.sample_rate_hz;
  const h2_fruit_slice_audio_clip_t* music =
      &h2_fruit_slice_audio_bank[H2_FRUIT_SLICE_AUDIO_MUSIC];
  const uint32_t music_end = (uint32_t)music->count * POSITION_ONE;
  for (unsigned i = 0; i < audio->format.frame_samples_per_channel; ++i) {
    int32_t mixed = 0;
    bool duck = false;
    for (unsigned v = 0; v < H2_FRUIT_SLICE_AUDIO_VOICES; ++v) {
      h2_fruit_slice_audio_voice_t* voice = &audio->voices[v];
      if (!voice->active) continue;
      const h2_fruit_slice_audio_clip_t* clip =
          &h2_fruit_slice_audio_bank[voice->sound];
      mixed += sample(clip, voice->position) * 150;
      voice->position += step;
      if (voice->position / POSITION_ONE >= clip->count) voice->active = false;
      duck = true;
    }
    if (!(audio->mute & H2_FRUIT_SLICE_MUTE_MUSIC))
      mixed += sample(music, audio->music_position) * (duck ? 10 : 24);
    audio->music_position += step;
    if (audio->music_position >= music_end) audio->music_position -= music_end;
    if (mixed > 32767) mixed = 32767;
    if (mixed < -32768) mixed = -32768;
    for (unsigned c = 0; c < audio->format.channels; ++c)
      audio->pcm[(size_t)i * audio->format.channels + c] = (int16_t)mixed;
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

int h2_fruit_slice_audio_pump(h2_fruit_slice_audio_t* audio, uint64_t now_ms) {
  if (audio == NULL) return H2_PAL_ERR_INVALID_ARG;
  if (!audio->available) return H2_PAL_ERR_UNSUPPORTED;
  if (audio->mute == 3u) return H2_PAL_OK;
  if (audio->worker_requested && audio->worker == NULL) {
    const int started = h2_fruit_slice_audio_worker_start(audio);
    if (started != H2_PAL_OK) return started;
  }
  if (audio->worker != NULL) return h2_fruit_slice_audio_worker_poll(audio);
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
    mix_frame(audio);
    h2_audio_frame_t frame = h2_audio_frame_for_buffer(
        audio->pcm,
        (size_t)audio->format.frame_samples_per_channel *
            audio->format.channels * sizeof(int16_t),
        audio->format);
    frame.bytes = frame.capacity;
    const int rc = h2_pal_audio_track_write(audio->track, &frame, 0u);
    if (rc == H2_PAL_OK) ++audio->writes;
    if (rc == H2_PAL_ERR_WOULD_BLOCK || rc == H2_PAL_ERR_TIMEOUT)
      ++audio->blocked_writes;
    if (rc != H2_PAL_OK && rc != H2_PAL_ERR_WOULD_BLOCK &&
        rc != H2_PAL_ERR_TIMEOUT)
      return rc;
    /* Full queues discard this small frame; never replay stale operations. */
    audio->submitted_samples += audio->format.frame_samples_per_channel;
    due = audio->started_ms +
          audio->submitted_samples * 1000u / audio->format.sample_rate_hz;
    if (rc != H2_PAL_OK) break;
  }
  return H2_PAL_OK;
}

int h2_fruit_slice_audio_close(h2_fruit_slice_audio_t* audio) {
  if (audio == NULL) return H2_PAL_ERR_INVALID_ARG;
  int rc = h2_fruit_slice_audio_suspend(audio);
  if (rc != H2_PAL_OK) return rc;
  if (audio->runtime != NULL) h2_pal_mem_free(audio->runtime->mem, audio->pcm);
  audio->pcm = NULL;
  audio->available = false;
  return H2_PAL_OK;
}
