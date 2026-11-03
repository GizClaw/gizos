#include "h2_mfg_internal.h"

#include "h2_atomic.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

/* Same single-board 500/1000/2500-Hz probe used in the 1.75C bring-up.
 * This measures far echo/headroom, followed by operator speech and a real
 * processed double-talk recording. It is not the independent-source,
 * three-band/two-level calibration and must not grant that qualification. */
static const int16_t probe[32] = {
    0,     2631,  4280,  4478,  3496,  2147,  1292,  1344,  2048,  2673, 2492,
    1259,  -600,  -2202, -2712, -1832, 0,     1832,  2712,  2202,  600,  -1259,
    -2492, -2673, -2048, -1344, -1292, -2147, -3496, -4478, -4280, -2631};

enum {
  RATE = 16000,
  MAX_SAMPLES = 512,
  WINDOW_SAMPLES = 96 * 512,
  RECORDING_QUEUE_FRAMES = 4,
  PEAK_LIMIT = 30000,
  CAPTURE_COMPLETE = 1,
  PLAYBACK_COMPLETE = 2
};
typedef enum phase {
  NOISE,
  FAR,
  NOISE_AFTER,
  NEAR,
  DOUBLE_TALK,
  REPLAY_ECHO,
  REPLAY
} phase_t;

typedef struct capture_meter {
  uint64_t sequence, end_ms, mic_energy, reference_energy, output_energy;
  uint64_t aec_reference_energy;
  uint32_t frames, skip, partial, peak, clipped;
  int result;
} capture_meter_t;

typedef struct playback_meter {
  uint64_t end_ms;
  uint32_t frames, active, skip, peak, clipped;
  int result;
} playback_meter_t;

struct h2_mfg_aec {
  h2_mfg_t* owner;
  phase_t phase;
  uint64_t start_ms, repeat_ms;
  unsigned samples, measured, warmup, echo_db, position;
  uint32_t mic_gain, volume;
  int diagnostics, registered, replayed;
  /* Only these atomic publications are read while workers are live. */
  h2_atomic_uint_t completed;
  h2_atomic_int_t worker_result;
  unsigned atomics_initialized;
  /* Each worker writes only its own meter. The controller reads these only
   * after BOTH workers have stopped and joined; UI uses controller fields. */
  capture_meter_t capture;
  playback_meter_t playback;
  uint64_t noise_raw[2], noise_output[2];
  uint64_t far_raw, far_output, far_reference, far_aec_reference;
  uint64_t near_output;
  /* RAM stores retain far-only/voice PCM, or just voice with diagnostics.
   * External stores need only one assembly/drain frame and a bounded queue. */
  int16_t* recording;
  size_t recording_bytes;
  const h2_mfg_recording_store_t* store;
  h2_pal_queue_t* recording_queue;
  unsigned stored_frames;
  int store_opened, store_result, replay_frame_loaded;
};

static int replay(phase_t phase) {
  return phase == REPLAY || phase == REPLAY_ECHO;
}

static const char* phase_name(phase_t phase) {
  static const char* names[] = {
      "QUIET",           "ECHO TEST",       "QUIET AGAIN",    "SPEAK NOW",
      "SPEAK OVER TONE", "LISTEN FOR ECHO", "LISTEN TO VOICE"};
  return names[phase];
}

const char* h2_mfg_aec_phase(const h2_mfg_t* m) {
  return m->aec == NULL ? "STARTING" : phase_name(m->aec->phase);
}

int h2_mfg_aec_echo_db(const h2_mfg_t* m) {
  return m->aec == NULL || m->aec->diagnostics != 1 ? -1 : (int)m->aec->echo_db;
}

static int valid(const h2_mfg_aec_t* a, const h2_audio_frame_t* f,
                 unsigned channels) {
  return f != NULL && f->data != NULL && f->sample_rate_hz == RATE &&
         f->sample_format == H2_AUDIO_SAMPLE_S16LE &&
         f->samples_per_channel == a->samples && f->channels == channels &&
         f->bytes == a->samples * channels * sizeof(int16_t) &&
         f->capacity >= f->bytes;
}

static size_t recording_samples(const h2_mfg_aec_t* a) {
  return (size_t)a->measured * a->samples;
}

static int16_t* voice_recording(h2_mfg_aec_t* a) {
  return a->recording + (a->diagnostics ? 0 : recording_samples(a));
}

static int recorded_phase(const h2_mfg_aec_t* a) {
  return a->phase == DOUBLE_TALK || (a->phase == FAR && !a->diagnostics);
}

