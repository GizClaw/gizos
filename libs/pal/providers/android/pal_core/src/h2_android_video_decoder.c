#include "h2_android_platform.h"
#include "h2_android_video_internal.h"

#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct h2_pal_video_decoder_frame {
  h2_pal_video_decoder_session_t *owner;
  uint8_t *pixels;
  size_t bytes;
  uint32_t width;
  uint32_t height;
  int64_t pts_us;
  int64_t duration_us;
};

struct h2_pal_video_decoder_session {
  h2_pal_mem_api_t allocator;
  AMediaCodec *codec;
  uint32_t width;
  uint32_t height;
  int32_t stride;
  int32_t slice_height;
  int32_t color_format;
  int64_t last_duration_us;
  int configured;
  int acquired;
  int eos_reached;
  int eos_after_frame;
  uint8_t *output;
  size_t output_capacity;
  h2_pal_video_decoder_frame_t frame;
};

static void drop_codec(h2_pal_video_decoder_session_t *session) {
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
  h2_pal_mem_free(&session->allocator, session->output);
  session->output = NULL;
  session->output_capacity = 0u;
}

static h2_pal_result_t
android_video_open(void *user, const h2_video_decoder_config_t *config,
                   h2_pal_video_decoder_session_t **out_session) {
  (void)user;
  const h2_pal_result_t validation =
      h2_android_video_validate_decoder_open(config, out_session);
  if (validation != H2_PAL_OK) {
    return validation;
  }
  if (config->preferred_format != H2_VIDEO_PIXEL_FORMAT_UNSPECIFIED &&
      config->preferred_format != H2_VIDEO_PIXEL_FORMAT_RGB565) {
    return H2_PAL_ERR_UNSUPPORTED;
  }
  h2_pal_video_decoder_session_t *session =
      h2_pal_mem_alloc(config->frame_allocator, sizeof(*session));
  if (session == NULL) {
    return H2_PAL_ERR_NO_MEMORY;
  }
  memset(session, 0, sizeof(*session));
  session->allocator = *config->frame_allocator;
  *out_session = session;
  return H2_PAL_OK;
}

static h2_pal_result_t
android_video_configure(void *user, h2_pal_video_decoder_session_t *session,
                        const h2_video_decoder_stream_config_t *config) {
  (void)user;
  if (session->configured || session->acquired) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  const h2_pal_result_t validation =
      h2_android_video_validate_decoder_stream(config);
  if (validation != H2_PAL_OK) {
    return validation;
  }
  AMediaCodec *codec = AMediaCodec_createDecoderByType("video/avc");
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
  AMediaFormat_setString(format, AMEDIAFORMAT_KEY_MIME, "video/avc");
  AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_WIDTH,
                        (int32_t)config->coded_width);
  AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_HEIGHT,
                        (int32_t)config->coded_height);
  AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_COLOR_FORMAT,
                        H2_ANDROID_COLOR_FORMAT_YUV420_PLANAR);
  AMediaFormat_setBuffer(format, "csd-0", config->codec_config,
                         config->codec_config_size);
  media_status_t status = AMediaCodec_configure(codec, format, NULL, NULL, 0u);
  (void)AMediaFormat_delete(format);
  if (status == AMEDIA_OK) {
    status = AMediaCodec_start(codec);
  }
  if (status != AMEDIA_OK) {
    (void)AMediaCodec_delete(codec);
    return status == AMEDIA_ERROR_UNSUPPORTED ? H2_PAL_ERR_UNSUPPORTED
                                              : H2_PAL_ERR_IO;
  }
  const size_t pixels = (size_t)config->visible_width * config->visible_height;
  uint8_t *output =
      h2_pal_mem_alloc(&session->allocator, pixels * sizeof(uint16_t));
  if (output == NULL) {
    (void)AMediaCodec_stop(codec);
    (void)AMediaCodec_delete(codec);
    return H2_PAL_ERR_NO_MEMORY;
  }
  session->codec = codec;
  session->output = output;
  session->output_capacity = pixels * sizeof(uint16_t);
  session->width = config->visible_width;
  session->height = config->visible_height;
  session->stride = (int32_t)config->coded_width;
  session->slice_height = (int32_t)config->coded_height;
  session->color_format = H2_ANDROID_COLOR_FORMAT_YUV420_PLANAR;
  session->configured = 1;
  return H2_PAL_OK;
}

