#include "h2_esp_board_private.h"

#include "h2_esp_board_config.h"

#include "esp_log.h"

#include <string.h>

static const char *TAG = "h2_esp_audio";

static int esp_audio_none_get_info(void *user, h2_audio_info_t *info) {
    (void)user;
    memset(info, 0, sizeof(*info));
    info->available = 0;
    return H2_AUDIO_OK;
}

static int esp_audio_none_read(
    void *user,
    h2_audio_frame_t *out_frame,
    uint32_t timeout_ms) {
    (void)user;
    (void)out_frame;
    (void)timeout_ms;
    return H2_AUDIO_ERR_UNAVAILABLE;
}

static int esp_audio_none_lifecycle(void *user) {
    (void)user;
    return H2_AUDIO_ERR_UNAVAILABLE;
}

static int esp_audio_none_create_track(
    void *user,
    const h2_audio_track_config_t *config,
    h2_pal_audio_track_t **out_track) {
    (void)user;
    (void)config;
    (void)out_track;
    return H2_AUDIO_ERR_UNAVAILABLE;
}

static int esp_audio_none_get_volume(void *user, uint32_t *out_percent) {
    (void)user;
    (void)out_percent;
    return H2_AUDIO_ERR_UNAVAILABLE;
}

static int esp_audio_none_set_volume(void *user, uint32_t percent) {
    (void)user;
    (void)percent;
    return H2_AUDIO_ERR_UNAVAILABLE;
}

h2_pal_audio_t *h2_esp_board_audio(void) {
    static int warned;
    static const h2_pal_audio_vtable_t vtable = {
        .get_info = esp_audio_none_get_info,
        .start_mic = esp_audio_none_lifecycle,
        .stop_mic = esp_audio_none_lifecycle,
        .start_speaker = esp_audio_none_lifecycle,
        .stop_speaker = esp_audio_none_lifecycle,
        .mic_read = esp_audio_none_read,
        .create_track = esp_audio_none_create_track,
        .get_speaker_volume_percent = esp_audio_none_get_volume,
        .set_speaker_volume_percent = esp_audio_none_set_volume,
    };
    static h2_pal_audio_t audio = {
        .user = NULL,
        .vtable = &vtable,
    };
    if (!warned) {
        ESP_LOGW(TAG, "board %s has no audio backend", H2_ESP_BOARD_NAME);
        warned = 1;
    }
    return &audio;
}

h2_pal_audio_t *h2_esp_board_audio_if_initialized(void) {
    return NULL;
}
