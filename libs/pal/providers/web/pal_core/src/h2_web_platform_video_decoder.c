#include "h2_web_platform_internal.h"

#include <emscripten.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define H2_WEB_VIDEO_MAX_PENDING 8u

struct h2_pal_video_decoder_frame {
  struct h2_pal_video_decoder_frame *next;
  uint8_t *pixels;
  size_t bytes;
  uint32_t width;
  uint32_t height;
  int64_t pts_us;
  int64_t duration_us;
};

struct h2_pal_video_decoder_session {
  h2_pal_mem_api_t allocator;
  h2_pal_video_decoder_frame_t *head;
  h2_pal_video_decoder_frame_t *tail;
  h2_pal_video_decoder_frame_t *acquired;
  uint8_t *codec_config;
  size_t codec_config_size;
  uint32_t width;
  uint32_t height;
  size_t queued;
  int configured;
  int eos_submitted;
  int eos_reached;
  int failed;
};

EM_ASYNC_JS(int, h2_web_video_configure_js,
            (uintptr_t address, const char *codec, uint32_t width,
             uint32_t height), {
  if (typeof VideoDecoder === 'undefined')
    return -3;
  const config = {
    codec : UTF8ToString(codec),
    codedWidth : width,
    codedHeight : height,
    optimizeForLatency : true,
    hardwareAcceleration : 'no-preference',
  };
  try {
    const support = await VideoDecoder.isConfigSupported(config);
    if (!support.supported)
      return -3;
    const entries = Module['h2WebVideoDecoders'] ||= new Map();
    const entry = {decoder : null, pending : new Set(), alive : true};
    entry.decoder = new VideoDecoder({
      output(frame) {
        if (!entry.alive || entries.get(address) !== entry) {
          frame.close();
          return;
  }
  const options = {format : 'RGBA'};
  let size;
  try { size = frame.allocationSize(options); }
  catch(error) {
    frame.close();
    Module['_h2_web_video_error'](address);
    return;
  }
  const copy =
      (async() =>
                 {
                   const data = new Uint8Array(size);
                   const layout = await frame.copyTo(data, options);
                   if (!entry.alive || entries.get(address) !== entry)
                     return;
                   const pointer =
                       Module['_h2_web_video_temp_alloc'](data.byteLength);
                   if (!pointer) {
                     Module['_h2_web_video_error'](address);
                     return;
                   }
                   HEAPU8.set(data, pointer);
                   Module['_h2_web_video_output'](
                       address, pointer, data.byteLength, frame.displayWidth,
                       frame.displayHeight, layout[0].offset, layout[0].stride,
                       Number(frame.timestamp || 0),
                       Number(frame.duration || 0));
                   Module['_h2_web_video_temp_free'](pointer);
                 })()
          .catch((error) =>
                           {
                             console.error('WebCodecs video output failed',
                                           error);
                             Module['_h2_web_video_error'](address);
                           })
          .finally(() => {
            frame.close();
            entry.pending.delete(copy);
          });
  entry.pending.add(copy);
      },
      error(error) {
  console.error('WebCodecs video decoder failed', error);
  if (entry.alive && entries.get(address) === entry) {
    Module['_h2_web_video_error'](address);
  }
      },
});
entry.decoder.configure(config);
entries.set(address, entry);
return 0;
}
catch(error) {
  console.error('WebCodecs video configure failed', error);
  return error && error.name === 'NotSupportedError' ? -3 : -4;
}
});

EM_JS(int, h2_web_video_load_js, (uintptr_t address), {
  const entry = Module['h2WebVideoDecoders']?.get(address);
  return entry && entry.alive ? entry.decoder.decodeQueueSize : -1;
});

EM_JS(int, h2_web_video_submit_js,
      (uintptr_t address, const void *data, size_t size,
       const void *config_data, size_t config_size, int is_key, double pts_us,
       double duration_us),
      {
        const entry = Module['h2WebVideoDecoders']?.get(address);
        if (!entry || !entry.alive)
          return -7;
        try {
          const prefix = is_key ? config_size : 0;
          const bytes = new Uint8Array(prefix + size);
          if (prefix)
            bytes.set(HEAPU8.subarray(config_data, config_data + config_size));
          bytes.set(HEAPU8.subarray(data, data + size), prefix);
          entry.decoder.decode(new EncodedVideoChunk({
            type : is_key ? 'key' : 'delta',
            timestamp : pts_us,
            duration : duration_us > 0 ? duration_us : undefined,
            data : bytes,
          }));
          return 0;
        }
        catch(error) {
          console.error('WebCodecs video submit failed', error);
          return error && error.name === 'DataError' ? -15 : -4;
        }
      });

