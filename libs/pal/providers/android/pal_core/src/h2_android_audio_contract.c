#include "h2_android_audio_internal.h"

#include <limits.h>
#include <string.h>

h2_pal_result_t h2_android_audio_validate_decoder_stream(
    const h2_audio_decoder_stream_config_t *config) {
  if (config == NULL || config->codec != H2_AUDIO_CODEC_AAC_LC ||
      config->bitstream_format != H2_AUDIO_BITSTREAM_AAC_RAW ||
      config->sample_rate_hz == 0u || config->sample_rate_hz > INT32_MAX ||
      config->channels == 0u || config->codec_config == NULL ||
      config->codec_config_size == 0u) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  return H2_PAL_OK;
}

h2_pal_result_t h2_android_audio_copy_pcm(
    const h2_pal_mem_api_t *allocator, const uint8_t *buffer,
    size_t capacity, int32_t offset, int32_t bytes, uint32_t sample_rate_hz,
    uint8_t channels, int64_t pts_us, h2_android_audio_pcm_copy_t *out_copy) {
  if (!h2_audio_decoder_allocator_is_valid(allocator) || buffer == NULL ||
      out_copy == NULL || offset < 0 || bytes <= 0 || sample_rate_hz == 0u ||
      channels == 0u || (size_t)offset > capacity ||
      (size_t)bytes > capacity - (size_t)offset ||
      ((size_t)bytes % ((size_t)channels * sizeof(int16_t))) != 0u) {
    return H2_PAL_ERR_FORMAT;
  }
  const size_t samples =
      (size_t)bytes / ((size_t)channels * sizeof(int16_t));
  if (samples == 0u || samples > UINT32_MAX) {
    return H2_PAL_ERR_FORMAT;
  }
  memset(out_copy, 0, sizeof(*out_copy));
  out_copy->data = h2_pal_mem_alloc(allocator, (size_t)bytes);
  if (out_copy->data == NULL) {
    return H2_PAL_ERR_NO_MEMORY;
  }
  memcpy(out_copy->data, buffer + offset, (size_t)bytes);
  out_copy->info = (h2_audio_decoder_frame_info_t){
      .data = out_copy->data,
      .bytes = (size_t)bytes,
      .sample_rate_hz = sample_rate_hz,
      .samples_per_channel = (uint32_t)samples,
      .channels = channels,
      .sample_format = H2_AUDIO_SAMPLE_S16LE,
      .pts_us = pts_us,
      .duration_us = (int64_t)samples * 1000000 / sample_rate_hz,
  };
  return H2_PAL_OK;
}

void h2_android_audio_release_pcm(const h2_pal_mem_api_t *allocator,
                                  h2_android_audio_pcm_copy_t *copy) {
  if (copy == NULL) {
    return;
  }
  h2_pal_mem_free(allocator, copy->data);
  memset(copy, 0, sizeof(*copy));
}

int h2_android_audio_validate_track_config(
    const h2_audio_track_config_t *config) {
  if (config == NULL || config->format.sample_rate_hz != 16000u ||
      config->format.frame_samples_per_channel != 512u ||
      config->format.channels != 1u ||
      config->format.sample_format != H2_AUDIO_SAMPLE_S16LE ||
      config->volume_factor_milli > 1000u || config->buffer_frames == 0u ||
      config->buffer_frames >
          INT32_MAX / config->format.frame_samples_per_channel) {
    return H2_AUDIO_ERR_INVALID_ARG;
  }
  return H2_PAL_OK;
}

int h2_android_audio_validate_playback_frame(
    const h2_audio_pcm_format_t *format, const h2_audio_frame_t *frame) {
  if (format == NULL || frame == NULL || frame->data == NULL ||
      frame->sample_rate_hz != format->sample_rate_hz ||
      frame->samples_per_channel != format->frame_samples_per_channel ||
      frame->channels != format->channels ||
      frame->sample_format != format->sample_format ||
      frame->bytes != (size_t)frame->samples_per_channel * frame->channels *
                          sizeof(int16_t)) {
    return H2_AUDIO_ERR_INVALID_ARG;
  }
  return H2_PAL_OK;
}

h2_pal_result_t h2_android_audio_map_media_config_status(int32_t status) {
  return status == 0 ? H2_PAL_OK : H2_PAL_ERR_UNSUPPORTED;
}

h2_pal_result_t h2_android_audio_map_media_io_status(int32_t status) {
  return status == 0 ? H2_PAL_OK : H2_PAL_ERR_IO;
}

int h2_android_audio_map_aaudio_open_result(int32_t result) {
  return result == 0 ? H2_PAL_OK : H2_AUDIO_ERR_UNAVAILABLE;
}

int h2_android_audio_map_aaudio_io_result(int32_t result) {
  return result == 0 ? H2_PAL_OK : H2_AUDIO_ERR_IO;
}

int h2_android_audio_map_aaudio_write_result(int32_t result,
                                             int32_t timeout_error) {
  if (result >= 0) {
    return H2_PAL_OK;
  }
  return result == timeout_error ? H2_AUDIO_ERR_WOULD_BLOCK
                                 : H2_AUDIO_ERR_IO;
}

int h2_android_audio_should_retain_partial_write(uint32_t written_frames,
                                                 uint32_t total_frames,
                                                 int32_t write_result) {
  return written_frames > 0u && written_frames < total_frames &&
         write_result <= 0;
}