static unsigned recording_index(const h2_mfg_aec_t* a) {
  return a->phase == FAR || a->phase == REPLAY_ECHO || a->diagnostics ? 0 : 1;
}

/* Owner-only error state; capture keeps sole ownership of its live meter. */
static int store_failure(h2_mfg_aec_t* a, int result) {
  if (a->store_result == 0) a->store_result = result;
  a->owner->model.ready = 0;
  return a->store_result;
}

static int open_recording(h2_mfg_aec_t* a) {
  if (a->recording != NULL) return a->store_result;
  h2_mfg_t* m = a->owner;
  size_t bytes = a->store != NULL
                     ? a->samples * sizeof(*a->recording)
                     : (a->diagnostics ? 1u : 2u) * recording_samples(a) *
                           sizeof(*a->recording);
  a->recording = h2_pal_mem_alloc(m->rt->mem, bytes);
  if (a->recording == NULL) return H2_PAL_ERR_NO_MEMORY;
  a->recording_bytes = bytes;
  memset(a->recording, 0, bytes);
  if (a->store == NULL) return 0;
  if (a->diagnostics) {
    const h2_pal_queue_config_t config = {.name = "mfg/aec/recording",
                                          .item_size = bytes,
                                          .item_count = RECORDING_QUEUE_FRAMES,
                                          .allocator = m->rt->mem};
    int rc = h2_pal_queue_create(m->rt->queue, &config, &a->recording_queue);
    if (rc != 0) return store_failure(a, rc);
  }
  /* Even a partially failed open needs a matching, retryable close. */
  a->store_opened = 1;
  int rc = a->store->open(a->store->user, a->diagnostics ? 1 : 2, a->measured,
                        a->samples);
  return rc == 0 ? 0 : store_failure(a, rc);
}

static int write_recording(h2_mfg_aec_t* a) {
  if (a->store_result != 0) return a->store_result;
  if (a->stored_frames >= a->measured)
    return store_failure(a, H2_PAL_ERR_INVALID_STATE);
  int rc = a->store->write_frame(a->store->user, recording_index(a),
                               a->stored_frames, a->recording, a->samples);
  if (rc != 0) return store_failure(a, rc);
  ++a->stored_frames;
  return 0;
}

/* Bounded while workers run, and enough to empty the queue after they join.
 * recv copies each callback loan before the owner calls blocking storage. */
static int drain_recording(h2_mfg_aec_t* a) {
  if (a->store_result != 0) return a->store_result;
  if (a->recording_queue == NULL || !recorded_phase(a)) return 0;
  for (unsigned budget = 0; budget < RECORDING_QUEUE_FRAMES; ++budget) {
    int rc = h2_pal_queue_recv(a->owner->rt->queue, a->recording_queue,
                              a->recording, H2_PAL_QUEUE_NO_WAIT);
    if (rc == H2_PAL_ERR_WOULD_BLOCK || rc == H2_PAL_ERR_TIMEOUT) return 0;
    if (rc != 0) return store_failure(a, rc);
    rc = write_recording(a);
    if (rc != 0) return rc;
  }
  return 0;
}

static void sample(int16_t value, uint64_t* energy, uint32_t* peak,
                   uint32_t* clipped) {
  int32_t v = value;
  uint32_t magnitude = (uint32_t)(v < 0 ? -v : v);
  if (energy != NULL) *energy += (uint64_t)magnitude * magnitude;
  if (magnitude > *peak) *peak = magnitude;
  *clipped += magnitude >= PEAK_LIMIT;
}

static void publish_worker_error(h2_mfg_aec_t* a, int result) {
  if (result == 0) return;
  int expected = 0;
  (void)h2_atomic_int_compare_exchange(&a->worker_result, &expected, result,
                                       H2_ATOMIC_RELEASE, H2_ATOMIC_RELAXED);
}

