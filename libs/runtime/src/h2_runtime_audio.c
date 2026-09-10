#include "h2_runtime_internal.h"

/* Level measurement is observational and costs code the smallest targets
 * cannot spare: bk3633 is a BLE-only part with no audio path at all, and its
 * OAD image budget is nearly full. Products that build it out keep the API
 * and get H2_PAL_ERR_UNSUPPORTED from the getter. */
#ifndef H2_RUNTIME_AUDIO_LEVELS
#define H2_RUNTIME_AUDIO_LEVELS 1
#endif

#define H2_RUNTIME_AUDIO_LEVEL_VALID 0x100u

static const h2_pal_audio_api_t *backend(h2_runtime_t *runtime) {
    return &runtime->private_state->audio_backend;
}

h2_pal_result_t h2_runtime_system_state_audio(
    const h2_runtime_t *runtime, h2_runtime_system_audio_state_t *out_state) {
    if (!h2_runtime_ready(runtime) || !out_state)
        return H2_PAL_ERR_INVALID_ARG;
    h2_runtime_private_t *state = runtime->private_state;
    if (atomic_flag_test_and_set(&state->audio_state_busy))
        return H2_PAL_ERR_BUSY;
    uint32_t actual = 0;
    int rc = h2_pal_audio_get_speaker_volume_percent(&state->audio_backend, &actual);
    if (rc == H2_PAL_OK && actual > 100u)
        rc = H2_PAL_ERR_FORMAT;
    if (rc == H2_PAL_OK) {
        uint32_t expected = state->audio_state.muted ? 0 : state->audio_state.volume_percent;
        /* Reconcile direct backend changes, without losing the logical volume
         * when the shared state intentionally silences the hardware. */
        if (!state->audio_state_valid || actual != expected) {
            state->audio_state.volume_percent = actual;
            state->audio_state.muted = actual == 0;
            state->audio_state_valid = true;
        }
        *out_state = state->audio_state;
    }
    atomic_flag_clear(&state->audio_state_busy);
    return rc;
}

h2_pal_result_t h2_runtime_audio_set_volume(
    h2_runtime_t *runtime, uint32_t percent, uint8_t muted) {
    if (!h2_runtime_ready(runtime) || percent > 100u || muted > 1u)
        return H2_PAL_ERR_INVALID_ARG;
    h2_runtime_private_t *state = runtime->private_state;
    if (atomic_flag_test_and_set(&state->audio_state_busy))
        return H2_PAL_ERR_BUSY;
    int rc = h2_pal_audio_set_speaker_volume_percent(backend(runtime), muted ? 0 : percent);
    if (rc == H2_PAL_OK) {
        state->audio_state.volume_percent = percent;
        state->audio_state.muted = muted;
        state->audio_state_valid = true;
    }
    atomic_flag_clear(&state->audio_state_busy);
    return rc;
}

static int get_info(void *user, h2_audio_info_t *info) {
    return h2_pal_audio_get_info(backend(user), info);
}
static int start_mic(void *user) { return h2_pal_audio_start_mic(backend(user)); }
static int stop_mic(void *user) { return h2_pal_audio_stop_mic(backend(user)); }
static int start_speaker(void *user) { return h2_pal_audio_start_speaker(backend(user)); }
static int stop_speaker(void *user) { return h2_pal_audio_stop_speaker(backend(user)); }
#if H2_RUNTIME_AUDIO_LEVELS
/*
 * Peak, not RMS: the peak of a frame rises on the first loud sample of a
 * speech onset, so a meter driven from it starts moving in the same frame,
 * and it costs one comparison per sample on a path that already runs at the
 * audio rate. An RMS would cost a multiply per sample and would still trail
 * the onset by most of the averaging window.
 */
static uint8_t frame_peak_percent(const h2_audio_frame_t *frame) {
    /* h2_audio_frame_t::data is an unconstrained void *, so the samples are
     * read with memcpy: a byte-aligned S16LE buffer would make an int16_t
     * cast undefined and can fault on the ARM targets. */
    const unsigned char *bytes = (const unsigned char *)frame->data;
    size_t count = frame->bytes / sizeof(int16_t);
    uint32_t peak = 0u;
    for (size_t i = 0u; i < count; ++i) {
        int16_t sample_value = 0;
        memcpy(&sample_value, bytes + i * sizeof(int16_t), sizeof(sample_value));
        int32_t sample = sample_value;
        uint32_t magnitude = (uint32_t)(sample < 0 ? -sample : sample);
        if (magnitude > peak)
            peak = magnitude;
    }
    if (peak > 32768u)
        peak = 32768u;
    return (uint8_t)((peak * 100u) / 32768u);
}

/* Publish one frame level. Frames the Runtime cannot measure (a foreign
 * sample format, an empty or absent buffer) leave the previous value alone,
 * so a meter keeps showing the last real measurement instead of dropping to
 * zero on a format the Runtime does not read. */
