#ifndef H2_MP4_DECODER_H
#define H2_MP4_DECODER_H

#include "h2/pal/hal/h2_pal_audio_decoder.h"
#include "h2/pal/hal/h2_pal_video_decoder.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define H2_MP4_DECODER_DEFAULT_MAX_FILE_BYTES (64u * 1024u * 1024u)
#define H2_MP4_DECODER_DEFAULT_MAX_SAMPLES 200000u
#define H2_MP4_DECODER_DEFAULT_MAX_PCM_BYTES (16u * 1024u * 1024u)
#define H2_MP4_DECODER_DEFAULT_MAX_PRESENTATION_BYTES (16u * 1024u * 1024u)

typedef struct h2_mp4_decoder_source_api {
    void *user;
    uint64_t size;
    h2_pal_result_t (*read_at)(
        void *user,
        uint64_t offset,
        void *buffer,
        size_t capacity,
        size_t *out_read);
} h2_mp4_decoder_source_api_t;

typedef struct h2_mp4_decoder_config {
    const h2_pal_mem_api_t *allocator;
    h2_mp4_decoder_source_api_t source;
    h2_pal_video_decoder_api_t video_decoder;
    h2_pal_audio_decoder_api_t audio_decoder;
    h2_video_pixel_format_t video_format;
    int require_video;
    int require_audio;
    size_t max_file_bytes;
    size_t max_samples;
    size_t max_pcm_bytes;
    size_t max_presentation_bytes;
} h2_mp4_decoder_config_t;

typedef struct h2_mp4_decoder_info {
    int has_video;
    int has_audio;
    uint32_t width;
    uint32_t height;
    uint32_t audio_sample_rate_hz;
    uint8_t audio_channels;
    int64_t duration_us;
} h2_mp4_decoder_info_t;

typedef struct h2_mp4_decoder_video_plane {
    void *data;
    size_t bytes;
    size_t stride_bytes;
} h2_mp4_decoder_video_plane_t;

typedef struct h2_mp4_decoder_frame_info {
    int64_t pts_us;
    int64_t duration_us;
    h2_video_pixel_format_t video_format;
    uint32_t width;
    uint32_t height;
    h2_mp4_decoder_video_plane_t video_planes[H2_VIDEO_DECODER_MAX_PLANES];
    uint8_t video_plane_count;
    int16_t *pcm;
    size_t pcm_samples_per_channel;
    uint32_t pcm_sample_rate_hz;
    uint8_t pcm_channels;
} h2_mp4_decoder_frame_info_t;

typedef struct h2_mp4_decoder h2_mp4_decoder_t;
typedef struct h2_mp4_decoder_frame h2_mp4_decoder_frame_t;

h2_pal_result_t h2_mp4_decoder_open(
    const h2_mp4_decoder_config_t *config,
    h2_mp4_decoder_t **out_decoder);
h2_pal_result_t h2_mp4_decoder_get_info(
    h2_mp4_decoder_t *decoder,
    h2_mp4_decoder_info_t *out_info);
h2_pal_result_t h2_mp4_decoder_acquire_frame(
    h2_mp4_decoder_t *decoder,
    uint32_t timeout_ms,
    h2_mp4_decoder_frame_t **out_frame);
h2_pal_result_t h2_mp4_decoder_frame_get_info(
    h2_mp4_decoder_t *decoder,
    h2_mp4_decoder_frame_t *frame,
    h2_mp4_decoder_frame_info_t *out_info);
h2_pal_result_t h2_mp4_decoder_release_frame(
    h2_mp4_decoder_t *decoder,
    h2_mp4_decoder_frame_t *frame);
h2_pal_result_t h2_mp4_decoder_seek(h2_mp4_decoder_t *decoder, int64_t target_us);
h2_pal_result_t h2_mp4_decoder_reset(h2_mp4_decoder_t *decoder);
h2_pal_result_t h2_mp4_decoder_close(h2_mp4_decoder_t *decoder);

#ifdef __cplusplus
}
#endif

#endif
