#ifndef H2_ANDROID_AUDIO_INTERNAL_H
#define H2_ANDROID_AUDIO_INTERNAL_H

#include "h2_pal.h"

#include <stddef.h>
#include <stdint.h>

typedef struct h2_android_audio_pcm_copy {
  void *data;
  h2_audio_decoder_frame_info_t info;
} h2_android_audio_pcm_copy_t;

h2_pal_result_t h2_android_audio_validate_decoder_stream(
    const h2_audio_decoder_stream_config_t *config);
h2_pal_result_t h2_android_audio_copy_pcm(
    const h2_pal_mem_api_t *allocator, const uint8_t *buffer,
    size_t capacity, int32_t offset, int32_t bytes, uint32_t sample_rate_hz,
    uint8_t channels, int64_t pts_us, h2_android_audio_pcm_copy_t *out_copy);
void h2_android_audio_release_pcm(const h2_pal_mem_api_t *allocator,
                                  h2_android_audio_pcm_copy_t *copy);

int h2_android_audio_validate_track_config(
    const h2_audio_track_config_t *config);
int h2_android_audio_validate_playback_frame(
    const h2_audio_pcm_format_t *format, const h2_audio_frame_t *frame);

h2_pal_result_t h2_android_audio_map_media_config_status(int32_t status);
h2_pal_result_t h2_android_audio_map_media_io_status(int32_t status);
int h2_android_audio_map_aaudio_open_result(int32_t result);
int h2_android_audio_map_aaudio_io_result(int32_t result);
int h2_android_audio_map_aaudio_write_result(int32_t result,
                                             int32_t timeout_error);
int h2_android_audio_should_retain_partial_write(uint32_t written_frames,
                                                 uint32_t total_frames,
                                                 int32_t write_result);

#endif
