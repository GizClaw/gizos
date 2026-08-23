#include "h2/pal/hal/h2_pal_video_decoder.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

struct h2_pal_video_decoder_session {
    int configured;
    int submitted;
    int acquired;
};

struct h2_pal_video_decoder_frame {
    h2_pal_video_decoder_session_t *owner;
};

typedef struct fixture {
    h2_pal_video_decoder_session_t session;
    h2_pal_video_decoder_frame_t frame;
    uint16_t pixels[4];
} fixture_t;

static void *test_alloc(void *user, size_t size) {
    (void)user;
    return malloc(size);
}

static void test_free(void *user, void *pointer) {
    (void)user;
    free(pointer);
}

static const h2_pal_mem_vtable_t s_mem_vtable = {
    .alloc = test_alloc,
    .realloc = NULL,
    .free = test_free,
};

static const h2_pal_mem_api_t s_mem = {
    .user = NULL,
    .vtable = &s_mem_vtable,
};

static h2_pal_result_t fixture_open(
    void *user,
    const h2_video_decoder_config_t *config,
    h2_pal_video_decoder_session_t **out_session) {
    fixture_t *fixture = user;
    assert(config->frame_allocator->vtable->alloc != NULL);
    fixture->session = (h2_pal_video_decoder_session_t){0};
    fixture->frame.owner = &fixture->session;
    *out_session = &fixture->session;
    return H2_PAL_OK;
}

static h2_pal_result_t fixture_configure(
    void *user,
    h2_pal_video_decoder_session_t *session,
    const h2_video_decoder_stream_config_t *config) {
    (void)user;
    assert(config->codec_config_size == 8u);
    if (session->acquired) return H2_PAL_ERR_INVALID_STATE;
    session->configured = 1;
    return H2_PAL_OK;
}

static h2_pal_result_t fixture_submit(
    void *user,
    h2_pal_video_decoder_session_t *session,
    const h2_video_decoder_packet_t *packet) {
    (void)user;
    if (!session->configured) return H2_PAL_ERR_INVALID_STATE;
    if (session->submitted || session->acquired) return H2_PAL_ERR_WOULD_BLOCK;
    assert(packet->size == 5u);
    session->submitted = 1;
    return H2_PAL_OK;
}

static h2_pal_result_t fixture_acquire(
    void *user,
    h2_pal_video_decoder_session_t *session,
    uint32_t timeout_ms,
    h2_pal_video_decoder_frame_t **out_frame) {
    fixture_t *fixture = user;
    (void)timeout_ms;
    if (!session->submitted) return H2_PAL_ERR_WOULD_BLOCK;
    if (session->acquired) return H2_PAL_ERR_WOULD_BLOCK;
    session->acquired = 1;
    *out_frame = &fixture->frame;
    return H2_PAL_OK;
}

static h2_pal_result_t fixture_info(
    void *user,
    h2_pal_video_decoder_session_t *session,
    h2_pal_video_decoder_frame_t *frame,
    h2_video_frame_info_t *out_info) {
    fixture_t *fixture = user;
    if (!session->acquired || frame != &fixture->frame || frame->owner != session) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    *out_info = (h2_video_frame_info_t){
        .format = H2_VIDEO_PIXEL_FORMAT_RGB565,
        .width = 2u,
        .height = 2u,
        .planes = {{
            .data = fixture->pixels,
            .bytes = sizeof(fixture->pixels),
            .stride_bytes = 4u,
        }},
        .plane_count = 1u,
        .pts_us = 1000,
        .duration_us = 33333,
    };
    return H2_PAL_OK;
}

static h2_pal_result_t fixture_release(
    void *user,
    h2_pal_video_decoder_session_t *session,
    h2_pal_video_decoder_frame_t *frame) {
    fixture_t *fixture = user;
    if (!session->acquired || frame != &fixture->frame) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    session->acquired = 0;
    session->submitted = 0;
    return H2_PAL_OK;
}

