#include "h2_web_platform_internal.h"

#include <emscripten.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define H2_WEB_AUDIO_MAX_PENDING 16u

struct h2_pal_audio_decoder_frame {
  struct h2_pal_audio_decoder_frame *next;
  int16_t *samples;
  size_t bytes;
  uint32_t sample_rate_hz;
  uint32_t samples_per_channel;
  uint8_t channels;
  int64_t pts_us;
  int64_t duration_us;
};

struct h2_pal_audio_decoder_session {
  h2_pal_mem_api_t allocator;
  h2_pal_audio_decoder_frame_t *head;
  h2_pal_audio_decoder_frame_t *tail;
  h2_pal_audio_decoder_frame_t *acquired;
  size_t queued;
  int configured;
  int eos_submitted;
  int eos_reached;
  int failed;
};

EM_ASYNC_JS(int, h2_web_audio_decoder_configure_js,
            (uintptr_t address, uint32_t sample_rate, uint32_t channels,
             const void *description, size_t description_size), {
  if (typeof AudioDecoder === 'undefined')
    return -3;
  const config = {
    codec : 'mp4a.40.2',
    sampleRate : sample_rate,
    numberOfChannels : channels,
    description : HEAPU8.slice(description, description + description_size),
  };
  try {
    const support = await AudioDecoder.isConfigSupported(config);
    if (!support.supported)
      return -3;
    const entries = Module['h2WebAudioDecoders'] ||= new Map();
    const entry = {decoder : null, pending : new Set(), alive : true};
    entry.decoder = new AudioDecoder({
      output(audio) {
        if (!entry.alive || entries.get(address) !== entry) {
          audio.close();
          return;
  }
  const options = {planeIndex : 0, format : 's16'};
  let size;
  try { size = audio.allocationSize(options); }
  catch(error) {
    audio.close();
    Module['_h2_web_audio_decoder_error'](address);
    return;
  }
  const copy =
      (async() =>
           {
             const data = new Uint8Array(size);
             await audio.copyTo(data, options);
             if (!entry.alive || entries.get(address) !== entry)
               return;
             const pointer =
                 Module['_h2_web_audio_decoder_temp_alloc'](data.byteLength);
             if (!pointer) {
               Module['_h2_web_audio_decoder_error'](address);
               return;
             }
             HEAPU8.set(data, pointer);
             Module['_h2_web_audio_decoder_output'](
                 address, pointer, data.byteLength, audio.sampleRate,
                 audio.numberOfFrames, audio.numberOfChannels,
                 Number(audio.timestamp || 0), Number(audio.duration || 0));
             Module['_h2_web_audio_decoder_temp_free'](pointer);
           })()
          .catch((error) =>
                           {
                             console.error('WebCodecs audio output failed',
                                           error);
                             Module['_h2_web_audio_decoder_error'](address);
                           })
          .finally(() => {
            audio.close();
            entry.pending.delete(copy);
          });
  entry.pending.add(copy);
      },
      error(error) {
  console.error('WebCodecs audio decoder failed', error);
  if (entry.alive && entries.get(address) === entry) {
    Module['_h2_web_audio_decoder_error'](address);
  }
      },
});
entry.decoder.configure(config);
entries.set(address, entry);
return 0;
}
catch(error) {
  console.error('WebCodecs audio configure failed', error);
  return error && error.name === 'NotSupportedError' ? -3 : -4;
}
});

EM_JS(int, h2_web_audio_decoder_load_js, (uintptr_t address), {
  const entry = Module['h2WebAudioDecoders']?.get(address);
  return entry && entry.alive ? entry.decoder.decodeQueueSize : -1;
});

EM_JS(int, h2_web_audio_decoder_submit_js,
      (uintptr_t address, const void *data, size_t size, double pts_us,
       double duration_us),
      {
        const entry = Module['h2WebAudioDecoders']?.get(address);
        if (!entry || !entry.alive)
          return -7;
        try {
          entry.decoder.decode(new EncodedAudioChunk({
            type : 'key',
            timestamp : pts_us,
            duration : duration_us > 0 ? duration_us : undefined,
            data : HEAPU8.slice(data, data + size),
          }));
          return 0;
        }
        catch(error) {
          console.error('WebCodecs audio submit failed', error);
          return error && error.name === 'DataError' ? -15 : -4;
        }
      });

EM_ASYNC_JS(int, h2_web_audio_decoder_flush_js, (uintptr_t address), {
  const entry = Module['h2WebAudioDecoders']?.get(address);
  if (!entry || !entry.alive)
    return -7;
  try {
    await entry.decoder.flush();
    await Promise.all(Array.from(entry.pending));
    return 0;
  }
  catch(error) {
    console.error('WebCodecs audio flush failed', error);
    return -4;
  }
});

