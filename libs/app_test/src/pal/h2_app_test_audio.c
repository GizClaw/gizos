#include "h2_app_test_audio.h"

#include "h2/pal/os/h2_pal_mem.h"

#include <stdatomic.h>
#include <string.h>

typedef struct h2_app_test_audio_track {
  h2_pal_audio_track_t api;
  h2_pal_audio_track_t *delegate;
  struct h2_app_test_audio *owner;
} h2_app_test_audio_track_t;

struct h2_app_test_audio {
  h2_pal_audio_api_t api;
  const h2_pal_mem_api_t *mem;
  const h2_pal_time_api_t *time;
  const h2_pal_audio_api_t *delegate;
  h2_app_test_audio_fixture_t fixture;
  const uint8_t *fixture_pcm;
  size_t fixture_pcm_size;
  size_t fixture_offset;
  uint64_t fixture_epoch_ms;
  uint64_t fixture_samples_emitted;
  uint64_t fixture_next_due_ms;
  h2_audio_info_t info;
  uint8_t scratch[H2_APP_TEST_AUDIO_SCRATCH_MAX];
  atomic_flag fixture_lock;
  /* Low bit is enabled; each transition increments the unsigned generation. */
  atomic_uint_least32_t capture_state;
  uint_least32_t fixture_capture_state;
  bool fixture_clock_needs_reset;
  atomic_bool mic_active;
  atomic_uint_least32_t mic_start_count;
  atomic_uint_least32_t mic_read_count;
  atomic_uint_least32_t fixture_bytes_emitted;
  atomic_bool fixture_complete;
  atomic_uint_least32_t real_capture_frames;
  atomic_uint_least32_t real_capture_no_frame;
  atomic_int real_capture_first_error;
  atomic_int real_capture_last_error;
  atomic_uint_least32_t mic_stop_count;
  atomic_uint_least32_t speaker_start_count;
  atomic_uint_least32_t speaker_stop_count;
  atomic_bool speaker_active;
  atomic_uint_least32_t track_create_count;
  atomic_uint_least32_t track_write_count;
  atomic_uint_least32_t track_drain_count;
  atomic_uint_least32_t track_close_count;
  atomic_uint_least32_t active_tracks;
  atomic_uint_least32_t playback_bytes;
  atomic_uint_least32_t playback_digest;
  atomic_uint_least32_t playback_peak;
};

static bool valid_fixture(const h2_app_test_audio_fixture_t *fixture) {
  if (fixture == NULL || fixture->pcm == NULL || fixture->size == 0u ||
      fixture->format.sample_rate_hz == 0u ||
      fixture->format.frame_samples_per_channel == 0u) {
    return false;
  }
  const size_t sample_bytes = h2_audio_pcm_frame_bytes(&fixture->format);
  return sample_bytes != 0u && fixture->size % sample_bytes == 0u &&
         sample_bytes * fixture->format.frame_samples_per_channel <=
             H2_APP_TEST_AUDIO_SCRATCH_MAX;
}

static void counter_add_saturated(atomic_uint_least32_t *counter,
                                  uint_least32_t increment) {
  uint_least32_t current = atomic_load_explicit(counter, memory_order_relaxed);
  for (;;) {
    const uint_least32_t desired =
        increment > UINT32_MAX - current ? UINT32_MAX : current + increment;
    if (atomic_compare_exchange_weak_explicit(counter, &current, desired,
                                              memory_order_relaxed,
                                              memory_order_relaxed)) {
      return;
    }
  }
}

static h2_pal_result_t fixture_lock(h2_app_test_audio_t *audio) {
  while (atomic_flag_test_and_set_explicit(&audio->fixture_lock,
                                           memory_order_acquire)) {
    /* The mic reader runs at a higher priority than the fixture selector.
     * A pure spin on the holder's core never lets the holder release the
     * lock and starves that core's idle task until the watchdog aborts, so
     * back off through the yielding sleep create() requires and report a
     * failed sleep instead of spinning. */
    const h2_pal_result_t rc = h2_pal_time_sleep_ms(audio->time, 1u);
    if (rc != H2_PAL_OK)
      return rc;
  }
  return H2_PAL_OK;
}

