#include "h2_garden_guard_audio.h"

#include "h2_atomic.h"
#include "h2_garden_guard_score.h"

#include <stdio.h>

#define GG_AUDIO_MUSIC (1u << GG_SOUND_COUNT)
#define GG_AUDIO_RESET (GG_AUDIO_MUSIC << 1)
#define GG_AUDIO_QUIET (GG_AUDIO_MUSIC << 2)
#define GG_AUDIO_STOP (GG_AUDIO_MUSIC << 3)
#define GG_AUDIO_MASK (GG_AUDIO_MUSIC - 1u)
#define GG_AUDIO_MAX_FRAMES 2048u
#define GG_AUDIO_BUFFER_FRAMES 3u

struct gg_audio {
  h2_runtime_t* runtime;
  h2_pal_task_t* task;
  h2_atomic_u32_t pending;
  uint32_t mode; /* Run-thread publication state. */
  uint32_t merged;
};

static void report(gg_audio_t* audio, h2_pal_log_level_t level, const char* key,
                   int value) {
  char message[88];
  (void)snprintf(message, sizeof(message), "H2_GARDEN_GUARD_AUDIO_%s %d", key,
                 value);
  (void)h2_pal_log_write(audio->runtime->log, level, "garden-guard", message);
}

static int close_stream(h2_runtime_t* runtime, h2_pal_audio_track_t** track) {
  if (*track == NULL) return H2_PAL_OK;
  const int closed = h2_pal_audio_track_close(*track);
  *track = NULL;
  const int stopped = h2_pal_audio_stop_speaker(runtime->audio);
  return stopped != H2_PAL_OK ? stopped : closed;
}

static void audio_run(void* user) {
  gg_audio_t* audio = user;
  h2_runtime_t* runtime = audio->runtime;
  h2_audio_info_t info = {0};
  int rc = h2_pal_audio_get_info(runtime->audio, &info);
  const h2_audio_pcm_format_t format = info.playback_format;
  if (rc != H2_PAL_OK || !info.available || !info.playback_supported ||
      format.sample_format != H2_AUDIO_SAMPLE_S16LE ||
      format.sample_rate_hz < 8000u || format.sample_rate_hz > 48000u ||
      format.frame_samples_per_channel == 0u ||
      format.frame_samples_per_channel > GG_AUDIO_MAX_FRAMES ||
      format.channels == 0u || format.channels > 2u) {
    report(audio, H2_PAL_LOG_INFO, "UNAVAILABLE",
           rc == H2_PAL_OK ? H2_PAL_ERR_UNSUPPORTED : rc);
    return;
  }
  const unsigned frames = format.frame_samples_per_channel;
  const size_t bytes = frames * (size_t)format.channels * sizeof(int16_t);
  int16_t* pcm = h2_pal_mem_alloc(runtime->mem, bytes);
  if (pcm == NULL) {
    report(audio, H2_PAL_LOG_WARN, "UNAVAILABLE", H2_PAL_ERR_NO_MEMORY);
    return;
  }
  const unsigned period_us = frames * 1000000u / format.sample_rate_hz;
  const unsigned lead_us = period_us <= 25000u ? 2u * period_us : 20000u;
  const h2_audio_track_config_t config = {
      .name = "garden-guard",
      .format = format,
      .volume_factor_milli = 1000u,
      .buffer_frames = GG_AUDIO_BUFFER_FRAMES,
      .allocator = runtime->mem};
  h2_audio_frame_t frame = h2_audio_frame_for_buffer(pcm, bytes, format);
  frame.bytes = bytes;
  gg_score_t score;
  gg_score_init(&score, format.sample_rate_hz);
  h2_pal_audio_track_t* track = NULL;
  uint64_t next = 0, first = 0;
  uint32_t writes = 0, blocked = 0, late = 0, max_mix = 0, peak = 0;
  uint32_t totals_ms = 0;
  for (;;) {
    const uint32_t message =
        h2_atomic_fetch_and(&audio->pending, GG_AUDIO_MUSIC | GG_AUDIO_QUIET);
    if (message & GG_AUDIO_STOP) break;
    const bool music = (message & GG_AUDIO_MUSIC) != 0u;
    uint16_t events = (uint16_t)(message & GG_AUDIO_MASK);
    if (message & GG_AUDIO_RESET) {
      rc = close_stream(runtime, &track);
      if (rc != H2_PAL_OK) break;
      gg_score_init(&score, format.sample_rate_hz);
    }
    if (message & GG_AUDIO_QUIET) gg_score_clear_effects(&score);
    if (!music) events &= (1u << GG_SOUND_WON) | (1u << GG_SOUND_LOST);
    if (!music && track && (message & GG_AUDIO_RESET) == 0u &&
        (events || !gg_score_has_fanfare(&score))) {
      /* Pause and mute discard old transients and scheduled PCM. */
      gg_score_clear_effects(&score);
      rc = close_stream(runtime, &track);
      if (rc != H2_PAL_OK) break;
    }
    gg_score_events(&score, events);
    if (!music && !gg_score_has_effect(&score)) {
      if (track != NULL) {
        rc = close_stream(runtime, &track);
        if (rc != H2_PAL_OK) break;
      }
      (void)h2_pal_time_sleep_ms(runtime->time, 20u);
      continue;
    }
    uint64_t now;
    rc = h2_pal_time_get_monotonic_us(runtime->time, &now);
    if (rc != H2_PAL_OK) break;
    if (track == NULL) {
      rc = h2_pal_audio_start_speaker(runtime->audio);
      if (rc != H2_PAL_OK) break;
      rc = h2_pal_audio_create_track(runtime->audio, &config, &track);
      if (rc != H2_PAL_OK) {
        const int stopped = h2_pal_audio_stop_speaker(runtime->audio);
        if (stopped != H2_PAL_OK)
          report(audio, H2_PAL_LOG_WARN, "STOP", stopped);
        break;
      }
      rc = h2_pal_time_get_monotonic_us(runtime->time, &now);
      if (rc != H2_PAL_OK) break;
      next = now;
      if (!first) first = now;
    }
    if (now > next && now - next > period_us) {
      ++late;
      next = now;
    }
    for (unsigned n = 0; n < 2u && next <= now + lead_us; ++n) {
      const gg_score_t before = score;
      const unsigned level =
          gg_score_render(&score, music, pcm, frames, format.channels);
      uint64_t mixed = now;
      (void)h2_pal_time_get_monotonic_us(runtime->time, &mixed);
      rc = h2_pal_audio_track_write(track, &frame, 0u);
      if (rc != H2_PAL_OK) {
        score = before;
        if (rc == H2_PAL_ERR_WOULD_BLOCK || rc == H2_PAL_ERR_TIMEOUT) {
          ++blocked;
          rc = H2_PAL_OK;
        }
        break;
      }
      ++writes;
      if (level > peak) peak = level;
      if (mixed >= now && mixed - now > max_mix)
        max_mix = (uint32_t)(mixed - now);
      (void)h2_pal_time_get_monotonic_us(runtime->time, &now);
      next += period_us;
      if (!music && !gg_score_has_effect(&score)) {
        /* Keep the tail in the provider for a bounded drain. */
        if (track->drain) (void)h2_pal_audio_track_drain(track, 160u);
        break;
      }
    }
    if (rc != H2_PAL_OK) break;
    const uint32_t elapsed = (uint32_t)((now - first) / 1000u);
    if (elapsed - totals_ms >= 10000u) {
      totals_ms = elapsed;
      char stats[148];
      (void)snprintf(
          stats, sizeof(stats),
          "H2_GARDEN_GUARD_AUDIO_STATS write=%u busy=%u late=%u max_mix_us=%u peak=%u rate=%u frame=%u",
          (unsigned)writes, (unsigned)blocked, (unsigned)late,
          (unsigned)max_mix, (unsigned)peak, (unsigned)format.sample_rate_hz,
          frames);
      (void)h2_pal_log_write(runtime->log, H2_PAL_LOG_INFO, "garden-guard",
                             stats);
    }
    (void)h2_pal_time_sleep_ms(runtime->time, 5u);
  }
  const int closed = close_stream(runtime, &track);
  h2_pal_mem_free(runtime->mem, pcm);
  if (rc != H2_PAL_OK || closed != H2_PAL_OK)
    report(audio, H2_PAL_LOG_WARN, "OFFLINE", rc != H2_PAL_OK ? rc : closed);
}