static h2_pal_result_t
android_video_submit(void *user, h2_pal_video_decoder_session_t *session,
                     const h2_video_decoder_packet_t *packet) {
  (void)user;
  if (!session->configured || session->eos_reached) {
    return H2_PAL_ERR_INVALID_STATE;
  }
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
      (packet->flags & H2_VIDEO_DECODER_PACKET_END_OF_STREAM) != 0u
          ? AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM
          : 0u;
  const media_status_t status = AMediaCodec_queueInputBuffer(
      session->codec, (size_t)index, 0u, packet->size, (uint64_t)packet->pts_us,
      flags);
  if (status != AMEDIA_OK) {
    return H2_PAL_ERR_IO;
  }
  if (packet->duration_us > 0) {
    session->last_duration_us = packet->duration_us;
  }
  return H2_PAL_OK;
}

static void update_output_format(h2_pal_video_decoder_session_t *session) {
  AMediaFormat *format = AMediaCodec_getOutputFormat(session->codec);
  if (format == NULL) {
    return;
  }
  int32_t value = 0;
  if (AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_STRIDE, &value) &&
      value > 0) {
    session->stride = value;
  }
  if (AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_SLICE_HEIGHT, &value) &&
      value > 0) {
    session->slice_height = value;
  }
  if (AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_COLOR_FORMAT, &value)) {
    session->color_format = value;
  }
  (void)AMediaFormat_delete(format);
}

static h2_pal_result_t
android_video_acquire(void *user, h2_pal_video_decoder_session_t *session,
                      uint32_t timeout_ms,
                      h2_pal_video_decoder_frame_t **out_frame) {
  (void)user;
  if (!session->configured || session->acquired) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  if (session->eos_reached) {
    return H2_PAL_EXIT;
  }
  const int64_t timeout_us =
      timeout_ms == 0u ? 5000 : (int64_t)timeout_ms * 1000;
  for (;;) {
    AMediaCodecBufferInfo info = {0};
    const ssize_t index =
        AMediaCodec_dequeueOutputBuffer(session->codec, &info, timeout_us);
    if (index == AMEDIACODEC_INFO_TRY_AGAIN_LATER) {
      return H2_PAL_ERR_WOULD_BLOCK;
    }
    if (index == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
      update_output_format(session);
      continue;
    }
    if (index == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) {
      continue;
    }
    if (index < 0) {
      return H2_PAL_ERR_IO;
    }
    const int eos = (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) != 0u;
    if (info.size == 0) {
      (void)AMediaCodec_releaseOutputBuffer(session->codec, (size_t)index,
                                            false);
      if (eos) {
        session->eos_reached = 1;
        return H2_PAL_EXIT;
      }
      continue;
    }
    if (session->color_format != H2_ANDROID_COLOR_FORMAT_YUV420_PLANAR ||
        session->stride < (int32_t)session->width ||
        session->slice_height < (int32_t)session->height) {
      (void)AMediaCodec_releaseOutputBuffer(session->codec, (size_t)index,
                                            false);
      return H2_PAL_ERR_UNSUPPORTED;
    }
    size_t capacity = 0u;
    uint8_t *buffer =
        AMediaCodec_getOutputBuffer(session->codec, (size_t)index, &capacity);
    const size_t stride = (size_t)session->stride;
    const size_t slice_height = (size_t)session->slice_height;
    const size_t pixels = (size_t)session->width * session->height;
    if (buffer == NULL || info.offset < 0 || info.size < 0 ||
        (size_t)info.offset > capacity ||
        (size_t)info.size > capacity - (size_t)info.offset ||
        pixels > SIZE_MAX / sizeof(uint16_t)) {
      (void)AMediaCodec_releaseOutputBuffer(session->codec, (size_t)index,
                                            false);
      return H2_PAL_ERR_FORMAT;
    }
    if (session->output_capacity < pixels * sizeof(uint16_t)) {
      (void)AMediaCodec_releaseOutputBuffer(session->codec, (size_t)index,
                                            false);
      return H2_PAL_ERR_NO_MEMORY;
    }
    const h2_pal_result_t copy_result = h2_android_video_copy_yuv420p_to_rgb565(
        session->output, session->output_capacity, buffer + (size_t)info.offset,
        (size_t)info.size, session->width, session->height, stride,
        slice_height);
    (void)AMediaCodec_releaseOutputBuffer(session->codec, (size_t)index, false);
    if (copy_result != H2_PAL_OK) {
      return copy_result;
    }
    session->frame = (h2_pal_video_decoder_frame_t){
        .owner = session,
        .pixels = session->output,
        .bytes = pixels * sizeof(uint16_t),
        .width = session->width,
        .height = session->height,
        .pts_us = info.presentationTimeUs,
        .duration_us = session->last_duration_us,
    };
    session->acquired = 1;
    session->eos_after_frame = eos;
    *out_frame = &session->frame;
    return H2_PAL_OK;
  }
}