EM_JS(void, h2_web_audio_decoder_drop_js, (uintptr_t address), {
  const entries = Module['h2WebAudioDecoders'];
  const entry = entries?.get(address);
  if (!entry)
    return;
  entry.alive = false;
  try { entry.decoder.close(); }
  catch(error) {}
  entries.delete(address);
});

static void
h2_web_audio_decoder_free_frames(h2_pal_audio_decoder_session_t *session) {
  h2_pal_audio_decoder_frame_t *frame = session->head;
  while (frame != NULL) {
    h2_pal_audio_decoder_frame_t *next = frame->next;
    h2_pal_mem_free(&session->allocator, frame->samples);
    h2_pal_mem_free(&session->allocator, frame);
    frame = next;
  }
  session->head = NULL;
  session->tail = NULL;
  session->queued = 0u;
}

EMSCRIPTEN_KEEPALIVE uintptr_t h2_web_audio_decoder_temp_alloc(size_t size) {
  return (uintptr_t)malloc(size);
}

EMSCRIPTEN_KEEPALIVE void h2_web_audio_decoder_temp_free(uintptr_t address) {
  free((void *)address);
}

EMSCRIPTEN_KEEPALIVE void h2_web_audio_decoder_error(uintptr_t address) {
  h2_pal_audio_decoder_session_t *session =
      (h2_pal_audio_decoder_session_t *)address;
  if (session != NULL)
    session->failed = 1;
}