static void on_capture(void* user, const h2_audio_aec_frame_t* f) {
  h2_mfg_aec_t* a = user;
  capture_meter_t* c = &a->capture;
  if (f == NULL || f->raw == NULL || f->raw->channels < 2 ||
      f->raw->channels > 4 || !valid(a, f->raw, f->raw->channels) ||
      !valid(a, f->reference_input, 1) || !valid(a, f->processed, 1) ||
      f->microphone_lane >= f->raw->channels ||
      f->reference_lane >= f->raw->channels ||
      (f->microphone_mask & (1u << f->microphone_lane)) == 0 ||
      (f->microphone_mask & (1u << f->reference_lane)) != 0 ||
      f->microphone_mask >= (1u << f->raw->channels) || f->sequence == 0 ||
      (c->sequence != 0 && f->sequence != c->sequence + 1)) {
    c->result = H2_PAL_ERR_FORMAT;
    publish_worker_error(a, c->result);
    return;
  }
  c->sequence = f->sequence;
  if (c->skip != 0) {
    --c->skip;
    return;
  }
  if (c->frames == a->measured) return;
  const int16_t* raw = f->raw->data;
  const int16_t* reference = f->reference_input->data;
  const int16_t* output = f->processed->data;
  for (unsigned i = 0; i < a->samples; ++i) {
    sample(raw[i * f->raw->channels + f->microphone_lane], &c->mic_energy,
           &c->peak, &c->clipped);
    for (unsigned lane = 0; lane < f->raw->channels; ++lane)
      if (lane != f->microphone_lane &&
          (f->microphone_mask & (1u << lane)) != 0)
        sample(raw[i * f->raw->channels + lane], NULL, &c->peak, &c->clipped);
    sample(raw[i * f->raw->channels + f->reference_lane], &c->reference_energy,
           &c->peak, &c->clipped);
    sample(reference[i], &c->aec_reference_energy, &c->peak, &c->clipped);
    sample(output[i], &c->output_energy, &c->peak, &c->clipped);
  }
  if (a->phase == DOUBLE_TALK) {
    if (a->store == NULL) {
      memcpy(voice_recording(a) + (size_t)c->frames * a->samples, output,
             a->samples * sizeof(int16_t));
    } else if (c->result == 0) {
      /* The queue copies the complete borrowed frame before returning. Never
       * block, allocate, or call the store from this capture worker. */
      int rc = h2_pal_queue_send(a->owner->rt->queue, a->recording_queue, output,
                                H2_PAL_QUEUE_NO_WAIT);
      if (rc != 0) {
        c->result = rc == H2_PAL_ERR_WOULD_BLOCK || rc == H2_PAL_ERR_TIMEOUT
                        ? H2_PAL_ERR_IO
                        : rc;
        publish_worker_error(a, c->result);
      }
    }
  }
  ++c->frames;
  if (c->frames == a->measured) {
    int rc = h2_pal_time_get_monotonic_ms(a->owner->rt->time, &c->end_ms);
    if (c->result == 0) c->result = rc;
    publish_worker_error(a, c->result);
    (void)h2_atomic_uint_fetch_or(&a->completed, CAPTURE_COMPLETE,
                                  H2_ATOMIC_RELEASE);
  }
}

static void on_playback(void* user, const h2_audio_frame_t* f) {
  h2_mfg_aec_t* a = user;
  playback_meter_t* p = &a->playback;
  if (!valid(a, f, 1)) {
    p->result = H2_PAL_ERR_FORMAT;
    publish_worker_error(a, p->result);
    return;
  }
  if (p->skip != 0) {
    --p->skip;
    return;
  }
  if (p->frames == a->measured) return;
  int active = 0;
  const int16_t* pcm = f->data;
  for (unsigned i = 0; i < a->samples; ++i) {
    sample(pcm[i], NULL, &p->peak, &p->clipped);
    active |= pcm[i] != 0;
  }
  p->active += active;
  ++p->frames;
  if (p->frames == a->measured) {
    int rc = h2_pal_time_get_monotonic_ms(a->owner->rt->time, &p->end_ms);
    if (p->result == 0) p->result = rc;
    publish_worker_error(a, p->result);
    (void)h2_atomic_uint_fetch_or(&a->completed, PLAYBACK_COMPLETE,
                                  H2_ATOMIC_RELEASE);
  }
}

int h2_mfg_aec_mic_frame(h2_mfg_t* m, const h2_audio_frame_t* f) {
  h2_mfg_aec_t* a = m->aec;
  if (a == NULL || a->diagnostics != 0) return 0;
  if (f == NULL || f->sample_rate_hz != RATE || f->channels != 1 ||
      f->sample_format != H2_AUDIO_SAMPLE_S16LE || f->bytes == 0 ||
      f->bytes > f->capacity ||
      f->bytes != (size_t)f->samples_per_channel * sizeof(int16_t))
    return H2_PAL_ERR_FORMAT;
  capture_meter_t* c = &a->capture;
  const int16_t* pcm = f->data;
  /* SDK reads may split a frame. Assemble complete windows without assuming
   * that an individual queue read equals a hardware processing frame. */
  for (unsigned i = 0; i < f->samples_per_channel; ++i) {
    if (c->skip != 0) {
      if (++c->partial == a->samples) {
        c->partial = 0;
        --c->skip;
      }
      continue;
    }
    if (c->frames == a->measured) break;
    sample(pcm[i], &c->output_energy, &c->peak, &c->clipped);
    if (a->store != NULL) {
      if (recorded_phase(a)) a->recording[c->partial] = pcm[i];
    } else {
      size_t index = (size_t)c->frames * a->samples + c->partial;
      if (a->phase == FAR) a->recording[index] = pcm[i];
      if (a->phase == DOUBLE_TALK) voice_recording(a)[index] = pcm[i];
    }
    if (++c->partial == a->samples) {
      c->partial = 0;
      if (a->store != NULL && recorded_phase(a)) {
        int rc = write_recording(a);
        if (rc != 0) return rc;
      }
      if (++c->frames == a->measured) {
        int rc = h2_pal_time_get_monotonic_ms(m->rt->time, &c->end_ms);
        if (rc != 0) return rc;
        (void)h2_atomic_uint_fetch_or(&a->completed, CAPTURE_COMPLETE,
                                      H2_ATOMIC_RELEASE);
      }
    }
  }
  return 0;
}

