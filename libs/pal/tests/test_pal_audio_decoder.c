#include "h2/pal/hal/h2_pal_audio_decoder.h"

#include <assert.h>
#include <stdlib.h>

struct h2_pal_audio_decoder_session {
    int configured;
    int frame_ready;
    int acquired;
};

struct h2_pal_audio_decoder_frame {
    h2_pal_audio_decoder_session_t *owner;
};

typedef struct fixture {
    h2_pal_audio_decoder_session_t session;
    h2_pal_audio_decoder_frame_t frame;
    int16_t samples[4];
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
static const h2_pal_mem_api_t s_mem = {.user = NULL, .vtable = &s_mem_vtable};

static h2_pal_result_t open_decoder(
    void *user,
    const h2_audio_decoder_config_t *config,
    h2_pal_audio_decoder_session_t **out_session) {
    fixture_t *fixture = user;
    assert(config->preferred_format == H2_AUDIO_SAMPLE_S16LE);
    fixture->session = (h2_pal_audio_decoder_session_t){0};
    fixture->frame.owner = &fixture->session;
    *out_session = &fixture->session;
    return H2_PAL_OK;
}

static h2_pal_result_t configure_decoder(
    void *user,
    h2_pal_audio_decoder_session_t *session,
    const h2_audio_decoder_stream_config_t *config) {
    (void)user;
    assert(config->sample_rate_hz == 48000u && config->channels == 1u);
    session->configured = 1;
    return H2_PAL_OK;
}

static h2_pal_result_t submit_packet(
    void *user,
    h2_pal_audio_decoder_session_t *session,
    const h2_audio_decoder_packet_t *packet) {
    (void)user;
    if (!session->configured) return H2_PAL_ERR_INVALID_STATE;
    if (session->frame_ready || session->acquired) return H2_PAL_ERR_WOULD_BLOCK;
    assert(packet->size == 2u);
    session->frame_ready = 1;
    return H2_PAL_OK;
}

static h2_pal_result_t acquire_frame(
    void *user,
    h2_pal_audio_decoder_session_t *session,
    uint32_t timeout_ms,
    h2_pal_audio_decoder_frame_t **out_frame) {
    fixture_t *fixture = user;
    (void)timeout_ms;
    if (!session->frame_ready || session->acquired) {
        return H2_PAL_ERR_WOULD_BLOCK;
    }
    session->acquired = 1;
    *out_frame = &fixture->frame;
    return H2_PAL_OK;
}

static h2_pal_result_t frame_info(
    void *user,
    h2_pal_audio_decoder_session_t *session,
    h2_pal_audio_decoder_frame_t *frame,
    h2_audio_decoder_frame_info_t *out_info) {
    fixture_t *fixture = user;
    if (!session->acquired || frame->owner != session) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    *out_info = (h2_audio_decoder_frame_info_t){
        .data = fixture->samples,
        .bytes = sizeof(fixture->samples),
        .sample_rate_hz = 48000u,
        .samples_per_channel = 4u,
        .channels = 1u,
        .sample_format = H2_AUDIO_SAMPLE_S16LE,
        .pts_us = 0,
        .duration_us = 83,
    };
    return H2_PAL_OK;
}

static h2_pal_result_t release_frame(
    void *user,
    h2_pal_audio_decoder_session_t *session,
    h2_pal_audio_decoder_frame_t *frame) {
    fixture_t *fixture = user;
    if (!session->acquired || frame != &fixture->frame) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    session->acquired = 0;
    session->frame_ready = 0;
    return H2_PAL_OK;
}

static h2_pal_result_t reset_decoder(
    void *user,
    h2_pal_audio_decoder_session_t *session) {
    (void)user;
    if (session->acquired) return H2_PAL_ERR_INVALID_STATE;
    *session = (h2_pal_audio_decoder_session_t){0};
    return H2_PAL_OK;
}

static h2_pal_result_t close_decoder(
    void *user,
    h2_pal_audio_decoder_session_t *session) {
    (void)user;
    return session->acquired ? H2_PAL_ERR_INVALID_STATE : H2_PAL_OK;
}

int main(void) {
    fixture_t fixture = {0};
    static const h2_pal_audio_decoder_vtable_t vtable = {
        .open = open_decoder,
        .configure = configure_decoder,
        .submit_packet = submit_packet,
        .acquire_frame = acquire_frame,
        .frame_get_info = frame_info,
        .release_frame = release_frame,
        .reset = reset_decoder,
        .close = close_decoder,
    };
    const h2_pal_audio_decoder_api_t api = {.user = &fixture, .vtable = &vtable};
    const h2_audio_decoder_config_t config = {
        .pcm_allocator = &s_mem,
        .preferred_format = H2_AUDIO_SAMPLE_S16LE,
    };
    h2_pal_audio_decoder_session_t *session = NULL;
    assert(h2_pal_audio_decoder_open(&api, &config, &session) == H2_PAL_OK);
    const uint8_t asc[2] = {0x11, 0x88};
    const h2_audio_decoder_stream_config_t stream = {
        .codec = H2_AUDIO_CODEC_AAC_LC,
        .bitstream_format = H2_AUDIO_BITSTREAM_AAC_RAW,
        .sample_rate_hz = 48000u,
        .channels = 1u,
        .codec_config = asc,
        .codec_config_size = sizeof(asc),
    };
    assert(h2_pal_audio_decoder_configure(&api, session, &stream) == H2_PAL_OK);
    const h2_audio_decoder_packet_t packet = {
        .data = asc,
        .size = sizeof(asc),
        .duration_us = 21333,
    };
    assert(h2_pal_audio_decoder_submit_packet(&api, session, &packet) == H2_PAL_OK);
    h2_pal_audio_decoder_frame_t *frame = NULL;
    assert(h2_pal_audio_decoder_acquire_frame(&api, session, 0u, &frame) ==
        H2_PAL_OK);
    h2_audio_decoder_frame_info_t info;
    assert(h2_pal_audio_decoder_frame_get_info(&api, session, frame, &info) ==
        H2_PAL_OK);
    assert(info.data == fixture.samples && info.samples_per_channel == 4u);
    assert(h2_pal_audio_decoder_release_frame(&api, session, frame) == H2_PAL_OK);
    assert(h2_pal_audio_decoder_reset(&api, session) == H2_PAL_OK);
    assert(h2_pal_audio_decoder_close(&api, session) == H2_PAL_OK);
    return 0;
}
