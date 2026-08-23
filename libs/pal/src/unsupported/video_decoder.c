#include "h2_pal.h"
#include <stddef.h>
#include <string.h>

static h2_pal_result_t unsupported_video_decoder_open(
    void *user,
    const h2_video_decoder_config_t *config,
    h2_pal_video_decoder_session_t **out_session) {
    (void)user;
    (void)config;
    if (out_session != NULL) {
        *out_session = NULL;
    }
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_video_decoder_configure(
    void *user,
    h2_pal_video_decoder_session_t *session,
    const h2_video_decoder_stream_config_t *config) {
    (void)user;
    (void)session;
    (void)config;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_video_decoder_submit_packet(
    void *user,
    h2_pal_video_decoder_session_t *session,
    const h2_video_decoder_packet_t *packet) {
    (void)user;
    (void)session;
    (void)packet;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_video_decoder_acquire_frame(
    void *user,
    h2_pal_video_decoder_session_t *session,
    uint32_t timeout_ms,
    h2_pal_video_decoder_frame_t **out_frame) {
    (void)user;
    (void)session;
    (void)timeout_ms;
    if (out_frame != NULL) {
        *out_frame = NULL;
    }
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_video_decoder_frame_get_info(
    void *user,
    h2_pal_video_decoder_session_t *session,
    h2_pal_video_decoder_frame_t *frame,
    h2_video_frame_info_t *out_info) {
    (void)user;
    (void)session;
    (void)frame;
    if (out_info != NULL) {
        *out_info = (h2_video_frame_info_t){0};
    }
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_video_decoder_release_frame(
    void *user,
    h2_pal_video_decoder_session_t *session,
    h2_pal_video_decoder_frame_t *frame) {
    (void)user;
    (void)session;
    (void)frame;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_video_decoder_reset(
    void *user,
    h2_pal_video_decoder_session_t *session) {
    (void)user;
    (void)session;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_video_decoder_close(
    void *user,
    h2_pal_video_decoder_session_t *session) {
    (void)user;
    (void)session;
    return H2_PAL_ERR_UNSUPPORTED;
}

static const h2_pal_video_decoder_vtable_t unsupported_video_decoder_vtable = {
    .open = unsupported_video_decoder_open,
    .configure = unsupported_video_decoder_configure,
    .submit_packet = unsupported_video_decoder_submit_packet,
    .acquire_frame = unsupported_video_decoder_acquire_frame,
    .frame_get_info = unsupported_video_decoder_frame_get_info,
    .release_frame = unsupported_video_decoder_release_frame,
    .reset = unsupported_video_decoder_reset,
    .close = unsupported_video_decoder_close,
};

static const h2_pal_video_decoder_api_t unsupported_video_decoder_api = {
    .user = NULL,
    .vtable = &unsupported_video_decoder_vtable,
};

const h2_pal_video_decoder_api_t *h2_pal_unsupported_video_decoder_api(void) {
    return &unsupported_video_decoder_api;
}
