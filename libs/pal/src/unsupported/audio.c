#include "h2_pal.h"
#include <stddef.h>
#include <string.h>

static int unsupported_audio_get_info(void *p0, h2_audio_info_t *p1) {
    (void)p0;
    (void)p1;
    return H2_PAL_ERR_UNSUPPORTED;
}

static int unsupported_audio_start_mic(void *p0) {
    (void)p0;
    return H2_PAL_ERR_UNSUPPORTED;
}

static int unsupported_audio_stop_mic(void *p0) {
    (void)p0;
    return H2_PAL_ERR_UNSUPPORTED;
}

static int unsupported_audio_start_speaker(void *p0) {
    (void)p0;
    return H2_PAL_ERR_UNSUPPORTED;
}

static int unsupported_audio_stop_speaker(void *p0) {
    (void)p0;
    return H2_PAL_ERR_UNSUPPORTED;
}

static int unsupported_audio_mic_read(void *p0, h2_audio_frame_t *p1, uint32_t p2) {
    (void)p0;
    (void)p1;
    (void)p2;
    return H2_PAL_ERR_UNSUPPORTED;
}

static int unsupported_audio_create_track(void *p0, const h2_audio_track_config_t *p1, h2_pal_audio_track_t **p2) {
    (void)p0;
    (void)p1;
    (void)p2;
    return H2_PAL_ERR_UNSUPPORTED;
}

static int unsupported_audio_get_speaker_volume_percent(void *p0, uint32_t *p1) {
    (void)p0;
    (void)p1;
    return H2_PAL_ERR_UNSUPPORTED;
}

static int unsupported_audio_set_speaker_volume_percent(void *p0, uint32_t p1) {
    (void)p0;
    (void)p1;
    return H2_PAL_ERR_UNSUPPORTED;
}

static const h2_pal_audio_vtable_t unsupported_audio_vtable = {
    .get_info = unsupported_audio_get_info,
    .start_mic = unsupported_audio_start_mic,
    .stop_mic = unsupported_audio_stop_mic,
    .start_speaker = unsupported_audio_start_speaker,
    .stop_speaker = unsupported_audio_stop_speaker,
    .mic_read = unsupported_audio_mic_read,
    .create_track = unsupported_audio_create_track,
    .get_speaker_volume_percent = unsupported_audio_get_speaker_volume_percent,
    .set_speaker_volume_percent = unsupported_audio_set_speaker_volume_percent,
};
static const h2_pal_audio_api_t unsupported_audio_api = { .user = NULL, .vtable = &unsupported_audio_vtable };
const h2_pal_audio_api_t *h2_pal_unsupported_audio_api(void) { return &unsupported_audio_api; }