static void fixture_unlock(h2_app_test_audio_t *audio) {
  atomic_flag_clear_explicit(&audio->fixture_lock, memory_order_release);
}

static int decorated_get_info(void *user, h2_audio_info_t *info) {
  h2_app_test_audio_t *audio = user;
  int rc = h2_pal_audio_get_info(audio->delegate, info);
  const h2_pal_result_t lock_rc = fixture_lock(audio);
  if (lock_rc != H2_PAL_OK)
    return lock_rc;
  if (rc == H2_PAL_OK && audio->fixture.pcm == NULL) info->mic_supported = 0u;
  fixture_unlock(audio);
  return rc;
}

static int decorated_start_mic(void *user) {
  h2_app_test_audio_t *audio = user;
  const h2_pal_result_t lock_rc = fixture_lock(audio);
  if (lock_rc != H2_PAL_OK)
    return lock_rc;
  if (audio->fixture.pcm == NULL) {
    fixture_unlock(audio);
    return H2_PAL_ERR_UNSUPPORTED;
  }
  if (atomic_load_explicit(&audio->mic_active, memory_order_acquire)) {
    fixture_unlock(audio);
    return H2_PAL_ERR_INVALID_STATE;
  }
  atomic_store_explicit(&audio->mic_active, true, memory_order_release);
  const h2_app_test_audio_fixture_t fixture = audio->fixture;
  audio->fixture_pcm = fixture.pcm;
  audio->fixture_pcm_size = fixture.size;
  fixture_unlock(audio);
  audio->fixture_offset = 0u;
  audio->fixture_samples_emitted = 0u;
  int rc = h2_pal_time_get_monotonic_ms(audio->time, &audio->fixture_epoch_ms);
  if (rc != H2_PAL_OK) {
    goto fail;
  }
  audio->fixture_next_due_ms = audio->fixture_epoch_ms;
  audio->fixture_clock_needs_reset = false;
  audio->fixture_capture_state =
      atomic_load_explicit(&audio->capture_state, memory_order_acquire);
  atomic_store_explicit(&audio->fixture_bytes_emitted, 0u,
                        memory_order_release);
  atomic_store_explicit(&audio->fixture_complete, false, memory_order_release);
  atomic_store_explicit(&audio->real_capture_frames, 0u, memory_order_release);
  atomic_store_explicit(&audio->real_capture_no_frame, 0u,
                        memory_order_release);
  atomic_store_explicit(&audio->real_capture_first_error, H2_PAL_OK,
                        memory_order_release);
  atomic_store_explicit(&audio->real_capture_last_error, H2_PAL_OK,
                        memory_order_release);
  rc = h2_pal_audio_get_info(audio->delegate, &audio->info);
  if (rc != H2_PAL_OK) {
    goto fail;
  }
  if (!audio->info.available || !audio->info.mic_supported ||
      audio->info.mic_format.sample_rate_hz != fixture.format.sample_rate_hz ||
      audio->info.mic_format.channels != fixture.format.channels ||
      audio->info.mic_format.frame_samples_per_channel !=
          fixture.format.frame_samples_per_channel ||
      audio->info.mic_format.sample_format != H2_AUDIO_SAMPLE_S16LE) {
    rc = H2_PAL_ERR_FORMAT;
    goto fail;
  }
  const size_t frame_bytes = h2_audio_pcm_frame_bytes(&audio->info.mic_format) *
                             audio->info.mic_format.frame_samples_per_channel;
  if (frame_bytes == 0u || frame_bytes > sizeof(audio->scratch)) {
    rc = H2_PAL_ERR_NO_SPACE;
    goto fail;
  }
  rc = h2_pal_audio_start_mic(audio->delegate);
  if (rc != H2_PAL_OK) {
    goto fail;
  }
  counter_add_saturated(&audio->mic_start_count, 1u);
  return rc;

fail:
  atomic_store_explicit(&audio->mic_active, false, memory_order_release);
  return rc;
}

