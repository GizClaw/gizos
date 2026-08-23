#ifndef H2_ANDROID_VIDEO_INTERNAL_H
#define H2_ANDROID_VIDEO_INTERNAL_H

#include "h2_pal.h"

#include <stddef.h>
#include <stdint.h>

enum {
  H2_ANDROID_COLOR_FORMAT_YUV420_PLANAR = 19,
};

h2_pal_result_t h2_android_video_validate_decoder_open(
    const h2_video_decoder_config_t *config,
    h2_pal_video_decoder_session_t **out_session);
h2_pal_result_t h2_android_video_validate_decoder_stream(
    const h2_video_decoder_stream_config_t *config);
h2_pal_result_t
h2_android_video_copy_yuv420p_to_rgb565(uint8_t *output, size_t output_capacity,
                                        const uint8_t *input, size_t input_size,
                                        uint32_t width, uint32_t height,
                                        size_t stride, size_t slice_height);

#endif
