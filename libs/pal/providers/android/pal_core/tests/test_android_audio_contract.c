#include "h2_android_audio_internal.h"

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct test_allocator_state {
  int fail;
  size_t allocations;
  size_t frees;
} test_allocator_state_t;

static void *test_alloc(void *user, size_t len) {
  test_allocator_state_t *state = user;
  if (state->fail) {
    return NULL;
  }
  ++state->allocations;
  return malloc(len);
}

static void test_free(void *user, void *pointer) {
  test_allocator_state_t *state = user;
  ++state->frees;
  free(pointer);
}

static const h2_pal_mem_vtable_t s_test_mem_vtable = {
    .alloc = test_alloc,
    .free = test_free,
};

static void test_decoder_stream_validation(void) {
  const uint8_t codec_config[] = {0x14u, 0x08u};
  h2_audio_decoder_stream_config_t config = {
      .codec = H2_AUDIO_CODEC_AAC_LC,
      .bitstream_format = H2_AUDIO_BITSTREAM_AAC_RAW,
      .sample_rate_hz = 16000u,
      .channels = 1u,
      .codec_config = codec_config,
      .codec_config_size = sizeof(codec_config),
  };
  assert(h2_android_audio_validate_decoder_stream(&config) == H2_PAL_OK);
  config.codec = (h2_audio_codec_t)0;
  assert(h2_android_audio_validate_decoder_stream(&config) ==
         H2_PAL_ERR_INVALID_ARG);
  config.codec = H2_AUDIO_CODEC_AAC_LC;
  config.bitstream_format = (h2_audio_bitstream_format_t)0;
  assert(h2_android_audio_validate_decoder_stream(&config) ==
         H2_PAL_ERR_INVALID_ARG);
  config.bitstream_format = H2_AUDIO_BITSTREAM_AAC_RAW;
  config.codec_config_size = 0u;
  assert(h2_android_audio_validate_decoder_stream(&config) ==
         H2_PAL_ERR_INVALID_ARG);
}

static void test_pcm_copy_validation_and_allocation(void) {
  const uint8_t pcm[] = {0x01u, 0x02u, 0x03u, 0x04u};
  test_allocator_state_t state = {0};
  const h2_pal_mem_api_t allocator = {
      .user = &state,
      .vtable = &s_test_mem_vtable,
  };
  h2_android_audio_pcm_copy_t copy = {0};
  assert(h2_android_audio_copy_pcm(&allocator, pcm, sizeof(pcm), 1, 4,
                                   16000u, 1u, 0, &copy) ==
         H2_PAL_ERR_FORMAT);
  assert(h2_android_audio_copy_pcm(&allocator, pcm, sizeof(pcm), 0, 3,
                                   16000u, 1u, 0, &copy) ==
         H2_PAL_ERR_FORMAT);
  assert(h2_android_audio_copy_pcm(&allocator, pcm, sizeof(pcm), 0, 4,
                                   16000u, 3u, 0, &copy) ==
         H2_PAL_ERR_FORMAT);
  state.fail = 1;
  assert(h2_android_audio_copy_pcm(&allocator, pcm, sizeof(pcm), 0, 4,
                                   16000u, 1u, 1000, &copy) ==
         H2_PAL_ERR_NO_MEMORY);
  state.fail = 0;
  assert(h2_android_audio_copy_pcm(&allocator, pcm, sizeof(pcm), 0, 4,
                                   16000u, 1u, 1000, &copy) == H2_PAL_OK);
  assert(copy.info.data == copy.data);
  assert(copy.info.bytes == sizeof(pcm));
  assert(copy.info.samples_per_channel == 2u);
  assert(copy.info.duration_us == 125);
  assert(memcmp(copy.data, pcm, sizeof(pcm)) == 0);
  h2_android_audio_release_pcm(&allocator, &copy);
  assert(state.allocations == 1u);
  assert(state.frees == 1u);
}

static h2_audio_track_config_t valid_track_config(void) {
  return (h2_audio_track_config_t){
      .name = "test",
      .format =
          {
              .sample_rate_hz = 16000u,
              .frame_samples_per_channel = 512u,
              .channels = 1u,
              .sample_format = H2_AUDIO_SAMPLE_S16LE,
          },
      .volume_factor_milli = 1000u,
      .buffer_frames = 4u,
  };
}

static void test_track_and_frame_validation(void) {
  h2_audio_track_config_t config = valid_track_config();
  assert(h2_android_audio_validate_track_config(&config) == H2_PAL_OK);
  config.format.sample_rate_hz = 48000u;
  assert(h2_android_audio_validate_track_config(&config) ==
         H2_AUDIO_ERR_INVALID_ARG);
  config = valid_track_config();
  config.volume_factor_milli = 1001u;
  assert(h2_android_audio_validate_track_config(&config) ==
         H2_AUDIO_ERR_INVALID_ARG);
  config = valid_track_config();
  config.buffer_frames = 0u;
  assert(h2_android_audio_validate_track_config(&config) ==
         H2_AUDIO_ERR_INVALID_ARG);

  int16_t samples[512] = {0};
  h2_audio_frame_t frame = {
      .data = samples,
      .capacity = sizeof(samples),
      .bytes = sizeof(samples),
      .sample_rate_hz = 16000u,
      .samples_per_channel = 512u,
      .channels = 1u,
      .sample_format = H2_AUDIO_SAMPLE_S16LE,
  };
  config = valid_track_config();
  assert(h2_android_audio_validate_playback_frame(&config.format, &frame) ==
         H2_PAL_OK);
  frame.bytes -= sizeof(int16_t);
  assert(h2_android_audio_validate_playback_frame(&config.format, &frame) ==
         H2_AUDIO_ERR_INVALID_ARG);
}

static void test_platform_error_mapping(void) {
  assert(h2_android_audio_map_media_config_status(0) == H2_PAL_OK);
  assert(h2_android_audio_map_media_config_status(-1) ==
         H2_PAL_ERR_UNSUPPORTED);
  assert(h2_android_audio_map_media_io_status(-1) == H2_PAL_ERR_IO);
  assert(h2_android_audio_map_aaudio_open_result(-1) ==
         H2_AUDIO_ERR_UNAVAILABLE);
  assert(h2_android_audio_map_aaudio_io_result(-1) == H2_AUDIO_ERR_IO);
  assert(h2_android_audio_map_aaudio_write_result(-2, -2) ==
         H2_AUDIO_ERR_WOULD_BLOCK);
  assert(h2_android_audio_map_aaudio_write_result(-3, -2) ==
         H2_AUDIO_ERR_IO);
  assert(!h2_android_audio_should_retain_partial_write(0u, 512u, 0));
  assert(h2_android_audio_should_retain_partial_write(128u, 512u, 0));
  assert(h2_android_audio_should_retain_partial_write(128u, 512u, -2));
  assert(h2_android_audio_should_retain_partial_write(128u, 512u, -3));
  assert(!h2_android_audio_should_retain_partial_write(512u, 512u, 0));
}

int main(void) {
  test_decoder_stream_validation();
  test_pcm_copy_validation_and_allocation();
  test_track_and_frame_validation();
  test_platform_error_mapping();
  return 0;
}