static h2_pal_result_t fixture_reset(
    void *user,
    h2_pal_video_decoder_session_t *session) {
    (void)user;
    if (session->acquired) return H2_PAL_ERR_INVALID_STATE;
    *session = (h2_pal_video_decoder_session_t){0};
    return H2_PAL_OK;
}

static h2_pal_result_t fixture_close(
    void *user,
    h2_pal_video_decoder_session_t *session) {
    (void)user;
    return session->acquired ? H2_PAL_ERR_INVALID_STATE : H2_PAL_OK;
}

int main(void) {
    fixture_t fixture = {0};
    static const h2_pal_video_decoder_vtable_t vtable = {
        .open = fixture_open,
        .configure = fixture_configure,
        .submit_packet = fixture_submit,
        .acquire_frame = fixture_acquire,
        .frame_get_info = fixture_info,
        .release_frame = fixture_release,
        .reset = fixture_reset,
        .close = fixture_close,
    };
    const h2_pal_video_decoder_api_t api = {
        .user = &fixture,
        .vtable = &vtable,
    };
    const h2_video_decoder_config_t config = {
        .frame_allocator = &s_mem,
        .preferred_format = H2_VIDEO_PIXEL_FORMAT_RGB565,
    };
    h2_pal_video_decoder_session_t *session = (void *)1;
    assert(h2_pal_video_decoder_open(&api, &config, &session) == H2_PAL_OK);

    const uint8_t codec_config[8] = {0, 0, 0, 1, 0x67, 0, 0, 1};
    const h2_video_decoder_stream_config_t stream = {
        .codec = H2_VIDEO_CODEC_H264,
        .bitstream_format = H2_VIDEO_BITSTREAM_H264_ANNEX_B,
        .coded_width = 2u,
        .coded_height = 2u,
        .visible_width = 2u,
        .visible_height = 2u,
        .codec_config = codec_config,
        .codec_config_size = sizeof(codec_config),
    };
    assert(h2_pal_video_decoder_configure(&api, session, &stream) == H2_PAL_OK);

    const uint8_t access_unit[5] = {0, 0, 0, 1, 0x65};
    const h2_video_decoder_packet_t packet = {
        .data = access_unit,
        .size = sizeof(access_unit),
        .pts_us = 1000,
        .dts_us = 1000,
        .duration_us = 33333,
    };
    assert(h2_pal_video_decoder_submit_packet(&api, session, &packet) == H2_PAL_OK);
    assert(h2_pal_video_decoder_submit_packet(&api, session, &packet) ==
        H2_PAL_ERR_WOULD_BLOCK);

    h2_pal_video_decoder_frame_t *frame = NULL;
    assert(h2_pal_video_decoder_acquire_frame(&api, session, 0u, &frame) ==
        H2_PAL_OK);
    h2_video_frame_info_t info = {0};
    assert(h2_pal_video_decoder_frame_get_info(&api, session, frame, &info) ==
        H2_PAL_OK);
    assert(info.width == 2u && info.planes[0].data == fixture.pixels);
    assert(h2_pal_video_decoder_reset(&api, session) ==
        H2_PAL_ERR_INVALID_STATE);
    assert(h2_pal_video_decoder_release_frame(&api, session, frame) ==
        H2_PAL_OK);
    assert(h2_pal_video_decoder_release_frame(&api, session, frame) ==
        H2_PAL_ERR_INVALID_STATE);
    assert(h2_pal_video_decoder_reset(&api, session) == H2_PAL_OK);
    assert(h2_pal_video_decoder_close(&api, session) == H2_PAL_OK);

    session = (void *)1;
    assert(h2_pal_video_decoder_open(NULL, &config, &session) ==
        H2_PAL_ERR_INVALID_ARG);
    assert(session == NULL);
    const h2_video_decoder_config_t no_allocator = {0};
    assert(h2_pal_video_decoder_open(&api, &no_allocator, &session) ==
        H2_PAL_ERR_INVALID_ARG);
    return 0;
}
