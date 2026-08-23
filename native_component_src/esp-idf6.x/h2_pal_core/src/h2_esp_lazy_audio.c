#include "h2_esp_lazy_audio.h"

#include <string.h>

static h2_pal_audio_t *resolve_audio(void *user) {
    h2_esp_lazy_audio_t *lazy_audio = (h2_esp_lazy_audio_t *)user;
    return lazy_audio->resolve(lazy_audio->user);
}

static int lazy_get_info(void *user, h2_audio_info_t *info) {
    h2_pal_audio_t *resolved = resolve_audio(user);
    return resolved == NULL ? H2_AUDIO_ERR_UNAVAILABLE : h2_pal_audio_get_info(resolved, info);
}

static int lazy_start_mic(void *user) {
    h2_pal_audio_t *resolved = resolve_audio(user);
    return resolved == NULL ? H2_AUDIO_ERR_UNAVAILABLE : h2_pal_audio_start_mic(resolved);
}

static int lazy_stop_mic(void *user) {
    h2_pal_audio_t *resolved = resolve_audio(user);
    return resolved == NULL ? H2_AUDIO_ERR_UNAVAILABLE : h2_pal_audio_stop_mic(resolved);
}

static int lazy_start_speaker(void *user) {
    h2_pal_audio_t *resolved = resolve_audio(user);
    return resolved == NULL ? H2_AUDIO_ERR_UNAVAILABLE : h2_pal_audio_start_speaker(resolved);
}

static int lazy_stop_speaker(void *user) {
    h2_pal_audio_t *resolved = resolve_audio(user);
    return resolved == NULL ? H2_AUDIO_ERR_UNAVAILABLE : h2_pal_audio_stop_speaker(resolved);
}

static int lazy_mic_read(
    void *user,
    h2_audio_frame_t *out_frame,
    uint32_t timeout_ms) {
    h2_pal_audio_t *resolved = resolve_audio(user);
    return resolved == NULL ? H2_AUDIO_ERR_UNAVAILABLE :
                              h2_pal_audio_mic_read(resolved, out_frame, timeout_ms);
}

static int lazy_create_track(
    void *user,
    const h2_audio_track_config_t *config,
    h2_pal_audio_track_t **out_track) {
    h2_pal_audio_t *resolved = resolve_audio(user);
    return resolved == NULL ? H2_AUDIO_ERR_UNAVAILABLE :
                              h2_pal_audio_create_track(resolved, config, out_track);
}

static int lazy_get_speaker_volume_percent(
    void *user,
    uint32_t *out_percent) {
    h2_pal_audio_t *resolved = resolve_audio(user);
    return resolved == NULL ? H2_AUDIO_ERR_UNAVAILABLE :
                              h2_pal_audio_get_speaker_volume_percent(resolved, out_percent);
}

static int lazy_set_speaker_volume_percent(
    void *user,
    uint32_t percent) {
    h2_pal_audio_t *resolved = resolve_audio(user);
    return resolved == NULL ? H2_AUDIO_ERR_UNAVAILABLE :
                              h2_pal_audio_set_speaker_volume_percent(resolved, percent);
}

int h2_esp_lazy_audio_init(
    h2_esp_lazy_audio_t *lazy_audio,
    void *user,
    h2_esp_lazy_audio_resolve_fn resolve) {
    if (lazy_audio == NULL || resolve == NULL) {
        return H2_AUDIO_ERR_INVALID_ARG;
    }
    memset(lazy_audio, 0, sizeof(*lazy_audio));
    lazy_audio->user = user;
    lazy_audio->resolve = resolve;
    static const h2_pal_audio_vtable_t vtable = {
        .get_info = lazy_get_info,
        .start_mic = lazy_start_mic,
        .stop_mic = lazy_stop_mic,
        .start_speaker = lazy_start_speaker,
        .stop_speaker = lazy_stop_speaker,
        .mic_read = lazy_mic_read,
        .create_track = lazy_create_track,
        .get_speaker_volume_percent = lazy_get_speaker_volume_percent,
        .set_speaker_volume_percent = lazy_set_speaker_volume_percent,
    };
    lazy_audio->api = (h2_pal_audio_t){
        .user = lazy_audio,
        .vtable = &vtable,
    };
    return H2_AUDIO_OK;
}

h2_pal_audio_t *h2_esp_lazy_audio_api(h2_esp_lazy_audio_t *lazy_audio) {
    return lazy_audio == NULL || lazy_audio->resolve == NULL ? NULL : &lazy_audio->api;
}