static void on_error(void* user, int capture, int result) {
  h2_mfg_aec_t* a = user;
  int* out = capture ? &a->capture.result : &a->playback.result;
  *out = result == 0 ? H2_PAL_ERR_IO : result;
  publish_worker_error(a, *out);
}

static int stop_phase(h2_mfg_t* m) {
  int rc = h2_mfg_audio_stop(m);
  if (rc != 0) return rc;
  if (m->aec->registered) {
    rc = h2_pal_audio_set_aec_observer(m->rt->audio, NULL);
    if (rc != 0) return rc;
    m->aec->registered = 0;
  }
  return 0;
}

int h2_mfg_aec_close(h2_mfg_t* m) {
  if (m->aec == NULL) return 0;
  int rc = stop_phase(m);
  if (rc != 0) return rc;
  if (m->aec->store_opened) {
    rc = m->aec->store->close(m->aec->store->user);
    if (rc != 0) return rc;
    m->aec->store_opened = 0;
  }
  /* Workers are joined and storage cleanup succeeded before releasing any
   * dependencies. Both RAM recordings and the small assembly frame are erased. */
  h2_pal_queue_destroy(m->rt->queue, m->aec->recording_queue);
  if (m->aec->recording != NULL)
    memset(m->aec->recording, 0, m->aec->recording_bytes);
  h2_pal_mem_free(m->rt->mem, m->aec->recording);
  if (m->aec->atomics_initialized >= 2)
    h2_atomic_int_destroy(&m->aec->worker_result);
  if (m->aec->atomics_initialized >= 1)
    h2_atomic_uint_destroy(&m->aec->completed);
  memset(m->aec, 0, sizeof(*m->aec));
  h2_pal_mem_free(m->rt->mem, m->aec);
  m->aec = NULL;
  return 0;
}