EM_ASYNC_JS(int, h2_web_video_flush_js, (uintptr_t address), {
  const entry = Module['h2WebVideoDecoders']?.get(address);
  if (!entry || !entry.alive)
    return -7;
  try {
    await entry.decoder.flush();
    await Promise.all(Array.from(entry.pending));
    return 0;
  }
  catch(error) {
    console.error('WebCodecs video flush failed', error);
    return -4;
  }
});

EM_JS(void, h2_web_video_drop_js, (uintptr_t address), {
  const entries = Module['h2WebVideoDecoders'];
  const entry = entries?.get(address);
  if (!entry)
    return;
  entry.alive = false;
  try { entry.decoder.close(); }
  catch(error) {}
  entries.delete(address);
});

static void h2_web_video_free_frames(h2_pal_video_decoder_session_t *session) {
  h2_pal_video_decoder_frame_t *frame = session->head;
  while (frame != NULL) {
    h2_pal_video_decoder_frame_t *next = frame->next;
    h2_pal_mem_free(&session->allocator, frame->pixels);
    h2_pal_mem_free(&session->allocator, frame);
    frame = next;
  }
  session->head = NULL;
  session->tail = NULL;
  session->queued = 0u;
}

EMSCRIPTEN_KEEPALIVE uintptr_t h2_web_video_temp_alloc(size_t size) {
  return (uintptr_t)malloc(size);
}

EMSCRIPTEN_KEEPALIVE void h2_web_video_temp_free(uintptr_t address) {
  free((void *)address);
}

EMSCRIPTEN_KEEPALIVE void h2_web_video_error(uintptr_t address) {
  h2_pal_video_decoder_session_t *session =
      (h2_pal_video_decoder_session_t *)address;
  if (session != NULL)
    session->failed = 1;
}

EMSCRIPTEN_KEEPALIVE void
h2_web_video_output(uintptr_t address, const uint8_t *rgba, size_t rgba_size,
                    uint32_t width, uint32_t height, size_t offset,
                    size_t stride, double pts_us, double duration_us) {
  h2_pal_video_decoder_session_t *session =
      (h2_pal_video_decoder_session_t *)address;
  if (session == NULL || !session->configured || session->failed ||
      width != session->width || height != session->height || width == 0u ||
      height == 0u || (size_t)width > SIZE_MAX / (size_t)height ||
      (size_t)width > SIZE_MAX / 4u || stride < (size_t)width * 4u ||
      offset > rgba_size || rgba_size - offset < (size_t)width * 4u ||
      (size_t)(height - 1u) >
          (rgba_size - offset - (size_t)width * 4u) / stride) {
    if (session != NULL)
      session->failed = 1;
    return;
  }
  const size_t pixels = (size_t)width * height;
  if (pixels > SIZE_MAX / sizeof(uint16_t) ||
      session->queued >= H2_WEB_VIDEO_MAX_PENDING) {
    session->failed = 1;
    return;
  }
  h2_pal_video_decoder_frame_t *frame =
      h2_pal_mem_alloc(&session->allocator, sizeof(*frame));
  uint16_t *output =
      h2_pal_mem_alloc(&session->allocator, pixels * sizeof(*output));
  if (frame == NULL || output == NULL) {
    h2_pal_mem_free(&session->allocator, frame);
    h2_pal_mem_free(&session->allocator, output);
    session->failed = 1;
    return;
  }
  for (size_t index = 0u; index < pixels; ++index) {
    const size_t source =
        offset + (index / width) * stride + (index % width) * 4u;
    const uint8_t red = rgba[source];
    const uint8_t green = rgba[source + 1u];
    const uint8_t blue = rgba[source + 2u];
    output[index] =
        (uint16_t)(((uint16_t)(red & 0xf8u) << 8u) |
                   ((uint16_t)(green & 0xfcu) << 3u) | ((uint16_t)blue >> 3u));
  }
  *frame = (h2_pal_video_decoder_frame_t){
      .pixels = (uint8_t *)output,
      .bytes = pixels * sizeof(*output),
      .width = width,
      .height = height,
      .pts_us = (int64_t)pts_us,
      .duration_us = (int64_t)duration_us,
  };
  if (session->tail == NULL)
    session->head = frame;
  else
    session->tail->next = frame;
  session->tail = frame;
  ++session->queued;
}

