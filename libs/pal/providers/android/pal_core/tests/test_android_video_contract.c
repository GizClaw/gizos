#include "h2_android_video_internal.h"

#include <assert.h>
#include <stdint.h>

static void *test_alloc(void *user, size_t len) {
  (void)user;
  (void)len;
  return NULL;
}

static void test_free(void *user, void *pointer) {
  (void)user;
  (void)pointer;
}

static const h2_pal_mem_vtable_t s_test_mem_vtable = {
    .alloc = test_alloc,
    .free = test_free,
};

static void test_open_validation(void) {
  const h2_pal_mem_api_t allocator = {
      .vtable = &s_test_mem_vtable,
  };
  const h2_video_decoder_config_t config = {
      .frame_allocator = &allocator,
      .preferred_format = H2_VIDEO_PIXEL_FORMAT_RGB565,
  };
  h2_pal_video_decoder_session_t *session = (void *)(uintptr_t)1u;
  assert(h2_android_video_validate_decoder_open(NULL, &session) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(session == NULL);

  session = (void *)(uintptr_t)1u;
  const h2_video_decoder_config_t missing_allocator = {0};
  assert(h2_android_video_validate_decoder_open(&missing_allocator, &session) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(session == NULL);

  assert(h2_android_video_validate_decoder_open(&config, NULL) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(h2_android_video_validate_decoder_open(&config, &session) ==
         H2_PAL_OK);
  assert(session == NULL);
}

static h2_video_decoder_stream_config_t valid_stream_config(void) {
  static const uint8_t codec_config[] = {0u, 0u, 0u, 1u, 0x67u};
  return (h2_video_decoder_stream_config_t){
      .codec = H2_VIDEO_CODEC_H264,
      .bitstream_format = H2_VIDEO_BITSTREAM_H264_ANNEX_B,
      .coded_width = 2u,
      .coded_height = 2u,
      .visible_width = 2u,
      .visible_height = 2u,
      .codec_config = codec_config,
      .codec_config_size = sizeof(codec_config),
  };
}

static void test_stream_validation(void) {
  h2_video_decoder_stream_config_t config = valid_stream_config();
  assert(h2_android_video_validate_decoder_stream(&config) == H2_PAL_OK);
  config.codec = (h2_video_codec_t)0;
  assert(h2_android_video_validate_decoder_stream(&config) ==
         H2_PAL_ERR_INVALID_ARG);
  config = valid_stream_config();
  config.visible_width = 1u;
  assert(h2_android_video_validate_decoder_stream(&config) ==
         H2_PAL_ERR_UNSUPPORTED);
  config = valid_stream_config();
  static const uint8_t invalid_config[] = {1u, 2u, 3u, 4u};
  config.codec_config = invalid_config;
  config.codec_config_size = sizeof(invalid_config);
  assert(h2_android_video_validate_decoder_stream(&config) ==
         H2_PAL_ERR_FORMAT);
}

static void test_planar_conversion_and_bounds(void) {
  const uint8_t black_yuv[] = {16u, 16u, 16u, 16u, 128u, 128u};
  uint16_t output[4] = {1u, 1u, 1u, 1u};
  assert(h2_android_video_copy_yuv420p_to_rgb565(
             (uint8_t *)output, sizeof(output), black_yuv, sizeof(black_yuv),
             2u, 2u, 2u, 2u) == H2_PAL_OK);
  for (size_t i = 0u; i < 4u; ++i) {
    assert(output[i] == 0u);
  }
  assert(h2_android_video_copy_yuv420p_to_rgb565(
             (uint8_t *)output, sizeof(output), black_yuv,
             sizeof(black_yuv) - 1u, 2u, 2u, 2u, 2u) == H2_PAL_ERR_FORMAT);
  assert(h2_android_video_copy_yuv420p_to_rgb565(
             (uint8_t *)output, sizeof(output) - 1u, black_yuv,
             sizeof(black_yuv), 2u, 2u, 2u, 2u) == H2_PAL_ERR_FORMAT);
  assert(h2_android_video_copy_yuv420p_to_rgb565(
             (uint8_t *)output, sizeof(output), black_yuv, sizeof(black_yuv),
             2u, 2u, 1u, 2u) == H2_PAL_ERR_FORMAT);
}

int main(void) {
  test_open_validation();
  test_stream_validation();
  test_planar_conversion_and_bounds();
  return 0;
}