static int start_phase(h2_mfg_t* m, phase_t phase) {
  h2_mfg_aec_t* a = m->aec;
  int rc = stop_phase(m);
  if (rc == 0) rc = drain_recording(a);
  if (rc != 0) return rc;
  a->phase = phase;
  unsigned warmup_samples =
      phase == FAR || phase == DOUBLE_TALK ? WINDOW_SAMPLES : 32 * 512;
  a->warmup = (warmup_samples + a->samples - 1) / a->samples;
  a->capture = (capture_meter_t){.skip = a->warmup};
  a->playback = (playback_meter_t){.skip = a->warmup};
  h2_atomic_uint_store(&a->completed, 0, H2_ATOMIC_RELAXED);
  h2_atomic_int_store(&a->worker_result, 0, H2_ATOMIC_RELAXED);
  m->audio_frames = m->mic_level = m->mic_peak = 0;
  m->audio_samples = m->mic_energy = 0;
  m->model.ready = 0;
  a->position = 0;
  a->replay_frame_loaded = 0;
  a->stored_frames = 0;
  a->repeat_ms = 0;
  unsigned volume =
      replay(phase) || phase == FAR || phase == DOUBLE_TALK ? 100 : 0;
  rc = h2_pal_audio_set_speaker_volume_percent(m->rt->audio, volume);
  if (rc == 0)
    rc = h2_pal_audio_get_speaker_volume_percent(m->rt->audio, &a->volume);
  if (rc == 0 && a->volume != volume) rc = H2_PAL_ERR_IO;
  const h2_audio_aec_observer_t observer = {.user = a,
                                            .on_capture = on_capture,
                                            .on_playback = on_playback,
                                            .on_error = on_error};
  if (rc == 0 && !replay(phase) && a->diagnostics != 0) {
    rc = h2_pal_audio_set_aec_observer(m->rt->audio, &observer);
    if (rc == 0) {
      a->registered = 1;
      a->diagnostics = 1;
    } else if (rc == H2_PAL_ERR_UNSUPPORTED && a->diagnostics == -1) {
      /* A real AEC path is required by enter(). An opaque SDK can provide
       * processed audio for operator acceptance without claiming raw/DAC
       * observations or an automatic echo-reduction measurement. */
      a->diagnostics = 0;
      rc = 0;
    }
  }
  if (rc == 0) rc = open_recording(a);
  if (rc == 0 && a->store != NULL && recorded_phase(a)) {
    if (a->recording_queue != NULL)
      rc = h2_pal_queue_reset(m->rt->queue, a->recording_queue);
    if (rc == 0)
      rc = a->store->begin(a->store->user, recording_index(a));
    if (rc != 0) return store_failure(a, rc);
  }
  if (rc == 0) rc = h2_pal_time_get_monotonic_ms(m->rt->time, &a->start_ms);
  if (rc == 0 && !replay(phase)) {
    rc = h2_pal_audio_start_mic(m->rt->audio);
    if (rc == 0) m->mic_started = 1;
  }
  if (rc == 0) {
    rc = h2_pal_audio_start_speaker(m->rt->audio);
    if (rc == 0) m->speaker_started = 1;
  }
  if (rc == 0) {
    const h2_audio_track_config_t track = {
        .name = "mfg/aec",
        .format = m->audio_info.playback_format,
        .volume_factor_milli = 1000,
        .buffer_frames = h2_mfg_playback_buffer_frames(m),
        .allocator = m->rt->mem};
    rc = h2_pal_audio_create_track(m->rt->audio, &track, &m->track);
  }
  (void)snprintf(m->detail, sizeof(m->detail),
                 "AEC phase=%s mode=%s echo_db=%d spk=%" PRIu32 " mic=%" PRIu32
                 " manual_voice=pending",
                 phase_name(phase),
                 a->diagnostics < 0 ? "starting"
                 : a->diagnostics  ? "diagnostics"
                                   : "operator",
                 h2_mfg_aec_echo_db(m), a->volume, a->mic_gain);
  return rc;
}

static int atomic_error(h2_atomic_result_t result) {
  return result == H2_ATOMIC_NO_MEMORY ? H2_PAL_ERR_NO_MEMORY
         : result == H2_ATOMIC_UNSUPPORTED ? H2_PAL_ERR_UNSUPPORTED
                                          : H2_PAL_ERR_INVALID_STATE;
}

int h2_mfg_aec_enter(h2_mfg_t* m) {
  int rc = h2_pal_audio_get_info(m->rt->audio, &m->audio_info);
  if (rc != 0) return rc;
  h2_audio_pcm_format_t mic = m->audio_info.mic_format;
  h2_audio_pcm_format_t spk = m->audio_info.playback_format;
  if (!m->audio_info.available || !m->audio_info.mic_supported ||
      !m->audio_info.playback_supported || !m->audio_info.aec_enabled ||
      mic.sample_rate_hz != RATE || spk.sample_rate_hz != RATE ||
      mic.channels != 1 || spk.channels != 1 ||
      mic.frame_samples_per_channel == 0 ||
      mic.frame_samples_per_channel > MAX_SAMPLES ||
      mic.frame_samples_per_channel % 32 != 0 ||
      spk.frame_samples_per_channel != mic.frame_samples_per_channel ||
      mic.sample_format != H2_AUDIO_SAMPLE_S16LE ||
      spk.sample_format != H2_AUDIO_SAMPLE_S16LE)
    return H2_PAL_ERR_UNSUPPORTED;
  const h2_mfg_recording_store_t* store = m->profile->aec_recording_store;
  if (store != NULL &&
      (store->open == NULL || store->begin == NULL || store->write_frame == NULL ||
       store->read_frame == NULL || store->close == NULL))
    return H2_PAL_ERR_INVALID_ARG;
  unsigned samples = mic.frame_samples_per_channel;
  unsigned measured = (WINDOW_SAMPLES + samples - 1) / samples;
  m->aec = h2_pal_mem_alloc(m->rt->mem, sizeof(*m->aec));
  if (m->aec == NULL) return H2_PAL_ERR_NO_MEMORY;
  memset(m->aec, 0, sizeof(*m->aec));
  m->aec->owner = m;
  m->aec->samples = samples;
  m->aec->measured = measured;
  m->aec->diagnostics = -1;
  m->aec->store = store;
  h2_atomic_result_t atomic = h2_atomic_uint_init(&m->aec->completed, 0);
  if (atomic != H2_ATOMIC_OK) return atomic_error(atomic);
  m->aec->atomics_initialized = 1;
  atomic = h2_atomic_int_init(&m->aec->worker_result, 0);
  if (atomic != H2_ATOMIC_OK) return atomic_error(atomic);
  m->aec->atomics_initialized = 2;
  m->pcm_bytes = samples * sizeof(int16_t);
  m->pcm = h2_pal_mem_alloc(m->rt->mem, m->pcm_bytes);
  if (m->pcm == NULL) return H2_PAL_ERR_NO_MEMORY;
  rc = h2_pal_audio_get_speaker_volume_percent(m->rt->audio, &m->saved_volume);
  if (rc == 0) m->volume_saved = 1;
  if (rc == 0)
    rc = h2_pal_audio_get_mic_gain_percent(m->rt->audio, &m->aec->mic_gain);
  return rc == 0 ? start_phase(m, NOISE) : rc;
}