static int h2_web_h264_is_key(const uint8_t *data, size_t size) {
  for (size_t index = 0u; index + 4u < size; ++index) {
    size_t nal = 0u;
    if (data[index] == 0u && data[index + 1u] == 0u && data[index + 2u] == 1u)
      nal = index + 3u;
    else if (index + 4u < size && data[index] == 0u && data[index + 1u] == 0u &&
             data[index + 2u] == 0u && data[index + 3u] == 1u)
      nal = index + 4u;
    if (nal != 0u && nal < size && (data[nal] & 0x1fu) == 5u)
      return 1;
  }
  return 0;
}

static void h2_web_h264_codec(const uint8_t *data, size_t size,
                              char codec[12]) {
  (void)strcpy(codec, "avc1.42E01E");
  for (size_t index = 0u; index + 7u < size; ++index) {
    size_t nal = 0u;
    if (data[index] == 0u && data[index + 1u] == 0u && data[index + 2u] == 1u)
      nal = index + 3u;
    else if (data[index] == 0u && data[index + 1u] == 0u &&
             data[index + 2u] == 0u && data[index + 3u] == 1u)
      nal = index + 4u;
    if (nal != 0u && nal + 3u < size && (data[nal] & 0x1fu) == 7u) {
      static const char hex[] = "0123456789ABCDEF";
      codec[5] = hex[data[nal + 1u] >> 4u];
      codec[6] = hex[data[nal + 1u] & 0x0fu];
      codec[7] = hex[data[nal + 2u] >> 4u];
      codec[8] = hex[data[nal + 2u] & 0x0fu];
      codec[9] = hex[data[nal + 3u] >> 4u];
      codec[10] = hex[data[nal + 3u] & 0x0fu];
      codec[11] = '\0';
      return;
    }
  }
}

static h2_pal_result_t
h2_web_video_open(void *user, const h2_video_decoder_config_t *config,
                  h2_pal_video_decoder_session_t **out_session) {
  (void)user;
  if (config->preferred_format != H2_VIDEO_PIXEL_FORMAT_UNSPECIFIED &&
      config->preferred_format != H2_VIDEO_PIXEL_FORMAT_RGB565)
    return H2_PAL_ERR_UNSUPPORTED;
  h2_pal_video_decoder_session_t *session =
      h2_pal_mem_alloc(config->frame_allocator, sizeof(*session));
  if (session == NULL)
    return H2_PAL_ERR_NO_MEMORY;
  memset(session, 0, sizeof(*session));
  session->allocator = *config->frame_allocator;
  *out_session = session;
  return H2_PAL_OK;
}

static h2_pal_result_t
h2_web_video_configure(void *user, h2_pal_video_decoder_session_t *session,
                       const h2_video_decoder_stream_config_t *config) {
  (void)user;
  if (session->configured || session->acquired != NULL)
    return H2_PAL_ERR_INVALID_STATE;
  if (config->codec != H2_VIDEO_CODEC_H264 ||
      config->bitstream_format != H2_VIDEO_BITSTREAM_H264_ANNEX_B)
    return H2_PAL_ERR_UNSUPPORTED;
  uint8_t *codec_config =
      h2_pal_mem_alloc(&session->allocator, config->codec_config_size);
  if (codec_config == NULL)
    return H2_PAL_ERR_NO_MEMORY;
  memcpy(codec_config, config->codec_config, config->codec_config_size);
  char codec[12];
  h2_web_h264_codec(codec_config, config->codec_config_size, codec);
  const int result = h2_web_video_configure_js(
      (uintptr_t)session, codec, config->coded_width, config->coded_height);
  if (result != H2_PAL_OK) {
    h2_pal_mem_free(&session->allocator, codec_config);
    return (h2_pal_result_t)result;
  }
  session->codec_config = codec_config;
  session->codec_config_size = config->codec_config_size;
  session->width = config->visible_width;
  session->height = config->visible_height;
  session->configured = 1;
  return H2_PAL_OK;
}

static h2_pal_result_t
h2_web_video_submit(void *user, h2_pal_video_decoder_session_t *session,
                    const h2_video_decoder_packet_t *packet) {
  (void)user;
  if (!session->configured || session->eos_submitted)
    return H2_PAL_ERR_INVALID_STATE;
  if (session->failed)
    return H2_PAL_ERR_IO;
  if ((packet->flags & H2_VIDEO_DECODER_PACKET_END_OF_STREAM) != 0u) {
    session->eos_submitted = 1;
    const int result = h2_web_video_flush_js((uintptr_t)session);
    if (result == H2_PAL_OK)
      session->eos_reached = 1;
    else
      session->failed = 1;
    return (h2_pal_result_t)result;
  }
  const int load = h2_web_video_load_js((uintptr_t)session);
  if (load < 0)
    return H2_PAL_ERR_INVALID_STATE;
  if (session->queued + (size_t)load >= H2_WEB_VIDEO_MAX_PENDING)
    return H2_PAL_ERR_WOULD_BLOCK;
  const int is_key = h2_web_h264_is_key(packet->data, packet->size);
  const int result = h2_web_video_submit_js(
      (uintptr_t)session, packet->data, packet->size, session->codec_config,
      session->codec_config_size, is_key, (double)packet->pts_us,
      (double)packet->duration_us);
  if (result == H2_PAL_OK)
    emscripten_sleep(0u);
  return (h2_pal_result_t)result;
}