static int decorated_stop_mic(void *user) {
  h2_app_test_audio_t *audio = user;
  const int rc = h2_pal_audio_stop_mic(audio->delegate);
  if (rc == H2_PAL_OK) {
    counter_add_saturated(&audio->mic_stop_count, 1u);
    atomic_store_explicit(&audio->mic_active, false, memory_order_release);
  }
  return rc;
}

static int decorated_start_speaker(void *user) {
  h2_app_test_audio_t *audio = user;
  const int rc = h2_pal_audio_start_speaker(audio->delegate);
  if (rc == H2_PAL_OK) {
    counter_add_saturated(&audio->speaker_start_count, 1u);
    atomic_store_explicit(&audio->speaker_active, true, memory_order_release);
  }
  return rc;
}

static int decorated_stop_speaker(void *user) {
  h2_app_test_audio_t *audio = user;
  const int rc = h2_pal_audio_stop_speaker(audio->delegate);
  if (rc == H2_PAL_OK) {
    counter_add_saturated(&audio->speaker_stop_count, 1u);
    atomic_store_explicit(&audio->speaker_active, false, memory_order_release);
  }
  return rc;
}

static void record_real_error(h2_app_test_audio_t *audio, int rc) {
  int expected = H2_PAL_OK;
  (void)atomic_compare_exchange_strong_explicit(
      &audio->real_capture_first_error, &expected, rc, memory_order_release,
      memory_order_relaxed);
  atomic_store_explicit(&audio->real_capture_last_error, rc,
                        memory_order_release);
}

/* Only the serialized mic reader owns pacing. The callback publishes a
 * generation so even a pause/resume between two reads rebases the clock. */
static bool prepare_fixture_clock(h2_app_test_audio_t *audio, uint64_t now_ms) {
  const uint_least32_t state =
      atomic_load_explicit(&audio->capture_state, memory_order_acquire);
  if ((state & 1u) == 0u) {
    return false;
  }
  if (audio->fixture_clock_needs_reset ||
      state != audio->fixture_capture_state) {
    audio->fixture_clock_needs_reset = false;
    audio->fixture_capture_state = state;
    audio->fixture_epoch_ms = now_ms;
    audio->fixture_next_due_ms = now_ms;
    audio->fixture_samples_emitted = 0u;
  }
  return true;
}

