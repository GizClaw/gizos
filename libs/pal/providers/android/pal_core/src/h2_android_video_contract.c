#include "h2_android_video_internal.h"

#include <limits.h>

static int clamp_byte(int value) {
  if (value < 0) {
    return 0;
  }
  if (value > 255) {
    return 255;
  }
  return value;
}

h2_pal_result_t h2_android_video_validate_decoder_open(
    const h2_video_decoder_config_t *config,
    h2_pal_video_decoder_session_t **out_session) {
  if (out_session == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  *out_session = NULL;
  if (config == NULL ||
      !h2_video_decoder_allocator_is_valid(config->frame_allocator)) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  return H2_PAL_OK;
}

h2_pal_result_t h2_android_video_validate_decoder_stream(
    const h2_video_decoder_stream_config_t *config) {
  if (config == NULL || config->codec != H2_VIDEO_CODEC_H264 ||
      config->bitstream_format != H2_VIDEO_BITSTREAM_H264_ANNEX_B ||
      config->codec_config == NULL || config->codec_config_size < 4u) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (config->visible_width != config->coded_width ||
      config->visible_height != config->coded_height ||
      config->coded_width > INT32_MAX || config->coded_height > INT32_MAX) {
    return H2_PAL_ERR_UNSUPPORTED;
  }
  const uint8_t *codec_config = config->codec_config;
  if (codec_config[0] != 0u || codec_config[1] != 0u ||
      (codec_config[2] != 1u &&
       (codec_config[2] != 0u || codec_config[3] != 1u))) {
    return H2_PAL_ERR_FORMAT;
  }
  return H2_PAL_OK;
}

h2_pal_result_t
h2_android_video_copy_yuv420p_to_rgb565(uint8_t *output, size_t output_capacity,
                                        const uint8_t *input, size_t input_size,
                                        uint32_t width, uint32_t height,
                                        size_t stride, size_t slice_height) {
  if (output == NULL || input == NULL || width == 0u || height == 0u ||
      stride < width || slice_height < height || (width & 1u) != 0u ||
      (height & 1u) != 0u) {
    return H2_PAL_ERR_FORMAT;
  }
  const size_t chroma_stride = (stride + 1u) / 2u;
  const size_t chroma_height = (slice_height + 1u) / 2u;
  if (stride > SIZE_MAX / slice_height ||
      chroma_stride > SIZE_MAX / chroma_height) {
    return H2_PAL_ERR_FORMAT;
  }
  const size_t luma_bytes = stride * slice_height;
  const size_t chroma_bytes = chroma_stride * chroma_height;
  if (chroma_bytes > (SIZE_MAX - luma_bytes) / 2u ||
      luma_bytes + chroma_bytes * 2u > input_size ||
      width > SIZE_MAX / height ||
      (size_t)width * height > output_capacity / sizeof(uint16_t)) {
    return H2_PAL_ERR_FORMAT;
  }
  const uint8_t *u_plane = input + luma_bytes;
  const uint8_t *v_plane = u_plane + chroma_bytes;
  for (uint32_t y = 0u; y < height; ++y) {
    uint16_t *destination =
        (uint16_t *)(void *)(output + (size_t)y * width * sizeof(uint16_t));
    const uint8_t *y_row = input + (size_t)y * stride;
    const uint8_t *u_row = u_plane + (size_t)(y / 2u) * chroma_stride;
    const uint8_t *v_row = v_plane + (size_t)(y / 2u) * chroma_stride;
    for (uint32_t x = 0u; x < width; ++x) {
      const int c = (int)y_row[x] - 16;
      const int d = (int)u_row[x / 2u] - 128;
      const int e = (int)v_row[x / 2u] - 128;
      const int red = clamp_byte((298 * c + 409 * e + 128) >> 8);
      const int green = clamp_byte((298 * c - 100 * d - 208 * e + 128) >> 8);
      const int blue = clamp_byte((298 * c + 516 * d + 128) >> 8);
      destination[x] =
          (uint16_t)(((uint16_t)(red & 0xf8) << 8) |
                     ((uint16_t)(green & 0xfc) << 3) | ((uint16_t)blue >> 3));
    }
  }
  return H2_PAL_OK;
}