static h2_pal_result_t
h2_web_video_acquire(void *user, h2_pal_video_decoder_session_t *session,
                     uint32_t timeout_ms,
                     h2_pal_video_decoder_frame_t **out_frame) {
  (void)user;
  if (!session->configured || session->acquired != NULL)
    return H2_PAL_ERR_INVALID_STATE;
  const double deadline = emscripten_get_now() + timeout_ms;
  while (session->head == NULL && !session->failed &&
         !(session->eos_reached && session->queued == 0u)) {
    if (timeout_ms == 0u || emscripten_get_now() >= deadline)
      return timeout_ms == 0u ? H2_PAL_ERR_WOULD_BLOCK : H2_PAL_ERR_TIMEOUT;
    emscripten_sleep(1u);
  }
  if (session->failed)
    return H2_PAL_ERR_IO;
  if (session->head == NULL)
    return H2_PAL_EXIT;
  session->acquired = session->head;
  session->head = session->head->next;
  session->acquired->next = NULL;
  if (session->head == NULL)
    session->tail = NULL;
  --session->queued;
  *out_frame = session->acquired;
  return H2_PAL_OK;
}

static h2_pal_result_t
h2_web_video_info(void *user, h2_pal_video_decoder_session_t *session,
                  h2_pal_video_decoder_frame_t *frame,
                  h2_video_frame_info_t *out_info) {
  (void)user;
  if (session->acquired != frame)
    return H2_PAL_ERR_INVALID_ARG;
  *out_info = (h2_video_frame_info_t){
      .format = H2_VIDEO_PIXEL_FORMAT_RGB565,
      .width = frame->width,
      .height = frame->height,
      .planes = {{frame->pixels, frame->bytes,
                  (size_t)frame->width * sizeof(uint16_t)}},
      .plane_count = 1u,
      .pts_us = frame->pts_us,
      .duration_us = frame->duration_us,
  };
  return H2_PAL_OK;
}

static h2_pal_result_t
h2_web_video_release(void *user, h2_pal_video_decoder_session_t *session,
                     h2_pal_video_decoder_frame_t *frame) {
  (void)user;
  if (session->acquired != frame)
    return H2_PAL_ERR_INVALID_ARG;
  h2_pal_mem_free(&session->allocator, frame->pixels);
  h2_pal_mem_free(&session->allocator, frame);
  session->acquired = NULL;
  return H2_PAL_OK;
}

static h2_pal_result_t
h2_web_video_reset(void *user, h2_pal_video_decoder_session_t *session) {
  (void)user;
  if (session->acquired != NULL)
    return H2_PAL_ERR_INVALID_STATE;
  h2_web_video_drop_js((uintptr_t)session);
  h2_web_video_free_frames(session);
  h2_pal_mem_free(&session->allocator, session->codec_config);
  session->codec_config = NULL;
  session->codec_config_size = 0u;
  session->configured = 0;
  session->eos_submitted = 0;
  session->eos_reached = 0;
  session->failed = 0;
  return H2_PAL_OK;
}

static h2_pal_result_t
h2_web_video_close(void *user, h2_pal_video_decoder_session_t *session) {
  (void)user;
  if (session->acquired != NULL)
    return H2_PAL_ERR_INVALID_STATE;
  (void)h2_web_video_reset(NULL, session);
  h2_pal_mem_free(&session->allocator, session);
  return H2_PAL_OK;
}

static const h2_pal_video_decoder_vtable_t h2_web_video_vtable = {
    .open = h2_web_video_open,
    .configure = h2_web_video_configure,
    .submit_packet = h2_web_video_submit,
    .acquire_frame = h2_web_video_acquire,
    .frame_get_info = h2_web_video_info,
    .release_frame = h2_web_video_release,
    .reset = h2_web_video_reset,
    .close = h2_web_video_close,
};

void h2_web_platform_video_decoder_init(h2_web_platform_t *platform) {
  platform->video_decoder_api = (h2_pal_video_decoder_api_t){
      .user = platform,
      .vtable = &h2_web_video_vtable,
  };
}