static int decorated_mic_read(void *user, h2_audio_frame_t *out_frame,
                              uint32_t timeout_ms) {
  h2_app_test_audio_t *audio = user;
  const size_t frame_bytes = h2_audio_pcm_frame_bytes(&audio->info.mic_format) *
                             audio->info.mic_format.frame_samples_per_channel;
  if (out_frame == NULL || out_frame->data == NULL ||
      out_frame->capacity < frame_bytes || frame_bytes == 0u) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  out_frame->bytes = 0u;
  if (!atomic_load_explicit(&audio->mic_active, memory_order_acquire)) {
    return H2_PAL_ERR_INVALID_STATE;
  }

  h2_audio_frame_t real = h2_audio_frame_for_buffer(
      audio->scratch, sizeof(audio->scratch), audio->info.mic_format);
  const int real_rc = h2_pal_audio_mic_read(audio->delegate, &real, 0u);
  if (real_rc == H2_PAL_OK && real.bytes != 0u) {
    counter_add_saturated(&audio->real_capture_frames, 1u);
  } else if (real_rc == H2_PAL_ERR_WOULD_BLOCK ||
             real_rc == H2_PAL_ERR_TIMEOUT ||
             (real_rc == H2_PAL_OK && real.bytes == 0u)) {
    counter_add_saturated(&audio->real_capture_no_frame, 1u);
  } else {
    record_real_error(audio, real_rc);
  }
  /*
   * Physical capture health is independent evidence.  It must never pace or
   * suppress the deterministic product input: otherwise a transient empty
   * hardware queue changes which fixture samples reach the server and makes
   * the business result depend on room timing.
   */
  memset(audio->scratch, 0, sizeof(audio->scratch));

  uint64_t now_ms = 0u;
  const h2_pal_result_t time_rc =
      h2_pal_time_get_monotonic_ms(audio->time, &now_ms);
  if (time_rc != H2_PAL_OK) {
    return time_rc;
  }
  h2_pal_result_t lock_rc = fixture_lock(audio);
  if (lock_rc != H2_PAL_OK) {
    return lock_rc;
  }
  if (!prepare_fixture_clock(audio, now_ms)) {
    fixture_unlock(audio);
    return H2_PAL_ERR_WOULD_BLOCK;
  }
  if (audio->fixture_pcm_size == 0u) {
    fixture_unlock(audio);
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (now_ms < audio->fixture_next_due_ms) {
    const uint64_t wait_ms = audio->fixture_next_due_ms - now_ms;
    if (timeout_ms == 0u) {
      fixture_unlock(audio);
      return H2_PAL_ERR_WOULD_BLOCK;
    }
    if (wait_ms > timeout_ms) {
      fixture_unlock(audio);
      const h2_pal_result_t sleep_rc =
          h2_pal_time_sleep_ms(audio->time, timeout_ms);
      return sleep_rc == H2_PAL_OK ? H2_PAL_ERR_TIMEOUT : sleep_rc;
    }
    fixture_unlock(audio);
    const h2_pal_result_t sleep_rc =
        h2_pal_time_sleep_ms(audio->time, (uint32_t)wait_ms);
    if (sleep_rc != H2_PAL_OK) {
      return sleep_rc;
    }
    const h2_pal_result_t refresh_rc =
        h2_pal_time_get_monotonic_ms(audio->time, &now_ms);
    if (refresh_rc != H2_PAL_OK) {
      return refresh_rc;
    }
    lock_rc = fixture_lock(audio);
    if (lock_rc != H2_PAL_OK) {
      return lock_rc;
    }
    if (!prepare_fixture_clock(audio, now_ms)) {
      fixture_unlock(audio);
      return H2_PAL_ERR_WOULD_BLOCK;
    }
  }

  const uint64_t frame_samples =
      audio->info.mic_format.frame_samples_per_channel;
  if (audio->fixture_samples_emitted > UINT64_MAX - frame_samples) {
    out_frame->bytes = 0u;
    fixture_unlock(audio);
    return H2_PAL_ERR_NO_SPACE;
  }
  const uint64_t next_samples = audio->fixture_samples_emitted + frame_samples;
  const uint64_t sample_rate = audio->info.mic_format.sample_rate_hz;
  if (next_samples > UINT64_MAX / UINT64_C(1000)) {
    out_frame->bytes = 0u;
    fixture_unlock(audio);
    return H2_PAL_ERR_NO_SPACE;
  }
  const uint64_t elapsed_numerator = next_samples * UINT64_C(1000);
  if (elapsed_numerator > UINT64_MAX - (sample_rate - 1u)) {
    out_frame->bytes = 0u;
    fixture_unlock(audio);
    return H2_PAL_ERR_NO_SPACE;
  }
  const uint64_t due_offset_ms =
      (elapsed_numerator + sample_rate - 1u) / sample_rate;
  if (audio->fixture_epoch_ms > UINT64_MAX - due_offset_ms) {
    out_frame->bytes = 0u;
    fixture_unlock(audio);
    return H2_PAL_ERR_NO_SPACE;
  }
  /* A delayed consumer must not burst old frames into a realtime uplink.
   * Preserve fractional-rate rounding while moving the epoch past a stall. */
  const uint64_t lateness = now_ms > audio->fixture_next_due_ms
                              ? now_ms - audio->fixture_next_due_ms : 0u;
  if (lateness > UINT64_MAX - audio->fixture_epoch_ms - due_offset_ms) {
    fixture_unlock(audio);
    return H2_PAL_ERR_NO_SPACE;
  }
  audio->fixture_epoch_ms += lateness;
  uint8_t *output = out_frame->data;
  for (size_t index = 0u; index < frame_bytes; ++index) {
    output[index] = audio->fixture_offset < audio->fixture_pcm_size
                        ? audio->fixture_pcm[audio->fixture_offset++]
                        : 0u;
  }
  out_frame->bytes = frame_bytes;
  out_frame->sample_rate_hz = audio->info.mic_format.sample_rate_hz;
  out_frame->samples_per_channel =
      audio->info.mic_format.frame_samples_per_channel;
  out_frame->channels = audio->info.mic_format.channels;
  out_frame->sample_format = audio->info.mic_format.sample_format;
  audio->fixture_samples_emitted = next_samples;
  audio->fixture_next_due_ms = audio->fixture_epoch_ms + due_offset_ms;
  counter_add_saturated(&audio->mic_read_count, 1u);
  atomic_store_explicit(&audio->fixture_bytes_emitted,
                        audio->fixture_offset > UINT32_MAX
                            ? UINT32_MAX
                            : (uint_least32_t)audio->fixture_offset,
                        memory_order_release);
  atomic_store_explicit(&audio->fixture_complete,
                        audio->fixture_offset == audio->fixture_pcm_size,
                        memory_order_release);
  fixture_unlock(audio);
  return H2_PAL_OK;
}

static int decorated_track_write(h2_pal_audio_track_t *track,
                                 const h2_audio_frame_t *frame,
                                 uint32_t timeout_ms) {
  h2_app_test_audio_track_t *wrapped = track->user;
  if (frame == NULL || frame->data == NULL || frame->bytes == 0u ||
      frame->bytes > frame->capacity) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  const int rc = h2_pal_audio_track_write(wrapped->delegate, frame, timeout_ms);
  if (rc == H2_PAL_OK) {
    uint32_t digest = UINT32_C(2166136261);
    const uint8_t *bytes = frame->data;
    for (size_t index = 0u; index < frame->bytes; ++index) {
      digest ^= bytes[index];
      digest *= UINT32_C(16777619);
    }
    if (frame->sample_format == H2_AUDIO_SAMPLE_S16LE) {
      uint_least32_t peak = 0u;
      for (size_t i = 0u; i + 1u < frame->bytes; i += 2u) {
        int value = (int16_t)((unsigned)bytes[i] | ((unsigned)bytes[i + 1u] << 8));
        uint_least32_t amplitude = (uint_least32_t)(value < 0 ? -value : value);
        if (amplitude > peak) peak = amplitude;
      }
      uint_least32_t previous = atomic_load(&wrapped->owner->playback_peak);
      while (previous < peak && !atomic_compare_exchange_weak(
          &wrapped->owner->playback_peak, &previous, peak)) {}
    }
    atomic_fetch_add_explicit(&wrapped->owner->playback_digest, digest,
                              memory_order_relaxed);
    counter_add_saturated(&wrapped->owner->track_write_count, 1u);
    counter_add_saturated(
        &wrapped->owner->playback_bytes,
        frame->bytes > UINT32_MAX ? UINT32_MAX : (uint_least32_t)frame->bytes);
  }
  return rc;
}

static int decorated_track_close(h2_pal_audio_track_t *track) {
  h2_app_test_audio_track_t *wrapped = track->user;
  h2_app_test_audio_t *owner = wrapped->owner;
  const int rc = h2_pal_audio_track_close(wrapped->delegate);
  if (rc != H2_PAL_OK) {
    /*
     * A failed delegated close does not terminate ownership.  Keep both the
     * wrapper and the active count so callers may retry and quiescence cannot
     * hide a live provider track.
     */
    return rc;
  }
  counter_add_saturated(&owner->track_close_count, 1u);
  h2_pal_mem_free(owner->mem, wrapped);
  atomic_fetch_sub_explicit(&owner->active_tracks, 1u, memory_order_release);
  return rc;
}

static int decorated_track_get_volume(h2_pal_audio_track_t *track,
                                      uint32_t *out_factor_milli) {
  h2_app_test_audio_track_t *wrapped = track->user;
  return h2_pal_audio_track_get_volume_factor(wrapped->delegate,
                                              out_factor_milli);
}

static int decorated_track_set_volume(h2_pal_audio_track_t *track,
                                      uint32_t factor_milli) {
  h2_app_test_audio_track_t *wrapped = track->user;
  return h2_pal_audio_track_set_volume_factor(wrapped->delegate, factor_milli);
}

static int decorated_track_drain(h2_pal_audio_track_t *track,
                                 uint32_t timeout_ms) {
  h2_app_test_audio_track_t *wrapped = track->user;
  const int rc = h2_pal_audio_track_drain(wrapped->delegate, timeout_ms);
  if (rc == H2_PAL_OK) {
    counter_add_saturated(&wrapped->owner->track_drain_count, 1u);
  }
  return rc;
}

static int decorated_create_track(void *user,
                                  const h2_audio_track_config_t *config,
                                  h2_pal_audio_track_t **out_track) {
  h2_app_test_audio_t *audio = user;
  if (out_track == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  *out_track = NULL;
  uint_least32_t active =
      atomic_load_explicit(&audio->active_tracks, memory_order_acquire);
  for (;;) {
    if (active >= H2_APP_TEST_AUDIO_TRACKS_MAX) {
      return H2_PAL_ERR_NO_SPACE;
    }
    if (atomic_compare_exchange_weak_explicit(&audio->active_tracks, &active,
                                              active + 1u, memory_order_acq_rel,
                                              memory_order_acquire)) {
      break;
    }
  }
  h2_app_test_audio_track_t *wrapped =
      h2_pal_mem_alloc(audio->mem, sizeof(*wrapped));
  if (wrapped == NULL) {
    atomic_fetch_sub_explicit(&audio->active_tracks, 1u, memory_order_release);
    return H2_PAL_ERR_NO_MEMORY;
  }
  memset(wrapped, 0, sizeof(*wrapped));
  int rc =
      h2_pal_audio_create_track(audio->delegate, config, &wrapped->delegate);
  if (rc == H2_PAL_OK && wrapped->delegate == NULL) {
    rc = H2_PAL_ERR_INVALID_STATE;
  }
  if (rc != H2_PAL_OK) {
    h2_pal_mem_free(audio->mem, wrapped);
    atomic_fetch_sub_explicit(&audio->active_tracks, 1u, memory_order_release);
    return rc;
  }
  wrapped->owner = audio;
  wrapped->api = (h2_pal_audio_track_t){
      .user = wrapped,
      .audio = &audio->api,
      .write = decorated_track_write,
      .close = decorated_track_close,
      .get_volume_factor = decorated_track_get_volume,
      .set_volume_factor = decorated_track_set_volume,
      .drain = decorated_track_drain,
  };
  counter_add_saturated(&audio->track_create_count, 1u);
  *out_track = &wrapped->api;
  return H2_PAL_OK;
}

static int decorated_get_volume(void *user, uint32_t *out_percent) {
  h2_app_test_audio_t *audio = user;
  return h2_pal_audio_get_speaker_volume_percent(audio->delegate, out_percent);
}

static int decorated_set_volume(void *user, uint32_t percent) {
  h2_app_test_audio_t *audio = user;
  return h2_pal_audio_set_speaker_volume_percent(audio->delegate, percent);
}

static const h2_pal_audio_vtable_t s_audio_vtable = {
    .get_info = decorated_get_info,
    .start_mic = decorated_start_mic,
    .stop_mic = decorated_stop_mic,
    .start_speaker = decorated_start_speaker,
    .stop_speaker = decorated_stop_speaker,
    .mic_read = decorated_mic_read,
    .create_track = decorated_create_track,
    .get_speaker_volume_percent = decorated_get_volume,
    .set_speaker_volume_percent = decorated_set_volume,
};

h2_pal_result_t
h2_app_test_audio_create(const h2_pal_mem_api_t *mem,
                         const h2_pal_time_api_t *time,
                         const h2_pal_audio_api_t *delegate,
                         const h2_app_test_audio_fixture_t *fixture,
                         h2_app_test_audio_t **out_audio) {
  if (out_audio == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  *out_audio = NULL;
  if (mem == NULL || mem->vtable == NULL || mem->vtable->alloc == NULL ||
      mem->vtable->free == NULL || time == NULL || time->vtable == NULL ||
      time->vtable->get_monotonic_ms == NULL ||
      time->vtable->sleep_ms == NULL || delegate == NULL ||
      delegate->vtable == NULL || (fixture != NULL && !valid_fixture(fixture))) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  h2_app_test_audio_t *audio = h2_pal_mem_alloc(mem, sizeof(*audio));
  if (audio == NULL) {
    return H2_PAL_ERR_NO_MEMORY;
  }
  memset(audio, 0, sizeof(*audio));
  atomic_flag_clear(&audio->fixture_lock);
  atomic_init(&audio->mic_active, false);
  atomic_init(&audio->capture_state, 1u);
  atomic_init(&audio->mic_start_count, 0u);
  atomic_init(&audio->mic_read_count, 0u);
  atomic_init(&audio->fixture_bytes_emitted, 0u);
  atomic_init(&audio->fixture_complete, false);
  atomic_init(&audio->real_capture_frames, 0u);
  atomic_init(&audio->real_capture_no_frame, 0u);
  atomic_init(&audio->real_capture_first_error, H2_PAL_OK);
  atomic_init(&audio->real_capture_last_error, H2_PAL_OK);
  atomic_init(&audio->mic_stop_count, 0u);
  atomic_init(&audio->speaker_start_count, 0u);
  atomic_init(&audio->speaker_stop_count, 0u);
  atomic_init(&audio->speaker_active, false);
  atomic_init(&audio->track_create_count, 0u);
  atomic_init(&audio->track_write_count, 0u);
  atomic_init(&audio->track_drain_count, 0u);
  atomic_init(&audio->track_close_count, 0u);
  atomic_init(&audio->active_tracks, 0u);
  atomic_init(&audio->playback_bytes, 0u);
  atomic_init(&audio->playback_digest, 0u);
  atomic_init(&audio->playback_peak, 0u);
  audio->api.user = audio;
  audio->api.vtable = &s_audio_vtable;
  audio->mem = mem;
  audio->time = time;
  audio->delegate = delegate;
  if (fixture != NULL) {
    audio->fixture = *fixture;
    audio->fixture_pcm = fixture->pcm;
    audio->fixture_pcm_size = fixture->size;
  }
  *out_audio = audio;
  return H2_PAL_OK;
}

const h2_pal_audio_api_t *h2_app_test_audio_api(h2_app_test_audio_t *audio) {
  return audio == NULL ? NULL : &audio->api;
}

void h2_app_test_audio_set_capture_active(h2_app_test_audio_t *audio,
                                         bool active) {
  if (audio == NULL) {
    return;
  }
  uint_least32_t state =
      atomic_load_explicit(&audio->capture_state, memory_order_acquire);
  while (((state & 1u) != 0u) != active) {
    if (atomic_compare_exchange_weak_explicit(
            &audio->capture_state, &state, state + (uint_least32_t)1u,
            memory_order_acq_rel, memory_order_acquire)) {
      return;
    }
  }
}

h2_pal_result_t
h2_app_test_audio_set_fixture(h2_app_test_audio_t *audio,
                              const h2_app_test_audio_fixture_t *fixture) {
  if (audio == NULL || !valid_fixture(fixture)) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  const h2_pal_result_t lock_rc = fixture_lock(audio);
  if (lock_rc != H2_PAL_OK) {
    return lock_rc;
  }
  const bool mic_active =
      atomic_load_explicit(&audio->mic_active, memory_order_acquire);
  if (mic_active &&
      (atomic_load_explicit(&audio->capture_state, memory_order_acquire) & 1u)) {
    fixture_unlock(audio);
    return H2_PAL_ERR_INVALID_STATE;
  }
  if (mic_active &&
      (fixture->format.sample_rate_hz != audio->info.mic_format.sample_rate_hz ||
       fixture->format.channels != audio->info.mic_format.channels ||
       fixture->format.frame_samples_per_channel !=
           audio->info.mic_format.frame_samples_per_channel ||
       fixture->format.sample_format != audio->info.mic_format.sample_format)) {
    fixture_unlock(audio);
    return H2_PAL_ERR_FORMAT;
  }
  audio->fixture = *fixture;
  audio->fixture_pcm = fixture->pcm;
  audio->fixture_pcm_size = fixture->size;
  audio->fixture_offset = 0u;
  audio->fixture_samples_emitted = 0u;
  audio->fixture_clock_needs_reset = true;
  atomic_store_explicit(&audio->fixture_bytes_emitted, 0u,
                        memory_order_release);
  atomic_store_explicit(&audio->fixture_complete, false, memory_order_release);
  fixture_unlock(audio);
  return H2_PAL_OK;
}

h2_pal_result_t
h2_app_test_audio_copy_evidence(h2_app_test_audio_t *audio,
                                h2_app_test_audio_evidence_t *out_evidence) {
  if (audio == NULL || out_evidence == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  out_evidence->mic_active =
      atomic_load_explicit(&audio->mic_active, memory_order_acquire);
  out_evidence->mic_start_count =
      atomic_load_explicit(&audio->mic_start_count, memory_order_acquire);
  out_evidence->mic_read_count =
      atomic_load_explicit(&audio->mic_read_count, memory_order_acquire);
  out_evidence->fixture_bytes_emitted =
      atomic_load_explicit(&audio->fixture_bytes_emitted, memory_order_acquire);
  out_evidence->fixture_complete =
      atomic_load_explicit(&audio->fixture_complete, memory_order_acquire);
  out_evidence->real_capture_frames =
      atomic_load_explicit(&audio->real_capture_frames, memory_order_acquire);
  out_evidence->real_capture_no_frame =
      atomic_load_explicit(&audio->real_capture_no_frame, memory_order_acquire);
  out_evidence->real_capture_first_error = atomic_load_explicit(
      &audio->real_capture_first_error, memory_order_acquire);
  out_evidence->real_capture_last_error = atomic_load_explicit(
      &audio->real_capture_last_error, memory_order_acquire);
  out_evidence->mic_stop_count =
      atomic_load_explicit(&audio->mic_stop_count, memory_order_acquire);
  out_evidence->speaker_start_count =
      atomic_load_explicit(&audio->speaker_start_count, memory_order_acquire);
  out_evidence->speaker_stop_count =
      atomic_load_explicit(&audio->speaker_stop_count, memory_order_acquire);
  out_evidence->speaker_active =
      atomic_load_explicit(&audio->speaker_active, memory_order_acquire);
  out_evidence->track_create_count =
      atomic_load_explicit(&audio->track_create_count, memory_order_acquire);
  out_evidence->track_write_count =
      atomic_load_explicit(&audio->track_write_count, memory_order_acquire);
  out_evidence->track_drain_count =
      atomic_load_explicit(&audio->track_drain_count, memory_order_acquire);
  out_evidence->track_close_count =
      atomic_load_explicit(&audio->track_close_count, memory_order_acquire);
  out_evidence->active_track_count = (uint32_t)atomic_load_explicit(
      &audio->active_tracks, memory_order_acquire);
  out_evidence->playback_bytes =
      atomic_load_explicit(&audio->playback_bytes, memory_order_acquire);
  out_evidence->playback_peak = atomic_load(&audio->playback_peak);
  out_evidence->playback_digest =
      atomic_load_explicit(&audio->playback_digest, memory_order_acquire);
  return H2_PAL_OK;
}

h2_pal_result_t h2_app_test_audio_destroy(h2_app_test_audio_t *audio) {
  if (audio == NULL) {
    return H2_PAL_OK;
  }
  if (atomic_load_explicit(&audio->mic_active, memory_order_acquire) ||
      atomic_load_explicit(&audio->speaker_active, memory_order_acquire)) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  if (atomic_load_explicit(&audio->active_tracks, memory_order_acquire) != 0u) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  h2_pal_mem_free(audio->mem, audio);
  return H2_PAL_OK;
}
