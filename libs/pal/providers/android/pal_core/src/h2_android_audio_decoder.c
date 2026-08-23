#include "h2_android_platform.h"
#include "h2_android_audio_internal.h"

#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct h2_pal_audio_decoder_frame {
  h2_pal_audio_decoder_session_t *owner;
  h2_android_audio_pcm_copy_t pcm;
};

struct h2_pal_audio_decoder_session {
  const h2_pal_mem_api_t *allocator;
  AMediaCodec *codec;
  uint32_t sample_rate_hz;
  uint8_t channels;
  int configured;
  int acquired;
  int eos_reached;
  int eos_after_frame;
};

static void android_audio_decoder_drop_codec(
    h2_pal_audio_decoder_session_t *session) {
  if (session->codec != NULL) {
    if (session->configured) {
      (void)AMediaCodec_stop(session->codec);
    }
    (void)AMediaCodec_delete(session->codec);
  }
  session->codec = NULL;
  session->configured = 0;
  session->eos_reached = 0;
  session->eos_after_frame = 0;
}

static h2_pal_result_t android_audio_decoder_open(
    void *user, const h2_audio_decoder_config_t *config,
    h2_pal_audio_decoder_session_t **out_session) {
  (void)user;
  if (config == NULL || out_session == NULL ||
      config->preferred_format != H2_AUDIO_SAMPLE_S16LE) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  h2_pal_audio_decoder_session_t *session = calloc(1u, sizeof(*session));
  if (session == NULL) {
    return H2_PAL_ERR_NO_MEMORY;
  }
  session->allocator = config->pcm_allocator;
  *out_session = session;
  return H2_PAL_OK;
}

static h2_pal_result_t android_audio_decoder_configure(
    void *user, h2_pal_audio_decoder_session_t *session,
    const h2_audio_decoder_stream_config_t *config) {
  (void)user;
  if (session == NULL || session->configured || session->acquired) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  const h2_pal_result_t validation =
      h2_android_audio_validate_decoder_stream(config);
  if (validation != H2_PAL_OK) {
    return validation;
  }
  AMediaCodec *codec = AMediaCodec_createDecoderByType("audio/mp4a-latm");
  AMediaFormat *format = AMediaFormat_new();
  if (codec == NULL || format == NULL) {
    if (codec != NULL) {
      (void)AMediaCodec_delete(codec);
    }
    if (format != NULL) {
      (void)AMediaFormat_delete(format);
    }
    return H2_PAL_ERR_UNAVAILABLE;
  }
  AMediaFormat_setString(format, AMEDIAFORMAT_KEY_MIME, "audio/mp4a-latm");
  AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_SAMPLE_RATE,
                       (int32_t)config->sample_rate_hz);
  AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_CHANNEL_COUNT,
                       (int32_t)config->channels);
  AMediaFormat_setBuffer(format, "csd-0", config->codec_config,
                         config->codec_config_size);
  h2_pal_result_t result = h2_android_audio_map_media_config_status(
      AMediaCodec_configure(codec, format, NULL, NULL, 0u));
  (void)AMediaFormat_delete(format);
  if (result == H2_PAL_OK) {
    result = h2_android_audio_map_media_config_status(
        AMediaCodec_start(codec));
  }
  if (result != H2_PAL_OK) {
    (void)AMediaCodec_delete(codec);
    return result;
  }
  session->codec = codec;
  session->sample_rate_hz = config->sample_rate_hz;
  session->channels = config->channels;
  session->configured = 1;
  return H2_PAL_OK;
}

static h2_pal_result_t android_audio_decoder_submit_packet(
    void *user, h2_pal_audio_decoder_session_t *session,
    const h2_audio_decoder_packet_t *packet) {
  (void)user;
  if (session == NULL || packet == NULL || !session->configured ||
      session->eos_reached) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  /* MediaCodec schedules work asynchronously. A short bounded handoff keeps
   * the synchronous PAL pump from observing both sides as stalled. */
  const ssize_t index = AMediaCodec_dequeueInputBuffer(session->codec, 5000);
  if (index == AMEDIACODEC_INFO_TRY_AGAIN_LATER) {
    return H2_PAL_ERR_WOULD_BLOCK;
  }
  if (index < 0) {
    return H2_PAL_ERR_IO;
  }
  size_t capacity = 0u;
  uint8_t *buffer =
      AMediaCodec_getInputBuffer(session->codec, (size_t)index, &capacity);
  if (buffer == NULL || packet->size > capacity) {
    return H2_PAL_ERR_FORMAT;
  }
  if (packet->size != 0u) {
    memcpy(buffer, packet->data, packet->size);
  }
  const uint32_t flags =
      (packet->flags & H2_AUDIO_DECODER_PACKET_END_OF_STREAM) != 0u
          ? AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM
          : 0u;
  return h2_android_audio_map_media_io_status(AMediaCodec_queueInputBuffer(
      session->codec, (size_t)index, 0u, packet->size,
      (uint64_t)packet->pts_us, flags));
}