static void publish_level(h2_runtime_t *runtime, atomic_uint *level,
                          atomic_uint *level_ms, const h2_audio_frame_t *frame) {
    if (frame == NULL || frame->data == NULL ||
        frame->sample_format != H2_AUDIO_SAMPLE_S16LE ||
        frame->bytes < sizeof(int16_t))
        return;
    uint64_t now = 0u;
    /* Timestamp zero means "never measured", so a clock the Runtime cannot
     * read leaves the previous measurement in place rather than publishing a
     * valid sample that looks like no sample at all. */
    if (h2_pal_time_get_monotonic_ms(runtime->time, &now) != H2_PAL_OK)
        return;
    atomic_store_explicit(level_ms, (unsigned int)(uint32_t)now, memory_order_relaxed);
    atomic_store_explicit(
        level, H2_RUNTIME_AUDIO_LEVEL_VALID | frame_peak_percent(frame),
        memory_order_relaxed);
}

#endif /* H2_RUNTIME_AUDIO_LEVELS */

static int mic_read(void *user, h2_audio_frame_t *frame, uint32_t timeout_ms) {
    h2_runtime_t *runtime = user;
    int rc = h2_pal_audio_mic_read(backend(runtime), frame, timeout_ms);
#if H2_RUNTIME_AUDIO_LEVELS
    if (rc == H2_PAL_OK) {
        h2_runtime_private_t *state = runtime->private_state;
        publish_level(runtime, &state->audio_capture_level,
                      &state->audio_capture_level_ms, frame);
    }
#endif
    return rc;
}

#if H2_RUNTIME_AUDIO_LEVELS
/* Playback frames never pass through an audio vtable call, only through the
 * track the backend hands out, so the Runtime wraps that track to see them. */
typedef struct runtime_audio_track {
    h2_pal_audio_track_t track;
    h2_pal_audio_track_t *backend_track;
    h2_runtime_t *runtime;
} runtime_audio_track_t;

static runtime_audio_track_t *track_of(h2_pal_audio_track_t *track) {
    return track != NULL ? (runtime_audio_track_t *)track->user : NULL;
}

static int track_write(h2_pal_audio_track_t *track, const h2_audio_frame_t *frame,
                       uint32_t timeout_ms) {
    runtime_audio_track_t *wrapper = track_of(track);
    if (wrapper == NULL)
        return H2_AUDIO_ERR_INVALID_ARG;
    h2_runtime_private_t *state = wrapper->runtime->private_state;
    publish_level(wrapper->runtime, &state->audio_playback_level,
                  &state->audio_playback_level_ms, frame);
    return h2_pal_audio_track_write(wrapper->backend_track, frame, timeout_ms);
}

/* Always installed, even when the backend track has no close: the wrapper
 * must be released exactly once whatever the backend does, and forwarding
 * through the PAL helper still reports the backend NULL as INVALID_ARG. */
static int track_close(h2_pal_audio_track_t *track) {
    runtime_audio_track_t *wrapper = track_of(track);
    if (wrapper == NULL)
        return H2_AUDIO_ERR_INVALID_ARG;
    int rc = h2_pal_audio_track_close(wrapper->backend_track);
    h2_pal_mem_free(wrapper->runtime->mem, wrapper);
    return rc;
}

static int track_get_volume_factor(h2_pal_audio_track_t *track, uint32_t *out_factor_milli) {
    runtime_audio_track_t *wrapper = track_of(track);
    if (wrapper == NULL)
        return H2_AUDIO_ERR_INVALID_ARG;
    return h2_pal_audio_track_get_volume_factor(wrapper->backend_track, out_factor_milli);
}

static int track_set_volume_factor(h2_pal_audio_track_t *track, uint32_t factor_milli) {
    runtime_audio_track_t *wrapper = track_of(track);
    if (wrapper == NULL)
        return H2_AUDIO_ERR_INVALID_ARG;
    return h2_pal_audio_track_set_volume_factor(wrapper->backend_track, factor_milli);
}

static int track_drain(h2_pal_audio_track_t *track, uint32_t timeout_ms) {
    runtime_audio_track_t *wrapper = track_of(track);
    if (wrapper == NULL)
        return H2_AUDIO_ERR_INVALID_ARG;
    return h2_pal_audio_track_drain(wrapper->backend_track, timeout_ms);
}

#endif /* H2_RUNTIME_AUDIO_LEVELS */