EMSCRIPTEN_KEEPALIVE void
h2_web_audio_decoder_output(uintptr_t address, const uint8_t *samples,
                            size_t bytes, uint32_t sample_rate_hz,
                            uint32_t samples_per_channel, uint32_t channels,
                            double pts_us, double duration_us) {
  h2_pal_audio_decoder_session_t *session =
      (h2_pal_audio_decoder_session_t *)address;
  if (session == NULL || !session->configured || session->failed ||
      sample_rate_hz == 0u || samples_per_channel == 0u || channels == 0u ||
      channels > UINT8_MAX ||
      (size_t)samples_per_channel > SIZE_MAX / channels ||
      (size_t)samples_per_channel * channels > SIZE_MAX / sizeof(int16_t) ||
      bytes != (size_t)samples_per_channel * channels * sizeof(int16_t) ||
      session->queued >= H2_WEB_AUDIO_MAX_PENDING) {
    if (session != NULL)
      session->failed = 1;
    return;
  }
  h2_pal_audio_decoder_frame_t *frame =
      h2_pal_mem_alloc(&session->allocator, sizeof(*frame));
  int16_t *copy = h2_pal_mem_alloc(&session->allocator, bytes);
  if (frame == NULL || copy == NULL) {
    h2_pal_mem_free(&session->allocator, frame);
    h2_pal_mem_free(&session->allocator, copy);
    session->failed = 1;
    return;
  }
  memcpy(copy, samples, bytes);
  *frame = (h2_pal_audio_decoder_frame_t){
      .samples = copy,
      .bytes = bytes,
      .sample_rate_hz = sample_rate_hz,
      .samples_per_channel = samples_per_channel,
      .channels = (uint8_t)channels,
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

static h2_pal_result_t
h2_web_audio_decoder_open(void *user, const h2_audio_decoder_config_t *config,
                          h2_pal_audio_decoder_session_t **out_session) {
  (void)user;
  if (config->preferred_format != 0 &&
      config->preferred_format != H2_AUDIO_SAMPLE_S16LE)
    return H2_PAL_ERR_UNSUPPORTED;
  h2_pal_audio_decoder_session_t *session =
      h2_pal_mem_alloc(config->pcm_allocator, sizeof(*session));
  if (session == NULL)
    return H2_PAL_ERR_NO_MEMORY;
  memset(session, 0, sizeof(*session));
  session->allocator = *config->pcm_allocator;
  *out_session = session;
  return H2_PAL_OK;
}

static h2_pal_result_t
h2_web_audio_decoder_configure(void *user,
                               h2_pal_audio_decoder_session_t *session,
                               const h2_audio_decoder_stream_config_t *config) {
  (void)user;
  if (session->configured || session->acquired != NULL)
    return H2_PAL_ERR_INVALID_STATE;
  if (config->codec != H2_AUDIO_CODEC_AAC_LC ||
      config->bitstream_format != H2_AUDIO_BITSTREAM_AAC_RAW)
    return H2_PAL_ERR_UNSUPPORTED;
  const int result = h2_web_audio_decoder_configure_js(
      (uintptr_t)session, config->sample_rate_hz, config->channels,
      config->codec_config, config->codec_config_size);
  if (result != H2_PAL_OK)
    return (h2_pal_result_t)result;
  session->configured = 1;
  return H2_PAL_OK;
}

static h2_pal_result_t
h2_web_audio_decoder_submit(void *user, h2_pal_audio_decoder_session_t *session,
                            const h2_audio_decoder_packet_t *packet) {
  (void)user;
  if (!session->configured || session->eos_submitted)
    return H2_PAL_ERR_INVALID_STATE;
  if (session->failed)
    return H2_PAL_ERR_IO;
  if ((packet->flags & H2_AUDIO_DECODER_PACKET_END_OF_STREAM) != 0u) {
    session->eos_submitted = 1;
    const int result = h2_web_audio_decoder_flush_js((uintptr_t)session);
    if (result == H2_PAL_OK)
      session->eos_reached = 1;
    else
      session->failed = 1;
    return (h2_pal_result_t)result;
  }
  const int load = h2_web_audio_decoder_load_js((uintptr_t)session);
  if (load < 0)
    return H2_PAL_ERR_INVALID_STATE;
  if (session->queued + (size_t)load >= H2_WEB_AUDIO_MAX_PENDING)
    return H2_PAL_ERR_WOULD_BLOCK;
  const int result = h2_web_audio_decoder_submit_js(
      (uintptr_t)session, packet->data, packet->size, (double)packet->pts_us,
      (double)packet->duration_us);
  if (result == H2_PAL_OK)
    emscripten_sleep(0u);
  return (h2_pal_result_t)result;
}

static h2_pal_result_t h2_web_audio_decoder_acquire(
    void *user, h2_pal_audio_decoder_session_t *session, uint32_t timeout_ms,
    h2_pal_audio_decoder_frame_t **out_frame) {
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
h2_web_audio_decoder_info(void *user, h2_pal_audio_decoder_session_t *session,
                          h2_pal_audio_decoder_frame_t *frame,
                          h2_audio_decoder_frame_info_t *out_info) {
  (void)user;
  if (session->acquired != frame)
    return H2_PAL_ERR_INVALID_ARG;
  *out_info = (h2_audio_decoder_frame_info_t){
      .data = frame->samples,
      .bytes = frame->bytes,
      .sample_rate_hz = frame->sample_rate_hz,
      .samples_per_channel = frame->samples_per_channel,
      .channels = frame->channels,
      .sample_format = H2_AUDIO_SAMPLE_S16LE,
      .pts_us = frame->pts_us,
      .duration_us = frame->duration_us,
  };
  return H2_PAL_OK;
}

static h2_pal_result_t
h2_web_audio_decoder_release(void *user,
                             h2_pal_audio_decoder_session_t *session,
                             h2_pal_audio_decoder_frame_t *frame) {
  (void)user;
  if (session->acquired != frame)
    return H2_PAL_ERR_INVALID_ARG;
  h2_pal_mem_free(&session->allocator, frame->samples);
  h2_pal_mem_free(&session->allocator, frame);
  session->acquired = NULL;
  return H2_PAL_OK;
}

static h2_pal_result_t
h2_web_audio_decoder_reset(void *user,
                           h2_pal_audio_decoder_session_t *session) {
  (void)user;
  if (session->acquired != NULL)
    return H2_PAL_ERR_INVALID_STATE;
  h2_web_audio_decoder_drop_js((uintptr_t)session);
  h2_web_audio_decoder_free_frames(session);
  session->configured = 0;
  session->eos_submitted = 0;
  session->eos_reached = 0;
  session->failed = 0;
  return H2_PAL_OK;
}

static h2_pal_result_t
h2_web_audio_decoder_close(void *user,
                           h2_pal_audio_decoder_session_t *session) {
  (void)user;
  if (session->acquired != NULL)
    return H2_PAL_ERR_INVALID_STATE;
  (void)h2_web_audio_decoder_reset(NULL, session);
  h2_pal_mem_free(&session->allocator, session);
  return H2_PAL_OK;
}

static const h2_pal_audio_decoder_vtable_t h2_web_audio_decoder_vtable = {
    .open = h2_web_audio_decoder_open,
    .configure = h2_web_audio_decoder_configure,
    .submit_packet = h2_web_audio_decoder_submit,
    .acquire_frame = h2_web_audio_decoder_acquire,
    .frame_get_info = h2_web_audio_decoder_info,
    .release_frame = h2_web_audio_decoder_release,
    .reset = h2_web_audio_decoder_reset,
    .close = h2_web_audio_decoder_close,
};

void h2_web_platform_audio_decoder_init(h2_web_platform_t *platform) {
  platform->audio_decoder_api = (h2_pal_audio_decoder_api_t){
      .user = platform,
      .vtable = &h2_web_audio_decoder_vtable,
  };
}