static unsigned reduction_db(uint64_t signal, uint64_t residual) {
  static const unsigned tenths[] = {1000, 1259, 1585, 1995, 2512,
                                    3162, 3981, 5012, 6310, 7943};
  uint64_t ratio = signal * 1000 / (residual == 0 ? 1 : residual);
  unsigned db = 0;
  while (ratio >= 10000) {
    db += 10;
    ratio /= 10;
  }
  for (unsigned i = 1; i < 10 && ratio >= tenths[i]; ++i) ++db;
  return db;
}

static int evaluate_echo(h2_mfg_t* m) {
  h2_mfg_aec_t* a = m->aec;
  uint64_t noise =
      a->noise_raw[0] > a->noise_raw[1] ? a->noise_raw[0] : a->noise_raw[1];
  uint64_t quiet =
      a->noise_raw[0] < a->noise_raw[1] ? a->noise_raw[0] : a->noise_raw[1];
  uint64_t output_noise = a->noise_output[0] > a->noise_output[1]
                              ? a->noise_output[0]
                              : a->noise_output[1];
  uint64_t signal = a->far_raw > noise ? a->far_raw - noise : 0;
  uint64_t residual =
      a->far_output > output_noise ? a->far_output - output_noise : 0;
  a->echo_db = reduction_db(signal, residual);
  const char* reason = NULL;
  if (noise > (quiet == 0 ? 1 : quiet) * 4)
    reason = "background unstable";
  else if (signal < (noise == 0 ? 1 : noise) * 10)
    reason = "echo input too weak";
  else if (a->far_reference < 1000 || a->far_aec_reference < 1000)
    reason = "reference inactive";
  else if (residual * 10 > signal)
    reason = "echo reduction below 10dB";
  (void)snprintf(m->detail, sizeof(m->detail),
                 "AEC echo_db=%u raw=%" PRIu64 " residual=%" PRIu64
                 " noise=%" PRIu64 " %s",
                 a->echo_db, signal, residual, noise,
                 reason == NULL ? "far_probe=OK voice=pending" : reason);
  (void)h2_pal_log_write(m->rt->log, H2_PAL_LOG_INFO, "mfg/aec", m->detail);
  return reason == NULL ? 0 : H2_PAL_ERR_IO;
}

