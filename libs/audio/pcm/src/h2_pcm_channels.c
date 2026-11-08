#include "h2_pcm_channels.h"

static int32_t read_s16le(const uint8_t* sample) {
  const uint32_t value = (uint32_t)sample[0] | ((uint32_t)sample[1] << 8u);
  return value >= 32768u ? (int32_t)value - 65536 : (int32_t)value;
}

int h2_pcm_s16_select_mono(const h2_audio_frame_t* raw_frame,
                           h2_audio_frame_t* out_frame,
                           const uint8_t* channel_indices,
                           uint8_t channel_count) {
  if (raw_frame == NULL || out_frame == NULL || channel_indices == NULL) {
    return H2_AUDIO_ERR_INVALID_ARG;
  }
  if (raw_frame->data == NULL || out_frame->data == NULL ||
      raw_frame->sample_format != H2_AUDIO_SAMPLE_S16LE ||
      out_frame->sample_format != H2_AUDIO_SAMPLE_S16LE ||
      raw_frame->sample_rate_hz != out_frame->sample_rate_hz ||
      raw_frame->channels == 0u || out_frame->channels != 1u ||
      channel_count == 0u) {
    return H2_AUDIO_ERR_UNSUPPORTED;
  }
  for (uint8_t channel = 0u; channel < channel_count; ++channel) {
    if (channel_indices[channel] >= raw_frame->channels) {
      return H2_AUDIO_ERR_UNSUPPORTED;
    }
  }

  const size_t raw_frame_bytes = h2_audio_frame_frame_bytes(raw_frame);
  const size_t out_frame_bytes = h2_audio_frame_frame_bytes(out_frame);
  if (raw_frame_bytes == 0u || out_frame_bytes == 0u ||
      raw_frame->bytes > raw_frame->capacity) {
    return H2_AUDIO_ERR_INVALID_ARG;
  }
  const size_t raw_frames = raw_frame->bytes / raw_frame_bytes;
  size_t out_frames = out_frame->capacity / out_frame_bytes;
  if (out_frames > raw_frames) {
    out_frames = raw_frames;
  }
  if (out_frames == 0u || out_frames > UINT16_MAX) {
    return H2_AUDIO_ERR_INVALID_ARG;
  }

  const uint8_t* raw = (const uint8_t*)raw_frame->data;
  uint8_t* out = (uint8_t*)out_frame->data;
  for (size_t i = 0u; i < out_frames; ++i) {
    /* At most UINT8_MAX S16 values fit comfortably in int32_t. Frame offsets
     * are bounded by bytes/capacity before multiplying or touching storage. */
    int32_t sum = 0;
    for (uint8_t channel = 0u; channel < channel_count; ++channel) {
      const size_t offset = i * raw_frame_bytes +
                            (size_t)channel_indices[channel] * sizeof(int16_t);
      sum += read_s16le(raw + offset);
    }
    const uint16_t value = (uint16_t)(sum / (int32_t)channel_count);
    out[i * out_frame_bytes] = (uint8_t)value;
    out[i * out_frame_bytes + 1u] = (uint8_t)(value >> 8u);
  }
  out_frame->bytes = out_frames * out_frame_bytes;
  out_frame->samples_per_channel = (uint16_t)out_frames;
  return H2_AUDIO_OK;
}
