#ifndef H2_MP4_DECODER_SIZING_H
#define H2_MP4_DECODER_SIZING_H

#include "h2/pal/hal/h2_pal_video_decoder.h"

#include <stddef.h>
#include <stdint.h>

static inline int h2_mp4_decoder_sizing_multiply(
    size_t left,
    size_t right,
    size_t *out) {
    if (right != 0u && left > SIZE_MAX / right) {
        return 0;
    }
    *out = left * right;
    return 1;
}

static inline int h2_mp4_decoder_annex_b_capacity(
    uint8_t nal_length_size,
    size_t sample_size,
    size_t *out) {
    if (out == NULL || nal_length_size == 0u || nal_length_size > 4u) {
        return 0;
    }

    const size_t max_nal_units =
        sample_size / ((size_t)nal_length_size + 1u);
    const size_t expansion_per_nal = 4u - nal_length_size;
    size_t expansion = 0u;
    if (!h2_mp4_decoder_sizing_multiply(
            max_nal_units, expansion_per_nal, &expansion) ||
        sample_size > SIZE_MAX - expansion) {
        return 0;
    }
    *out = sample_size + expansion;
    return 1;
}

static inline int h2_mp4_decoder_video_frame_capacity(
    h2_video_pixel_format_t format,
    size_t width,
    size_t height,
    size_t *out) {
    if (out == NULL) {
        return 0;
    }

    size_t luma = 0u;
    if (!h2_mp4_decoder_sizing_multiply(width, height, &luma)) {
        return 0;
    }
    switch (format) {
        case H2_VIDEO_PIXEL_FORMAT_NV12:
        case H2_VIDEO_PIXEL_FORMAT_YUV420P: {
            const size_t chroma_width = width / 2u + width % 2u;
            const size_t chroma_height = height / 2u + height % 2u;
            size_t chroma_plane = 0u;
            size_t chroma_bytes = 0u;
            if (!h2_mp4_decoder_sizing_multiply(
                    chroma_width, chroma_height, &chroma_plane) ||
                !h2_mp4_decoder_sizing_multiply(
                    chroma_plane, 2u, &chroma_bytes) ||
                luma > SIZE_MAX - chroma_bytes) {
                return 0;
            }
            *out = luma + chroma_bytes;
            return 1;
        }
        case H2_VIDEO_PIXEL_FORMAT_RGB565:
            return h2_mp4_decoder_sizing_multiply(luma, 2u, out);
        case H2_VIDEO_PIXEL_FORMAT_RGB888:
            return h2_mp4_decoder_sizing_multiply(luma, 3u, out);
        case H2_VIDEO_PIXEL_FORMAT_RGBA8888:
            return h2_mp4_decoder_sizing_multiply(luma, 4u, out);
        case H2_VIDEO_PIXEL_FORMAT_UNSPECIFIED:
        default:
            return 0;
    }
}

#endif