static int create_track(void *user, const h2_audio_track_config_t *config,
                        h2_pal_audio_track_t **out) {
    h2_runtime_t *runtime = user;
#if !H2_RUNTIME_AUDIO_LEVELS
    /* Without measurement there is nothing to wrap: hand the backend track
     * straight through, exactly as this proxy did before levels existed. */
    return h2_pal_audio_create_track(backend(runtime), config, out);
#else
    h2_pal_audio_track_t *backend_track = NULL;
    int rc = h2_pal_audio_create_track(backend(runtime), config, &backend_track);
    if (rc != H2_PAL_OK)
        return rc;
    if (backend_track == NULL)
        return H2_AUDIO_ERR_INVALID_ARG;
    runtime_audio_track_t *wrapper =
        (runtime_audio_track_t *)h2_pal_mem_alloc(runtime->mem, sizeof(*wrapper));
    if (wrapper == NULL) {
        (void)h2_pal_audio_track_close(backend_track);
        return H2_PAL_ERR_NO_MEMORY;
    }
    wrapper->backend_track = backend_track;
    wrapper->runtime = runtime;
    /* An operation the backend track does not provide stays NULL here, so the
     * PAL helpers report it exactly as they would on the backend track. */
    wrapper->track = (h2_pal_audio_track_t){
        .user = wrapper,
        .audio = runtime->audio,
        .write = backend_track->write != NULL ? track_write : NULL,
        .close = track_close,
        .get_volume_factor =
            backend_track->get_volume_factor != NULL ? track_get_volume_factor : NULL,
        .set_volume_factor =
            backend_track->set_volume_factor != NULL ? track_set_volume_factor : NULL,
        .drain = backend_track->drain != NULL ? track_drain : NULL,
    };
    *out = &wrapper->track;
    return H2_PAL_OK;
#endif /* H2_RUNTIME_AUDIO_LEVELS */
}

static int get_volume(void *user, uint32_t *out) {
    h2_runtime_system_audio_state_t state;
    int rc = h2_runtime_system_state_audio(user, &state);
    if (rc == H2_PAL_OK)
        *out = state.muted ? 0 : state.volume_percent;
    return rc;
}
static int set_volume(void *user, uint32_t percent) {
    return h2_runtime_audio_set_volume(user, percent, 0);
}

#if H2_RUNTIME_AUDIO_LEVELS
static uint64_t widen_level_ms(const h2_runtime_t *runtime, unsigned int level,
                               unsigned int stored_ms) {
    if ((level & H2_RUNTIME_AUDIO_LEVEL_VALID) == 0u)
        return 0u;
    uint64_t now = 0u;
    if (h2_pal_time_get_monotonic_ms(runtime->time, &now) != H2_PAL_OK)
        return (uint64_t)stored_ms;
    uint64_t widened = (now & ~(uint64_t)UINT32_MAX) | (uint64_t)stored_ms;
    if (widened > now)
        widened -= (uint64_t)UINT32_MAX + 1u;
    return widened;
}

h2_pal_result_t h2_runtime_audio_get_levels(
    const h2_runtime_t *runtime, h2_runtime_audio_levels_t *out_levels) {
    if (!h2_runtime_ready(runtime) || !out_levels)
        return H2_PAL_ERR_INVALID_ARG;
    h2_runtime_private_t *state = runtime->private_state;
    unsigned int capture =
        atomic_load_explicit(&state->audio_capture_level, memory_order_relaxed);
    unsigned int capture_ms =
        atomic_load_explicit(&state->audio_capture_level_ms, memory_order_relaxed);
    unsigned int playback =
        atomic_load_explicit(&state->audio_playback_level, memory_order_relaxed);
    unsigned int playback_ms =
        atomic_load_explicit(&state->audio_playback_level_ms, memory_order_relaxed);
    out_levels->capture_percent = (uint8_t)(capture & 0xFFu);
    out_levels->playback_percent = (uint8_t)(playback & 0xFFu);
    out_levels->capture_updated_ms = widen_level_ms(runtime, capture, capture_ms);
    out_levels->playback_updated_ms = widen_level_ms(runtime, playback, playback_ms);
    return H2_PAL_OK;
}

#else
h2_pal_result_t h2_runtime_audio_get_levels(
    const h2_runtime_t *runtime, h2_runtime_audio_levels_t *out_levels) {
    if (!h2_runtime_ready(runtime) || !out_levels)
        return H2_PAL_ERR_INVALID_ARG;
    return H2_PAL_ERR_UNSUPPORTED;
}

#endif /* H2_RUNTIME_AUDIO_LEVELS */

void h2_runtime_audio_bind(h2_runtime_t *runtime) {
    static const h2_pal_audio_vtable_t vtable = {
        .get_info = get_info, .start_mic = start_mic, .stop_mic = stop_mic,
        .start_speaker = start_speaker, .stop_speaker = stop_speaker,
        .mic_read = mic_read, .create_track = create_track,
        .get_speaker_volume_percent = get_volume,
        .set_speaker_volume_percent = set_volume,
    };
    h2_runtime_private_t *state = runtime->private_state;
    state->audio_backend = state->audio_proxy;
    state->audio_state_busy = (atomic_flag)ATOMIC_FLAG_INIT;
#if H2_RUNTIME_AUDIO_LEVELS
    atomic_init(&state->audio_capture_level, 0u);
    atomic_init(&state->audio_capture_level_ms, 0u);
    atomic_init(&state->audio_playback_level, 0u);
    atomic_init(&state->audio_playback_level_ms, 0u);
#endif
    state->audio_proxy = (h2_pal_audio_api_t){runtime, &vtable};
}