static int finish_window(h2_mfg_t* m) {
  h2_mfg_aec_t* a = m->aec;
  /* On opaque SDKs drain proves downstream consumption, not physical DAC
   * acceptance. Do not manufacture the optional DAC observer counters. */
  int rc = a->diagnostics ? 0 : h2_pal_audio_track_drain(m->track, 1000);
  if (rc == 0) rc = stop_phase(m);
  if (rc == 0) rc = drain_recording(a);
  if (rc != 0) return rc;
  const capture_meter_t* c = &a->capture;
  const playback_meter_t* p = &a->playback;
  if (c->result != 0) return c->result;
  if (p->result != 0) return p->result;
  if (a->store != NULL && recorded_phase(a) &&
      a->stored_frames != a->measured)
    return store_failure(a, H2_PAL_ERR_INVALID_STATE);
  uint64_t end = c->end_ms > p->end_ms ? c->end_ms : p->end_ms;
  uint64_t elapsed = end >= a->start_ms ? end - a->start_ms : 0;
  uint64_t nominal =
      (uint64_t)(a->warmup + a->measured) * a->samples * 1000 / RATE;
  int far = a->phase == FAR || a->phase == DOUBLE_TALK;
  char line[480];
  (void)snprintf(
      line, sizeof(line),
      "H2_MFG_AEC_WINDOW phase=%s mode=%s raw_observed=%d dac_observed=%d"
      " samples=%u frames=%u dac=%u active=%u queued=%" PRIu64
      " elapsed_ms=%" PRIu64 " warmup=%u raw_energy=%" PRIu64
      " output_energy=%" PRIu64 " ref_energy=%" PRIu64
      " aec_ref_energy=%" PRIu64
      " peak=%u clipped=%u dac_peak=%u dac_clipped=%u"
      " capture_elapsed_ms=%" PRIu64 " dac_elapsed_ms=%" PRIu64,
      phase_name(a->phase), a->diagnostics ? "diagnostics" : "operator",
      a->diagnostics, a->diagnostics, a->samples, (unsigned)c->frames,
      (unsigned)p->frames, (unsigned)p->active, m->audio_samples / a->samples,
      elapsed, a->warmup, c->mic_energy, c->output_energy, c->reference_energy,
      c->aec_reference_energy, (unsigned)c->peak, (unsigned)c->clipped,
      (unsigned)p->peak, (unsigned)p->clipped,
      c->end_ms >= a->start_ms ? c->end_ms - a->start_ms : 0,
      p->end_ms >= a->start_ms ? p->end_ms - a->start_ms : 0);
  (void)h2_pal_log_write(m->rt->log, H2_PAL_LOG_INFO, "mfg/aec", line);
  if (c->frames != a->measured || elapsed + 1 < nominal ||
      elapsed > nominal * 5 / 4 + 100)
    return H2_PAL_ERR_INVALID_STATE;
  if (a->diagnostics) {
    if (p->frames != a->measured || p->active != (far ? a->measured : 0))
      return H2_PAL_ERR_INVALID_STATE;
  } else if (m->audio_samples / a->samples < a->warmup + a->measured) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  if (c->clipped != 0 || p->clipped != 0) {
    (void)snprintf(m->detail, sizeof(m->detail), "AEC headroom exceeded");
    return H2_PAL_ERR_IO;
  }
  const uint64_t samples = (uint64_t)a->measured * a->samples;
  uint64_t raw = c->mic_energy / samples, output = c->output_energy / samples;
  if (a->phase == NOISE || a->phase == NOISE_AFTER) {
    unsigned index = a->phase == NOISE ? 0 : 1;
    a->noise_raw[index] = raw;
    a->noise_output[index] = output;
    if (a->phase == NOISE) return start_phase(m, FAR);
    rc = a->diagnostics ? evaluate_echo(m) : 0;
    return rc == 0 ? start_phase(m, NEAR) : rc;
  }
  if (a->phase == FAR) {
    a->far_raw = raw;
    a->far_output = output;
    a->far_reference = c->reference_energy / samples;
    a->far_aec_reference = c->aec_reference_energy / samples;
    return start_phase(m, NOISE_AFTER);
  }
  if (a->phase == NEAR) {
    const uint64_t* background =
        a->diagnostics ? a->noise_raw : a->noise_output;
    uint64_t noise =
        background[0] > background[1] ? background[0] : background[1];
    /* Silence or a muted AEC output cannot finish the operator speech check.
     * These are activity prerequisites, not controlled-source retention. */
    uint64_t activity = a->diagnostics ? raw : output;
    if (activity <= (noise + 1) * 10 || output < 16384 ||
        (a->diagnostics && output * 4 < raw))
      return start_phase(m, NEAR);
    a->near_output = output;
    return start_phase(m, DOUBLE_TALK);
  }
  if (output < 16384 || output < (a->far_output + 1) * 10 ||
      output * 4 < a->near_output)
    return start_phase(m, DOUBLE_TALK);
  return start_phase(m, a->diagnostics ? REPLAY : REPLAY_ECHO);
}

/* Owner work between synchronous display strips. This only services the live
 * phase: no worker lifecycle, blocking track drain, transitions or readiness. */