static void android_audio_decoder_update_format(
    h2_pal_audio_decoder_session_t *session) {
  AMediaFormat *format = AMediaCodec_getOutputFormat(session->codec);
  if (format == NULL) {
    return;
  }
  int32_t value = 0;
  if (AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_SAMPLE_RATE, &value) &&
      value > 0) {
    session->sample_rate_hz = (uint32_t)value;
  }
  if (AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_CHANNEL_COUNT, &value) &&
      value > 0 && value <= UINT8_MAX) {
    session->channels = (uint8_t)value;
  }
  (void)AMediaFormat_delete(format);
}

static h2_pal_result_t android_audio_decoder_acquire_frame(
    void *user, h2_pal_audio_decoder_session_t *session, uint32_t timeout_ms,
    h2_pal_audio_decoder_frame_t **out_frame) {
  (void)user;
  if (session == NULL || out_frame == NULL || !session->configured ||
      session->acquired) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  if (session->eos_reached) {
    return H2_PAL_EXIT;
  }
  const int64_t timeout_us =
      timeout_ms == 0u ? 5000 : (int64_t)timeout_ms * 1000;
  for (;;) {
    AMediaCodecBufferInfo buffer_info = {0};
    const ssize_t index = AMediaCodec_dequeueOutputBuffer(
        session->codec, &buffer_info, timeout_us);
    if (index == AMEDIACODEC_INFO_TRY_AGAIN_LATER) {
      return H2_PAL_ERR_WOULD_BLOCK;
    }
    if (index == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
      android_audio_decoder_update_format(session);
      continue;
    }
    if (index == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) {
      continue;
    }
    if (index < 0) {
      return H2_PAL_ERR_IO;
    }
    const int eos =
        (buffer_info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) != 0u;
    if (buffer_info.size == 0) {
      (void)AMediaCodec_releaseOutputBuffer(session->codec, (size_t)index,
                                            false);
      if (eos) {
        session->eos_reached = 1;
        return H2_PAL_EXIT;
      }
      continue;
    }
    size_t capacity = 0u;
    uint8_t *buffer =
        AMediaCodec_getOutputBuffer(session->codec, (size_t)index, &capacity);
    h2_pal_audio_decoder_frame_t *frame = calloc(1u, sizeof(*frame));
    if (frame == NULL) {
      (void)AMediaCodec_releaseOutputBuffer(session->codec, (size_t)index,
                                            false);
      return H2_PAL_ERR_NO_MEMORY;
    }
    const h2_pal_result_t copy_result = h2_android_audio_copy_pcm(
        session->allocator, buffer, capacity, buffer_info.offset,
        buffer_info.size, session->sample_rate_hz, session->channels,
        buffer_info.presentationTimeUs, &frame->pcm);
    (void)AMediaCodec_releaseOutputBuffer(session->codec, (size_t)index,
                                          false);
    if (copy_result != H2_PAL_OK) {
      free(frame);
      return copy_result;
    }
    frame->owner = session;
    session->acquired = 1;
    session->eos_after_frame = eos;
    *out_frame = frame;
    return H2_PAL_OK;
  }
}

static h2_pal_result_t android_audio_decoder_frame_get_info(
    void *user, h2_pal_audio_decoder_session_t *session,
    h2_pal_audio_decoder_frame_t *frame,
    h2_audio_decoder_frame_info_t *out_info) {
  (void)user;
  if (session == NULL || frame == NULL || out_info == NULL ||
      !session->acquired || frame->owner != session) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  *out_info = frame->pcm.info;
  return H2_PAL_OK;
}

static h2_pal_result_t android_audio_decoder_release_frame(
    void *user, h2_pal_audio_decoder_session_t *session,
    h2_pal_audio_decoder_frame_t *frame) {
  (void)user;
  if (session == NULL || frame == NULL || !session->acquired ||
      frame->owner != session) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  h2_android_audio_release_pcm(session->allocator, &frame->pcm);
  free(frame);
  session->acquired = 0;
  if (session->eos_after_frame) {
    session->eos_reached = 1;
    session->eos_after_frame = 0;
  }
  return H2_PAL_OK;
}

static h2_pal_result_t android_audio_decoder_reset(
    void *user, h2_pal_audio_decoder_session_t *session) {
  (void)user;
  if (session == NULL || session->acquired) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  android_audio_decoder_drop_codec(session);
  return H2_PAL_OK;
}

static h2_pal_result_t android_audio_decoder_close(
    void *user, h2_pal_audio_decoder_session_t *session) {
  (void)user;
  if (session == NULL || session->acquired) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  android_audio_decoder_drop_codec(session);
  free(session);
  return H2_PAL_OK;
}

static const h2_pal_audio_decoder_vtable_t s_android_audio_decoder_vtable = {
    .open = android_audio_decoder_open,
    .configure = android_audio_decoder_configure,
    .submit_packet = android_audio_decoder_submit_packet,
    .acquire_frame = android_audio_decoder_acquire_frame,
    .frame_get_info = android_audio_decoder_frame_get_info,
    .release_frame = android_audio_decoder_release_frame,
    .reset = android_audio_decoder_reset,
    .close = android_audio_decoder_close,
};

static const h2_pal_audio_decoder_api_t s_android_audio_decoder = {
    .user = NULL,
    .vtable = &s_android_audio_decoder_vtable,
};

const h2_pal_audio_decoder_api_t *h2_android_platform_audio_decoder_api(void) {
  return &s_android_audio_decoder;
}