gg_audio_t* gg_audio_start(h2_runtime_t* runtime) {
  gg_audio_t* audio = h2_pal_mem_alloc(runtime->mem, sizeof(*audio));
  if (audio == NULL) return NULL;
  *audio = (gg_audio_t){.runtime = runtime};
  if (h2_atomic_init(&audio->pending, 0u) != H2_ATOMIC_OK) {
    h2_pal_mem_free(runtime->mem, audio);
    return NULL;
  }
  const h2_pal_task_options_t task = {.name = H2_GARDEN_GUARD_AUDIO_TASK,
                                      .min_stack_size = 8192u};
  const int rc =
      h2_pal_task_start(runtime->task, &task, audio_run, audio, &audio->task);
  if (rc == H2_PAL_OK) return audio;
  report(audio, H2_PAL_LOG_WARN, "TASK", rc);
  h2_atomic_destroy(&audio->pending);
  h2_pal_mem_free(runtime->mem, audio);
  return NULL;
}

void gg_audio_publish(gg_audio_t* audio, uint16_t sounds, bool music,
                      bool muted, bool reset) {
  if (!audio) return;
  if (muted) sounds = 0;
  const uint32_t mode = muted ? GG_AUDIO_QUIET : music ? GG_AUDIO_MUSIC : 0u;
  if (reset || audio->mode != mode) {
    uint32_t word = sounds | mode;
    /* Carry a pending reset through a faster control change. A reset which
     * raced with worker consumption can safely be applied again. */
    if (reset || (h2_atomic_load(&audio->pending) & GG_AUDIO_RESET))
      word |= GG_AUDIO_RESET;
    h2_atomic_store(&audio->pending, word);
    audio->mode = mode;
  } else if (sounds) {
    const uint32_t prior = h2_atomic_fetch_or(&audio->pending, sounds);
    if (prior & sounds) ++audio->merged;
  }
}

int gg_audio_join(gg_audio_t* audio) {
  if (!audio) return H2_PAL_OK;
  h2_atomic_store(&audio->pending, GG_AUDIO_STOP);
  const int rc = h2_pal_task_join(audio->runtime->task, audio->task);
  if (rc != H2_PAL_OK) return rc;
  if (audio->merged)
    report(audio, H2_PAL_LOG_INFO, "MERGED", (int)audio->merged);
  h2_atomic_destroy(&audio->pending);
  h2_pal_mem_free(audio->runtime->mem, audio);
  return H2_PAL_OK;
}
