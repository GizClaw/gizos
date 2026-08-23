#include "h2_linux_fdk_aac_decoder.h"

#include "h2_fdk_aac_test_fixture.h"

#include <stdio.h>
#include <stdlib.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "check failed: %s:%d: %s\n", \
                __FILE__, __LINE__, #condition); \
        exit(EXIT_FAILURE); \
    } \
} while (0)

static void *test_alloc(void *user, size_t size) {
    (void)user;
    return malloc(size);
}

static void test_free(void *user, void *memory) {
    (void)user;
    free(memory);
}

int main(void) {
    const h2_pal_mem_vtable_t mem_vtable = {
        .alloc = test_alloc,
        .free = test_free,
    };
    const h2_pal_mem_api_t allocator = {
        .user = NULL,
        .vtable = &mem_vtable,
    };
    const h2_audio_decoder_config_t open_config = {
        .pcm_allocator = &allocator,
        .preferred_format = H2_AUDIO_SAMPLE_S16LE,
    };
    const h2_pal_audio_decoder_api_t *api =
        h2_linux_fdk_aac_decoder_api();
    h2_pal_audio_decoder_session_t *session = NULL;
    CHECK(h2_pal_audio_decoder_open(api, &open_config, &session) == H2_PAL_OK);
    const uint8_t asc[] = {0x14u, 0x08u, 0x56u, 0xe5u, 0x00u};
    const h2_audio_decoder_stream_config_t stream = {
        .codec = H2_AUDIO_CODEC_AAC_LC,
        .bitstream_format = H2_AUDIO_BITSTREAM_AAC_RAW,
        .sample_rate_hz = 16000u,
        .channels = 1u,
        .codec_config = asc,
        .codec_config_size = sizeof(asc),
    };
    CHECK(h2_pal_audio_decoder_configure(api, session, &stream) == H2_PAL_OK);
    const h2_audio_decoder_packet_t packet = {
        .data = h2_fdk_aac_test_access_unit,
        .size = sizeof(h2_fdk_aac_test_access_unit),
        .pts_us = 0,
        .duration_us = 64000,
    };
    CHECK(h2_pal_audio_decoder_submit_packet(api, session, &packet) == H2_PAL_OK);
    h2_pal_audio_decoder_frame_t *frame = NULL;
    CHECK(h2_pal_audio_decoder_acquire_frame(
              api, session, 0u, &frame) == H2_PAL_OK);
    CHECK(frame != NULL);
    h2_audio_decoder_frame_info_t info = {0};
    CHECK(h2_pal_audio_decoder_frame_get_info(
              api, session, frame, &info) == H2_PAL_OK);
    CHECK(info.data != NULL);
    CHECK(info.bytes == 2048u);
    CHECK(info.sample_rate_hz == 16000u);
    CHECK(info.samples_per_channel == 1024u);
    CHECK(info.channels == 1u);
    CHECK(info.sample_format == H2_AUDIO_SAMPLE_S16LE);
    CHECK(info.pts_us == 0);
    CHECK(info.duration_us == 64000);
    CHECK(h2_pal_audio_decoder_release_frame(api, session, frame) == H2_PAL_OK);
    const h2_audio_decoder_packet_t eos = {
        .flags = H2_AUDIO_DECODER_PACKET_END_OF_STREAM,
    };
    CHECK(h2_pal_audio_decoder_submit_packet(api, session, &eos) == H2_PAL_OK);
    frame = NULL;
    CHECK(h2_pal_audio_decoder_acquire_frame(
              api, session, 0u, &frame) == H2_PAL_EXIT);
    CHECK(h2_pal_audio_decoder_reset(api, session) == H2_PAL_OK);
    CHECK(h2_pal_audio_decoder_close(api, session) == H2_PAL_OK);
    return 0;
}