int h2_mfg_aec_service(h2_mfg_t* m, uint64_t now) {
  h2_mfg_aec_t* a = m->aec;
  if (a == NULL || a->atomics_initialized != 2)
    return H2_PAL_ERR_INVALID_STATE;
  int rc = drain_recording(a);
  if (rc == 0)
    rc = h2_atomic_int_load(&a->worker_result, H2_ATOMIC_ACQUIRE);
  if (rc != 0) {
    m->model.ready = 0;
    return rc;
  }
  if (!replay(a->phase)) {
    rc = h2_mfg_audio_tick(m, now, 1);
    /* The microphone's readiness never grants the separate AEC check. */
    m->model.ready = 0;
    if (rc != 0) return rc;
  } else if (a->repeat_ms != 0) {
    /* Only tick starts another listening cycle, even after this deadline. */
    return 0;
  }
  int far = a->phase == FAR || a->phase == DOUBLE_TALK;
  for (unsigned budget = 0; budget < 16; ++budget) {
    if (replay(a->phase) && a->position == a->measured) break;
    int16_t* pcm = m->pcm;
    if (a->store != NULL && replay(a->phase)) {
      if (!a->replay_frame_loaded) {
        rc = a->store->read_frame(a->store->user, recording_index(a),
                                 a->position, pcm, a->samples);
        if (rc != 0) return store_failure(a, rc);
        a->replay_frame_loaded = 1;
      }
    } else {
      for (unsigned i = 0; i < a->samples; ++i) {
        size_t index = (size_t)a->position * a->samples + i;
        pcm[i] = a->phase == REPLAY        ? voice_recording(a)[index]
                 : a->phase == REPLAY_ECHO ? a->recording[index]
                 : far                     ? probe[i % 32]
                                           : 0;
      }
    }
    h2_audio_frame_t frame = h2_audio_frame_for_buffer(
        m->pcm, m->pcm_bytes, m->audio_info.playback_format);
    frame.bytes = m->pcm_bytes;
    rc = h2_pal_audio_track_write(m->track, &frame, 0);
    if (rc == H2_PAL_ERR_WOULD_BLOCK || rc == H2_PAL_ERR_TIMEOUT) break;
    if (rc != 0) {
      m->model.ready = 0;
      return rc;
    }
    if (replay(a->phase)) {
      ++a->position;
      a->replay_frame_loaded = 0;
    }
    m->audio_samples += a->samples;
  }
  rc = drain_recording(a);
  if (rc == 0)
    rc = h2_atomic_int_load(&a->worker_result, H2_ATOMIC_ACQUIRE);
  if (rc != 0) m->model.ready = 0;
  return rc;
}

int h2_mfg_aec_tick(h2_mfg_t* m) {
  h2_mfg_aec_t* a = m->aec;
  if (a == NULL) return H2_PAL_ERR_INVALID_STATE;
  uint64_t now = 0;
  int rc = h2_pal_time_get_monotonic_ms(m->rt->time, &now);
  if (rc != 0) return rc;
  if (replay(a->phase) && a->repeat_ms != 0 && now >= a->repeat_ms) {
    if (a->phase == REPLAY && !a->diagnostics) {
      rc = start_phase(m, REPLAY_ECHO);
      if (rc != 0) return rc;
    } else {
      a->repeat_ms = 0;
    }
  }
  rc = h2_mfg_aec_service(m, now);
  if (rc != 0) return rc;
  if (replay(a->phase) && a->repeat_ms == 0 && a->position == a->measured) {
    rc = h2_pal_audio_track_drain(m->track, 1000);
    if (rc != 0) return rc;
    if (a->phase == REPLAY_ECHO) return start_phase(m, REPLAY);
    a->replayed = 1;
    a->position = 0;
    rc = h2_pal_time_get_monotonic_ms(m->rt->time, &now);
    if (rc != 0) return rc;
    a->repeat_ms = now + 2000;
  }
  m->model.ready = a->replayed;
  (void)snprintf(m->detail, sizeof(m->detail),
                 "AEC phase=%s mode=%s echo_db=%d spk=%" PRIu32 " mic=%" PRIu32
                 " level=%u frames=%u"
                 " voice=%s",
                 phase_name(a->phase),
                 a->diagnostics ? "diagnostics" : "operator",
                 h2_mfg_aec_echo_db(m), a->volume, a->mic_gain, m->mic_level,
                 m->audio_frames, a->replayed ? "operator-confirm" : "pending");
  if (!replay(a->phase)) {
    uint64_t nominal =
        (uint64_t)(a->warmup + a->measured) * a->samples * 1000 / RATE;
    uint64_t elapsed = now >= a->start_ms ? now - a->start_ms : 0;
    unsigned completed = h2_atomic_uint_load(&a->completed, H2_ATOMIC_ACQUIRE);
    int complete = (completed & CAPTURE_COMPLETE) != 0 &&
                   (a->diagnostics
                        ? (completed & PLAYBACK_COMPLETE) != 0
                        : m->audio_samples / a->samples >=
                              a->warmup + a->measured);
    /* Completion publications avoid racing the worker-owned meters. Preserve
     * the existing cadence limit, then join and run every strict window check. */
    if ((elapsed >= nominal && complete) || elapsed >= nominal * 5 / 4 + 100)
      return finish_window(m);
  }
  return 0;
}