static h2_pal_result_t android_video_frame_get_info(
    void *user, h2_pal_video_decoder_session_t *session,
    h2_pal_video_decoder_frame_t *frame, h2_video_frame_info_t *out_info) {
  (void)user;
  if (!session->acquired || frame->owner != session) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  *out_info = (h2_video_frame_info_t){
      .format = H2_VIDEO_PIXEL_FORMAT_RGB565,
      .width = frame->width,
      .height = frame->height,
      .planes = {{
          .data = frame->pixels,
          .bytes = frame->bytes,
          .stride_bytes = (size_t)frame->width * sizeof(uint16_t),
      }},
      .plane_count = 1u,
      .pts_us = frame->pts_us,
      .duration_us = frame->duration_us,
  };
  return H2_PAL_OK;
}

static h2_pal_result_t
android_video_release(void *user, h2_pal_video_decoder_session_t *session,
                      h2_pal_video_decoder_frame_t *frame) {
  (void)user;
  if (!session->acquired || frame->owner != session) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  session->acquired = 0;
  if (session->eos_after_frame) {
    session->eos_reached = 1;
    session->eos_after_frame = 0;
  }
  return H2_PAL_OK;
}

static h2_pal_result_t
android_video_reset(void *user, h2_pal_video_decoder_session_t *session) {
  (void)user;
  if (session->acquired) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  drop_codec(session);
  return H2_PAL_OK;
}

static h2_pal_result_t
android_video_close(void *user, h2_pal_video_decoder_session_t *session) {
  (void)user;
  if (session->acquired) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  drop_codec(session);
  h2_pal_mem_free(&session->allocator, session);
  return H2_PAL_OK;
}

static const h2_pal_video_decoder_vtable_t s_android_video_vtable = {
    .open = android_video_open,
    .configure = android_video_configure,
    .submit_packet = android_video_submit,
    .acquire_frame = android_video_acquire,
    .frame_get_info = android_video_frame_get_info,
    .release_frame = android_video_release,
    .reset = android_video_reset,
    .close = android_video_close,
};

static const h2_pal_video_decoder_api_t s_android_video_api = {
    .user = NULL,
    .vtable = &s_android_video_vtable,
};

const h2_pal_video_decoder_api_t *h2_android_platform_video_decoder_api(void) {
  return &s_android_video_api;
}
