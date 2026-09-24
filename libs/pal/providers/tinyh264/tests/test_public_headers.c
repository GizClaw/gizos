#include "h2_tinyh264.h"

#include <stdio.h>
#include <stdlib.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "check failed: %s:%d: %s\n", __FILE__, __LINE__, #condition); \
    exit(1); \
} } while (0)

static size_t s_allocations;
static void *tracked_alloc(void *user, size_t size) {
    (void)user;
    void *ptr = malloc(size);
    if (ptr != NULL) ++s_allocations;
    return ptr;
}
static void tracked_free(void *user, void *ptr) {
    (void)user;
    if (ptr != NULL) --s_allocations;
    free(ptr);
}
static const h2_pal_mem_vtable_t s_mem_vtable = {
    .alloc = tracked_alloc,
    .free = tracked_free,
};
static const h2_pal_mem_api_t s_mem = {.vtable = &s_mem_vtable};

static uint64_t fnv1a64(const void *data, size_t size) {
    const unsigned char *bytes = data;
    uint64_t hash = UINT64_C(1469598103934665603);
    for (size_t i = 0u; i < size; ++i) {
        hash ^= bytes[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static size_t next_start(const unsigned char *data, size_t size, size_t at) {
    for (size_t i = at; i + 4u < size; ++i) {
        if (data[i] == 0u && data[i + 1u] == 0u &&
            (data[i + 2u] == 1u ||
             (data[i + 2u] == 0u && data[i + 3u] == 1u))) {
            return i;
        }
    }
    return size;
}

static unsigned nal_type(const unsigned char *data, size_t size, size_t at) {
    if (at + 4u >= size) return 0u;
    const size_t prefix = data[at + 2u] == 1u ? 3u : 4u;
    return data[at + prefix] & 0x1fu;
}

int main(int argc, char **argv) {
    CHECK(argc == 1 || argc == 2);
    const char *fixture_path =
        argc == 2 ? argv[1] : "tests/fixtures/baseline_annexb.h264";
    FILE *file = fopen(fixture_path, "rb");
    CHECK(file != NULL);
    CHECK(fseek(file, 0, SEEK_END) == 0);
    const long file_size = ftell(file);
    CHECK(file_size > 0 && fseek(file, 0, SEEK_SET) == 0);
    unsigned char *bytes = malloc((size_t)file_size);
    CHECK(bytes != NULL);
    CHECK(fread(bytes, 1u, (size_t)file_size, file) == (size_t)file_size);
    fclose(file);

    size_t first_vcl = (size_t)file_size;
    size_t second_vcl = (size_t)file_size;
    for (size_t at = next_start(bytes, (size_t)file_size, 0u);
         at < (size_t)file_size;) {
        const unsigned type = nal_type(bytes, (size_t)file_size, at);
        if (type == 1u || type == 5u) {
            if (first_vcl == (size_t)file_size) first_vcl = at;
            else {
                second_vcl = at;
                break;
            }
        }
        at = next_start(bytes, (size_t)file_size, at + 3u);
    }
    CHECK(first_vcl > 0u && second_vcl > first_vcl);

    const h2_pal_video_decoder_api_t *api = h2_tinyh264_video_decoder_api();
    h2_pal_video_decoder_session_t *unsupported_session = NULL;
    const h2_video_decoder_config_t unsupported_open_config = {
        .frame_allocator = &s_mem,
        .preferred_format = H2_VIDEO_PIXEL_FORMAT_RGB888,
    };
    CHECK(h2_pal_video_decoder_open(
               api, &unsupported_open_config, &unsupported_session) ==
           H2_PAL_ERR_UNSUPPORTED);
    CHECK(unsupported_session == NULL);
    const h2_video_decoder_config_t open_config = {
        .frame_allocator = &s_mem,
        .preferred_format = H2_VIDEO_PIXEL_FORMAT_YUV420P,
    };
    h2_pal_video_decoder_session_t *session = NULL;
    h2_pal_video_decoder_session_t *session_two = NULL;
    h2_pal_video_decoder_session_t *rgb_session = NULL;
    CHECK(h2_pal_video_decoder_open(api, &open_config, &session) == H2_PAL_OK);
    CHECK(h2_pal_video_decoder_open(api, &open_config, &session_two) == H2_PAL_OK);
    const h2_video_decoder_config_t rgb_open_config = {
        .frame_allocator = &s_mem,
        .preferred_format = H2_VIDEO_PIXEL_FORMAT_RGB565,
    };
    CHECK(h2_pal_video_decoder_open(
               api, &rgb_open_config, &rgb_session) == H2_PAL_OK);
    const unsigned char high_profile_sps[] = {
        0u, 0u, 0u, 1u, 0x67u, 100u, 0u, 10u,
    };
    h2_video_decoder_stream_config_t unsupported_stream = {
        .codec = H2_VIDEO_CODEC_H264,
        .bitstream_format = H2_VIDEO_BITSTREAM_H264_ANNEX_B,
        .coded_width = 64u, .coded_height = 48u,
        .visible_width = 64u, .visible_height = 48u,
        .codec_config = high_profile_sps,
        .codec_config_size = sizeof(high_profile_sps),
    };
    CHECK(h2_pal_video_decoder_configure(
               api, rgb_session, &unsupported_stream) ==
           H2_PAL_ERR_UNSUPPORTED);
    const unsigned char avcc_packet[] = {
        0u, 0u, 0u, 4u, 0x67u, 66u, 0u, 10u,
    };
    unsupported_stream.codec_config = avcc_packet;
    unsupported_stream.codec_config_size = sizeof(avcc_packet);
    CHECK(h2_pal_video_decoder_configure(
               api, rgb_session, &unsupported_stream) ==
           H2_PAL_ERR_FORMAT);
    const h2_video_decoder_stream_config_t stream = {
        .codec = H2_VIDEO_CODEC_H264,
        .bitstream_format = H2_VIDEO_BITSTREAM_H264_ANNEX_B,
        .coded_width = 64u, .coded_height = 48u,
        .visible_width = 64u, .visible_height = 48u,
        .codec_config = bytes, .codec_config_size = first_vcl,
    };
    CHECK(h2_pal_video_decoder_configure(api, session, &stream) == H2_PAL_OK);
    CHECK(h2_pal_video_decoder_configure(api, session_two, &stream) == H2_PAL_OK);
    h2_video_decoder_stream_config_t cropped_stream = stream;
    cropped_stream.visible_width = 62u;
    CHECK(h2_pal_video_decoder_configure(
               api, rgb_session, &cropped_stream) ==
           H2_PAL_ERR_UNSUPPORTED);
    CHECK(h2_pal_video_decoder_configure(api, rgb_session, &stream) == H2_PAL_OK);
    const h2_video_decoder_packet_t packet = {
        .data = bytes + first_vcl,
        .size = second_vcl - first_vcl,
        .pts_us = 0,
        .duration_us = 200000,
    };
    const uint64_t packet_hash =
        fnv1a64(packet.data, packet.size);
    CHECK(h2_pal_video_decoder_submit_packet(api, session, &packet) == H2_PAL_OK);
    CHECK(fnv1a64(packet.data, packet.size) == packet_hash);
    CHECK(h2_pal_video_decoder_submit_packet(api, session, &packet) ==
           H2_PAL_ERR_WOULD_BLOCK);
    CHECK(h2_pal_video_decoder_submit_packet(api, session_two, &packet) == H2_PAL_OK);
    CHECK(h2_pal_video_decoder_submit_packet(api, rgb_session, &packet) == H2_PAL_OK);
    h2_pal_video_decoder_frame_t *frame = NULL;
    h2_pal_video_decoder_frame_t *frame_two = NULL;
    h2_pal_video_decoder_frame_t *rgb_frame = NULL;
    CHECK(h2_pal_video_decoder_acquire_frame(api, session, 0u, &frame) == H2_PAL_OK);
    h2_pal_video_decoder_frame_t *duplicate = NULL;
    CHECK(h2_pal_video_decoder_acquire_frame(
               api, session, 0u, &duplicate) == H2_PAL_ERR_WOULD_BLOCK);
    CHECK(duplicate == NULL);
    CHECK(h2_pal_video_decoder_acquire_frame(
               api, session_two, 0u, &frame_two) == H2_PAL_OK);
    CHECK(h2_pal_video_decoder_acquire_frame(
               api, rgb_session, 0u, &rgb_frame) == H2_PAL_OK);
    CHECK(frame != frame_two);
    CHECK(h2_pal_video_decoder_release_frame(api, session, frame_two) ==
           H2_PAL_ERR_INVALID_STATE);
    CHECK(h2_pal_video_decoder_reset(api, session) ==
           H2_PAL_ERR_INVALID_STATE);
    CHECK(h2_pal_video_decoder_close(api, session) ==
           H2_PAL_ERR_INVALID_STATE);
    h2_video_frame_info_t info = {0};
    CHECK(h2_pal_video_decoder_frame_get_info(api, session, frame, &info) == H2_PAL_OK);
    CHECK(info.width == 64u && info.height == 48u);
    CHECK(info.format == H2_VIDEO_PIXEL_FORMAT_YUV420P && info.plane_count == 3u);
    h2_video_frame_info_t rgb_info = {0};
    CHECK(h2_pal_video_decoder_frame_get_info(
               api, rgb_session, rgb_frame, &rgb_info) == H2_PAL_OK);
    CHECK(rgb_info.width == 64u && rgb_info.height == 48u);
    CHECK(rgb_info.format == H2_VIDEO_PIXEL_FORMAT_RGB565);
    CHECK(rgb_info.plane_count == 1u);
    CHECK(rgb_info.planes[0].bytes == 64u * 48u * sizeof(uint16_t));
    CHECK(fnv1a64(rgb_info.planes[0].data, rgb_info.planes[0].bytes) ==
           UINT64_C(11472510323931125994));
    CHECK(h2_pal_video_decoder_release_frame(api, session, frame) == H2_PAL_OK);
    CHECK(h2_pal_video_decoder_release_frame(api, session, frame) ==
           H2_PAL_ERR_INVALID_STATE);
    CHECK(h2_pal_video_decoder_release_frame(
               api, session_two, frame_two) == H2_PAL_OK);
    CHECK(h2_pal_video_decoder_release_frame(
               api, rgb_session, rgb_frame) == H2_PAL_OK);
    const h2_video_decoder_packet_t eos = {
        .flags = H2_VIDEO_DECODER_PACKET_END_OF_STREAM,
    };
    CHECK(h2_pal_video_decoder_submit_packet(api, session, &eos) == H2_PAL_OK);
    CHECK(h2_pal_video_decoder_submit_packet(api, session, &packet) ==
           H2_PAL_ERR_INVALID_STATE);
    CHECK(h2_pal_video_decoder_acquire_frame(
               api, session, 0u, &duplicate) == H2_PAL_EXIT);
    CHECK(h2_pal_video_decoder_reset(api, session) == H2_PAL_OK);
    CHECK(h2_pal_video_decoder_configure(api, session, &stream) == H2_PAL_OK);
    CHECK(h2_pal_video_decoder_close(api, session) == H2_PAL_OK);
    CHECK(h2_pal_video_decoder_close(api, session_two) == H2_PAL_OK);
    CHECK(h2_pal_video_decoder_close(api, rgb_session) == H2_PAL_OK);
    free(bytes);
    CHECK(s_allocations == 0u);
    return 0;
}
