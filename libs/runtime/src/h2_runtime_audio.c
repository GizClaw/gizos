#include "h2_runtime_internal.h"

static const h2_pal_audio_api_t *backend(h2_runtime_t *runtime) {
    return &runtime->private_state->audio_backend;
}

h2_pal_result_t h2_runtime_system_state_audio(
    const h2_runtime_t *runtime, h2_runtime_system_audio_state_t *out_state) {
    if (!h2_runtime_ready(runtime) || !out_state)
        return H2_PAL_ERR_INVALID_ARG;
    h2_runtime_private_t *state = runtime->private_state;
    if (atomic_exchange(&state->audio_state_busy, true))
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
    atomic_store(&state->audio_state_busy, false);
    return rc;
}

h2_pal_result_t h2_runtime_audio_set_volume(
    h2_runtime_t *runtime, uint32_t percent, uint8_t muted) {
    if (!h2_runtime_ready(runtime) || percent > 100u || muted > 1u)
        return H2_PAL_ERR_INVALID_ARG;
    h2_runtime_private_t *state = runtime->private_state;
    if (atomic_exchange(&state->audio_state_busy, true))
        return H2_PAL_ERR_BUSY;
    int rc = h2_pal_audio_set_speaker_volume_percent(backend(runtime), muted ? 0 : percent);
    if (rc == H2_PAL_OK) {
        state->audio_state.volume_percent = percent;
        state->audio_state.muted = muted;
        state->audio_state_valid = true;
    }
    atomic_store(&state->audio_state_busy, false);
    return rc;
}

static int get_info(void *user, h2_audio_info_t *info) {
    return h2_pal_audio_get_info(backend(user), info);
}
static int start_mic(void *user) { return h2_pal_audio_start_mic(backend(user)); }
static int stop_mic(void *user) { return h2_pal_audio_stop_mic(backend(user)); }
static int start_speaker(void *user) { return h2_pal_audio_start_speaker(backend(user)); }
static int stop_speaker(void *user) { return h2_pal_audio_stop_speaker(backend(user)); }
static int mic_read(void *user, h2_audio_frame_t *frame, uint32_t timeout_ms) {
    return h2_pal_audio_mic_read(backend(user), frame, timeout_ms);
}
static int create_track(void *user, const h2_audio_track_config_t *config,
                        h2_pal_audio_track_t **out) {
    return h2_pal_audio_create_track(backend(user), config, out);
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
    atomic_init(&state->audio_state_busy, false);
    state->audio_proxy = (h2_pal_audio_api_t){runtime, &vtable};
}
