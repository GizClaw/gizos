#include "buffet_audio.h"

#include <math.h>
#include <string.h>

enum { BANK_RATE = 16000, ONE = 65536, MAX_FRAME = 4096 };
static const unsigned gain[] = {500, 550, 650, 620, 520, 430, 600, 660, 560};
static const unsigned priority[] = {0, 2, 2, 2, 5, 3, 3, 4, 4};
static const char* const cue_name[] = {"music", "admission", "plate",
                                       "bite",  "red",       "recovery",
                                       "flip",  "reveal",    "closing"};

bf_sound_state_t bf_sound_state(const bf_game_t* g) {
  return (bf_sound_state_t){g->phase,
                            g->eaten,
                            g->album.reveal_ms,
                            bf_reveal_duration(g),
                            g->fullness_milli > bf_rules.red_line * 1000,
                            g->muted};
}
void bf_mix_emit(bf_mix_t* m, unsigned cue, unsigned delay_ms) {
  if (!m->rate || cue == BF_MUSIC || cue >= BF_SOUND_COUNT) return;
  unsigned slot = 0;
  for (unsigned i = 0; i < 3; i++) {
    if (m->voices[i].cue == cue) {
      slot = i;
      break;
    }
    if (!m->voices[i].cue ||
        priority[m->voices[i].cue] < priority[m->voices[slot].cue])
      slot = i;
  }
  if (priority[m->voices[slot].cue] > priority[cue]) return;
  m->voices[slot] = (bf_voice_t){
      .cue = cue, .delay = (unsigned)((uint64_t)delay_ms * m->rate / 1000)};
}
static int sample(unsigned cue, uint64_t position) {
  const bf_clip_t* clip = &bf_tracks[cue];
  uint32_t index = (uint32_t)(position >> 16), next = index + 1;
  int first = clip->samples[index];
  int last = next < clip->count ? clip->samples[next]
                                : (cue == BF_MUSIC ? clip->samples[0] : 0);
  return first +
         (int)(((int64_t)(last - first) * (int32_t)(position & (ONE - 1))) /
               ONE);
}
void bf_mix_frame(bf_mix_t* m, int16_t* output, unsigned frames,
                  unsigned channels) {
  uint64_t step = ((uint64_t)BANK_RATE << 16) / m->rate,
           end = (uint64_t)bf_tracks[BF_MUSIC].count << 16;
  float attack = 1.f / (m->rate * .20f), release = 1.f / (m->rate * .36f);
  for (unsigned i = 0; i < frames; i++) {
    int mixed = 0;
    for (unsigned v = 0; v < 3; v++) {
      bf_voice_t* voice = &m->voices[v];
      if (!voice->cue) continue;
      if (voice->delay) {
        voice->delay--;
        continue;
      }
      mixed +=
          (int)(sample(voice->cue, voice->pos) * (int)gain[voice->cue] / 1000);
      voice->pos += step;
      if ((voice->pos >> 16) >= bf_tracks[voice->cue].count) voice->cue = 0;
    }
    m->envelope = m->music_on ? fminf(1, m->envelope + attack)
                              : fmaxf(0, m->envelope - release);
    if (m->envelope > 0) {
      mixed += (int)(sample(BF_MUSIC, m->song_pos) * .50f * m->envelope);
      m->song_pos = (m->song_pos + step) % end;
    }
    mixed = mixed * 85 / 100;
    if (mixed > 32767) mixed = 32767;
    if (mixed < -32768) mixed = -32768;
    for (unsigned ch = 0; ch < channels; ch++)
      output[(size_t)i * channels + ch] = (int16_t)mixed;
  }
}
static void mute_pref(h2_runtime_t* rt, bf_game_t* read_into, bool mute) {
  h2_pal_pref_namespace_t* ns = NULL;
  int rc = h2_pal_pref_open(
      rt->pref, "buffet_audio",
      read_into ? H2_PAL_PREF_OPEN_READ_ONLY : H2_PAL_PREF_OPEN_READ_WRITE,
      &ns);
  if (rc != H2_PAL_OK || !ns) return;
  if (read_into) {
    int flag = 0;
    if (ns->get_bool && ns->get_bool(ns, "muted_v1", &flag) == H2_PAL_OK)
      read_into->muted = flag != 0;
  } else if (ns->set_bool &&
             ns->set_bool(ns, "muted_v1", mute ? 1 : 0) == H2_PAL_OK &&
             ns->commit)
    (void)ns->commit(ns);
  if (ns->close) (void)ns->close(ns);
}
void bf_audio_init(bf_audio_t* a, h2_runtime_t* rt, bf_game_t* g) {
  a->rt = rt;
  mute_pref(rt, g, false);
  h2_audio_info_t info = {0};
  if (h2_pal_audio_get_info(rt->audio, &info) != H2_PAL_OK || !info.available ||
      !info.playback_supported)
    return;
  a->format = info.playback_format;
  h2_audio_pcm_format_t f = a->format;
  if (f.sample_format != H2_AUDIO_SAMPLE_S16LE || !f.channels ||
      f.channels > 2 || f.sample_rate_hz < 8000 || f.sample_rate_hz > 48000 ||
      !f.frame_samples_per_channel || f.frame_samples_per_channel > MAX_FRAME)
    return;
  a->samples = h2_pal_mem_alloc(
      rt->mem,
      (size_t)f.channels * f.frame_samples_per_channel * sizeof(int16_t));
  a->available = a->samples != NULL;
  a->mix.rate = f.sample_rate_hz;
}
static int stop(bf_audio_t* a) {
  int rc = H2_PAL_OK;
  if (a->track) {
    rc = h2_pal_audio_track_close(a->track);
    a->track = NULL;
  }
  if (a->speaker) {
    int closed = h2_pal_audio_stop_speaker(a->rt->audio);
    if (closed == H2_PAL_OK) a->speaker = false;
    if (rc == H2_PAL_OK) rc = closed;
  }
  memset(a->mix.voices, 0, sizeof(a->mix.voices));
  a->mix.envelope = 0;
  a->clock_started = false;
  a->tail_until = 0;
  return rc;
}
static int open_track(bf_audio_t* a) {
  if (a->track) return H2_PAL_OK;
  int rc = h2_pal_audio_start_speaker(a->rt->audio);
  if (rc != H2_PAL_OK) return rc;
  a->speaker = true;
  h2_audio_track_config_t cfg = {.name = "tidewheel",
                                 .format = a->format,
                                 .volume_factor_milli = 1000,
                                 .buffer_frames = 4,
                                 .allocator = a->rt->mem};
  rc = h2_pal_audio_create_track(a->rt->audio, &cfg, &a->track);
  if (rc != H2_PAL_OK) (void)stop(a);
  return rc;
}
static bool active(bf_audio_t* a) {
  return a->mix.music_on || a->mix.envelope > 0 || a->mix.voices[0].cue ||
         a->mix.voices[1].cue || a->mix.voices[2].cue;
}
static int pump(bf_audio_t* a, uint64_t now) {
  int rc = open_track(a);
  if (rc != H2_PAL_OK) return rc;
  if (!a->clock_started ||
      now > a->origin + a->sent * 1000 / a->mix.rate + 130) {
    a->origin = now;
    a->sent = 0;
    a->clock_started = true;
  }
  unsigned n = a->format.frame_samples_per_channel,
           channels = a->format.channels;
  for (int j = 0; j < 3 && a->origin + a->sent * 1000 / a->mix.rate <= now + 18;
       j++) {
    uint64_t old = a->mix.song_pos;
    bf_mix_frame(&a->mix, a->samples, n, channels);
    h2_audio_frame_t f = h2_audio_frame_for_buffer(
        a->samples, (size_t)n * channels * sizeof(int16_t), a->format);
    f.bytes = f.capacity;
    rc = h2_pal_audio_track_write(a->track, &f, 0);
    a->sent += n;
    if (rc == H2_PAL_ERR_TIMEOUT || rc == H2_PAL_ERR_WOULD_BLOCK)
      return H2_PAL_OK;
    if (rc != H2_PAL_OK) return rc;
    if (old > a->mix.song_pos)
      (void)h2_pal_log_write(a->rt->log, H2_PAL_LOG_INFO, "buffet-audio",
                             "loop");
  }
  return H2_PAL_OK;
}
static void emit(bf_audio_t* a, unsigned cue, unsigned delay) {
  bf_mix_emit(&a->mix, cue, delay);
  (void)h2_pal_log_write(a->rt->log, H2_PAL_LOG_INFO, "buffet-audio",
                         cue_name[cue]);
}
static void card_edges(bf_audio_t* a, bf_sound_state_t old,
                       bf_sound_state_t next, bool settled) {
  if (!next.queue || next.reveal >= next.queue) return;
  int start = settled ? -1 : old.reveal;
  if (next.reveal - start > 150)
    return; /* An explicit skip settles the rack quietly. */
  for (int i = 0; i < next.queue; i += BF_REVEAL_MS) {
    if (start < i && next.reveal >= i) emit(a, BF_FLIP, 0);
    if (start < i + 200 && next.reveal >= i + 200) emit(a, BF_REVEAL, 0);
  }
}
void bf_audio_update(bf_audio_t* a, bf_sound_state_t old, const bf_game_t* g,
                     uint64_t now, bool inactive) {
  bf_sound_state_t next = bf_sound_state(g);
  if (next.muted != old.muted) {
    mute_pref(a->rt, NULL, next.muted);
    (void)h2_pal_log_write(a->rt->log, H2_PAL_LOG_INFO, "buffet-audio",
                           next.muted ? "mute" : "unmute");
  }
  if (!a->available) return;
  if (inactive || next.muted) {
    if (a->track || a->speaker) (void)stop(a);
    if (next.phase == BF_DINING)
      a->mix.song_pos = (((uint64_t)g->elapsed_ms * BANK_RATE / 1000) %
                         bf_tracks[BF_MUSIC].count)
                        << 16;
    return;
  }
  a->mix.music_on = next.phase == BF_DINING;
  if (old.phase != BF_DINING && next.phase == BF_DINING) {
    a->mix.song_pos = 0;
    a->mix.envelope = 0;
    emit(a, BF_ADMIT, 0);
  }
  if (old.phase == BF_DINING && next.phase == BF_DINING) {
    if (next.eaten > old.eaten) {
      emit(a, BF_PLATE, 0);
      emit(a, BF_BITE, 185);
    }
    if (old.red != next.red) emit(a, next.red ? BF_RED : BF_RECOVER, 0);
  }
  bool settled = old.phase == BF_DINING && next.phase != BF_DINING;
  if (settled && next.phase == BF_CLOSING) emit(a, BF_CLOSE, 0);
  if (next.phase == BF_CLOSING || next.phase == BF_HOSPITAL)
    card_edges(a, old, next, settled);
  if (next.phase == BF_TICKET && old.phase != BF_TICKET) {
    (void)stop(a);
    return;
  }
  if (active(a)) a->tail_until = now + 170;
  if (a->tail_until <= now) {
    if (a->track || a->speaker) (void)stop(a);
    return;
  }
  if (now < a->retry_after) return;
  if (pump(a, now) != H2_PAL_OK) {
    (void)stop(a);
    a->retry_after = now + 1900;
    (void)h2_pal_log_write(a->rt->log, H2_PAL_LOG_WARN, "buffet-audio",
                           "output unavailable");
  }
}
h2_pal_result_t bf_audio_close(bf_audio_t* a) {
  if (!a->rt) return H2_PAL_OK;
  int rc = stop(a);
  if (a->speaker) (void)stop(a);
  h2_pal_mem_free(a->rt->mem, a->samples);
  a->samples = NULL;
  return rc;
}
