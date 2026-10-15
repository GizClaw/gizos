#include "h2_web_main_thread.h"
#include "h2_web_platform_internal.h"

#include <emscripten.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// Submission stops at the soft limit; output already inside the decoder may
// still land up to the hard limit.
#define H2_WEB_AUDIO_MAX_PENDING 16u
#define H2_WEB_AUDIO_MAX_QUEUED (3u * H2_WEB_AUDIO_MAX_PENDING)

struct h2_pal_audio_decoder_frame {
  struct h2_pal_audio_decoder_frame *next;
  int16_t *samples;
  int pcm_owned;
  size_t bytes;
  uint32_t sample_rate_hz;
  uint32_t samples_per_channel;
  uint8_t channels;
  int64_t pts_us;
  int64_t duration_us;
};

struct h2_pal_audio_decoder_session {
  h2_web_platform_t *platform;
  h2_web_async_t *acquire_op;
  h2_pal_mem_api_t allocator;
  h2_pal_audio_decoder_frame_t *head;
  h2_pal_audio_decoder_frame_t *tail;
  h2_pal_audio_decoder_frame_t *acquired;
  size_t queued;
  int configured;
  int eos_submitted;
  int eos_reached;
  int failed;
  h2_pal_result_t failure_result;
};

/* clang-format off */
EM_JS(void, h2_web_audio_decoder_configure_js,
      (void *context, h2_web_main_result_t *result, h2_web_main_completion_t *completion), {
  h2WebMain(context, result, completion, ["u32", "u32", "u32", "u32", "u32", "pointer", "u32"], "i32",
    (platform_address, address, op_id, sample_rate, channels, description, description_size) => {
  if (typeof AudioDecoder === 'undefined')
    return -3;
  const config = {
    codec : 'mp4a.40.2',
    sampleRate : sample_rate,
    numberOfChannels : channels,
    description : HEAPU8.slice(description, description + description_size),
  };
  (async () => { try {
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
} })().then((result) => {
  // A caller that gave up (task cancel) never learns of this decoder.
  if (!Module['_h2_web_async_complete'](platform_address, op_id, result) &&
      result === 0) {
    const entries = Module['h2WebAudioDecoders'];
    const entry = entries?.get(address);
    if (entry) {
      entry.alive = false;
      try { entry.decoder.close(); } catch (_) {}
      entries.delete(address);
    }
  }
});
return 0;
});
});
/* clang-format on */

/* clang-format off */
EM_JS(void, h2_web_audio_decoder_load_js,
      (void *context, h2_web_main_result_t *result, h2_web_main_completion_t *completion), {
  h2WebMain(context, result, completion, ["u32"], "i32",
    (address) => {
  const entry = Module['h2WebAudioDecoders']?.get(address);
  return entry && entry.alive ? entry.decoder.decodeQueueSize : -1;
});
});
/* clang-format on */

/* clang-format off */
EM_JS(void, h2_web_audio_decoder_submit_js,
      (void *context, h2_web_main_result_t *result, h2_web_main_completion_t *completion), {
  h2WebMain(context, result, completion, ["u32", "pointer", "u32", "double", "double"], "i32",
    (address, data, size, pts_us, duration_us) => {
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
});
/* clang-format on */

/* clang-format off */
EM_JS(void, h2_web_audio_decoder_flush_js,
      (void *context, h2_web_main_result_t *result, h2_web_main_completion_t *completion), {
  h2WebMain(context, result, completion, ["u32", "u32", "u32"], "i32",
    (platform_address, address, op_id) => {
  const entry = Module['h2WebAudioDecoders']?.get(address);
  if (!entry || !entry.alive)
    return -7;
  (async () => {
    try {
      await entry.decoder.flush();
      await Promise.all(Array.from(entry.pending));
      return 0;
    }
    catch(error) {
      if (!entry.alive) return -10;
      console.error('WebCodecs audio flush failed', error);
      return -4;
    }
  })().then((result) => Module['_h2_web_async_complete'](
      platform_address, op_id, result));
  return 0;
});
});
/* clang-format on */

/* clang-format off */
EM_JS(void, h2_web_audio_decoder_drop_js,
      (void *context, h2_web_main_result_t *result, h2_web_main_completion_t *completion), {
  h2WebMain(context, result, completion, ["u32"], null,
    (address) => {
  const entries = Module['h2WebAudioDecoders'];
  const entry = entries?.get(address);
  if (!entry)
    return;
  entry.alive = false;
  try { entry.decoder.close(); }
  catch(error) {}
  entries.delete(address);
});
});
/* clang-format on */

static void
h2_web_audio_decoder_free_frames(h2_pal_audio_decoder_session_t *session) {
  H2_WEB_STATE_GUARD();
  h2_pal_audio_decoder_frame_t *frame = session->head;
  while (frame != NULL) {
    h2_pal_audio_decoder_frame_t *next = frame->next;
    if (frame->pcm_owned) h2_pal_mem_free(&session->allocator, frame->samples);
    else free(frame->samples);
    free(frame);
    frame = next;
  }
  session->head = NULL;
  session->tail = NULL;
  session->queued = 0u;
}

EMSCRIPTEN_KEEPALIVE uintptr_t h2_web_audio_decoder_temp_alloc(size_t size) {
  H2_WEB_STATE_GUARD();
  return (uintptr_t)malloc(size);
}

EMSCRIPTEN_KEEPALIVE void h2_web_audio_decoder_temp_free(uintptr_t address) {
  H2_WEB_STATE_GUARD();
  free((void *)address);
}

static void
h2_web_audio_decoder_wake(h2_pal_audio_decoder_session_t *session) {
  H2_WEB_STATE_GUARD();
  if (session->acquire_op != NULL)
    h2_web_async_signal(session->platform, session->acquire_op, H2_PAL_OK);
}

EMSCRIPTEN_KEEPALIVE void h2_web_audio_decoder_error(uintptr_t address) {
  H2_WEB_STATE_GUARD();
  h2_pal_audio_decoder_session_t *session =
      (h2_pal_audio_decoder_session_t *)address;
  if (session != NULL) {
    session->failed = 1;
    session->failure_result = H2_PAL_ERR_IO;
    h2_web_audio_decoder_wake(session);
  }
}

EMSCRIPTEN_KEEPALIVE void
h2_web_audio_decoder_output(uintptr_t address, const uint8_t *samples,
                            size_t bytes, uint32_t sample_rate_hz,
                            uint32_t samples_per_channel, uint32_t channels,
                            double pts_us, double duration_us) {
  H2_WEB_STATE_GUARD();
  h2_pal_audio_decoder_session_t *session =
      (h2_pal_audio_decoder_session_t *)address;
  if (session == NULL || session->failed) return;
  if (!session->configured ||
      sample_rate_hz == 0u || samples_per_channel == 0u || channels == 0u ||
      channels > UINT8_MAX ||
      (size_t)samples_per_channel > SIZE_MAX / channels ||
      (size_t)samples_per_channel * channels > SIZE_MAX / sizeof(int16_t) ||
      bytes != (size_t)samples_per_channel * channels * sizeof(int16_t) ||
      session->queued >= H2_WEB_AUDIO_MAX_QUEUED) {
    if (session != NULL) {
      session->failed = 1;
      session->failure_result = session->configured
          ? H2_PAL_ERR_FORMAT : H2_PAL_ERR_INVALID_STATE;
      h2_web_audio_decoder_wake(session);
    }
    return;
  }
  /* WebCodecs invokes this bridge on the browser UI thread. Keep its staging
   * private; the caller's potentially blocking PCM allocator runs on acquire. */
  h2_pal_audio_decoder_frame_t *frame = malloc(sizeof(*frame));
  int16_t *copy = malloc(bytes);
  if (frame == NULL || copy == NULL) {
    free(frame);
    free(copy);
    session->failed = 1;
    session->failure_result = H2_PAL_ERR_NO_MEMORY;
    h2_web_audio_decoder_wake(session);
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
  h2_web_audio_decoder_wake(session);
}

static h2_pal_result_t
h2_web_audio_decoder_open(void *user, const h2_audio_decoder_config_t *config,
                          h2_pal_audio_decoder_session_t **out_session) {
  H2_WEB_STATE_GUARD();
  if (config->preferred_format != 0 &&
      config->preferred_format != H2_AUDIO_SAMPLE_S16LE)
    return H2_PAL_ERR_UNSUPPORTED;
  h2_pal_audio_decoder_session_t *session =
      h2_pal_mem_alloc(config->pcm_allocator, sizeof(*session));
  if (session == NULL)
    return H2_PAL_ERR_NO_MEMORY;
  memset(session, 0, sizeof(*session));
  session->allocator = *config->pcm_allocator;
  session->platform = user;
  *out_session = session;
  return H2_PAL_OK;
}

static h2_pal_result_t
h2_web_audio_decoder_configure(void *user,
                               h2_pal_audio_decoder_session_t *session,
                               const h2_audio_decoder_stream_config_t *config) {
  H2_WEB_STATE_GUARD();
  (void)user;
  if (session->configured || session->acquired != NULL)
    return H2_PAL_ERR_INVALID_STATE;
  if (config->codec != H2_AUDIO_CODEC_AAC_LC ||
      config->bitstream_format != H2_AUDIO_BITSTREAM_AAC_RAW)
    return H2_PAL_ERR_UNSUPPORTED;
  if (config->codec_config_size < 2u) return H2_PAL_ERR_FORMAT;
  h2_web_async_t op;
  h2_web_async_begin(session->platform, &op);
  int result = ((int)h2_web_main_call(
                    h2_web_audio_decoder_configure_js,
                    (const void *[]){&(uintptr_t){(uintptr_t)session->platform},
                                     &(uintptr_t){(uintptr_t)session},
                                     &(uint32_t){op.id},
                                     &(uint32_t){config->sample_rate_hz},
                                     &(uint32_t){config->channels},
                                     &(const void *){config->codec_config},
                                     &(size_t){config->codec_config_size}})
                    .i32);
  result = h2_web_async_finish(session->platform, &op, result);
  if (result != H2_PAL_OK)
    return (h2_pal_result_t)result;
  session->configured = 1;
  return H2_PAL_OK;
}

static h2_pal_result_t
h2_web_audio_decoder_submit(void *user, h2_pal_audio_decoder_session_t *session,
                            const h2_audio_decoder_packet_t *packet) {
  H2_WEB_STATE_GUARD();
  (void)user;
  if (!session->configured || session->eos_submitted)
    return H2_PAL_ERR_INVALID_STATE;
  if (session->failed)
    return session->failure_result;
  if ((packet->flags & H2_AUDIO_DECODER_PACKET_END_OF_STREAM) != 0u) {
    session->eos_submitted = 1;
    h2_web_async_t op;
    h2_web_async_begin(session->platform, &op);
    int result = ((int)h2_web_main_call(
                      h2_web_audio_decoder_flush_js,
                      (const void *[]){
                          &(uintptr_t){(uintptr_t)session->platform},
                          &(uintptr_t){(uintptr_t)session}, &(uint32_t){op.id}})
                      .i32);
    result = h2_web_async_finish(session->platform, &op, result);
    if (result == H2_PAL_OK)
      session->eos_reached = 1;
    else {
      session->failed = 1;
      session->failure_result = (h2_pal_result_t)result;
    }
    return (h2_pal_result_t)result;
  }
  const int load =
      ((int)h2_web_main_call(h2_web_audio_decoder_load_js,
                             (const void *[]){&(uintptr_t){(uintptr_t)session}})
           .i32);
  if (load < 0)
    return H2_PAL_ERR_INVALID_STATE;
  if (session->queued + (size_t)load >= H2_WEB_AUDIO_MAX_PENDING)
    return H2_PAL_ERR_WOULD_BLOCK;
  const int result =
      ((int)h2_web_main_call(
           h2_web_audio_decoder_submit_js,
           (const void *[]){
               &(uintptr_t){(uintptr_t)session}, &(const void *){packet->data},
               &(size_t){packet->size}, &(double){(double)packet->pts_us},
               &(double){(double)packet->duration_us}})
           .i32);
  return (h2_pal_result_t)result;
}

static h2_pal_result_t h2_web_audio_decoder_acquire(
    void *user, h2_pal_audio_decoder_session_t *session, uint32_t timeout_ms,
    h2_pal_audio_decoder_frame_t **out_frame) {
  H2_WEB_STATE_GUARD();
  (void)user;
  if (!session->configured)
    return H2_PAL_ERR_INVALID_STATE;
  if (session->acquired != NULL) return H2_PAL_ERR_WOULD_BLOCK;
  const double deadline = emscripten_get_now() + timeout_ms;
  while (session->head == NULL && !session->failed &&
         !(session->eos_reached && session->queued == 0u)) {
    const double now = emscripten_get_now();
    if (timeout_ms == 0u || now >= deadline)
      return timeout_ms == 0u ? H2_PAL_ERR_WOULD_BLOCK : H2_PAL_ERR_TIMEOUT;
    // Tasks yield until WebCodecs delivers output; other tasks keep running.
    h2_web_async_t op;
    h2_web_async_begin(session->platform, &op);
    session->acquire_op = &op;
    const h2_pal_result_t wait = h2_web_async_wait(
        session->platform, &op, (uint32_t)(deadline - now) + 1u);
    session->acquire_op = NULL;
    h2_web_async_end(session->platform, &op);
    if (wait == H2_PAL_ERR_CLOSED)
      return H2_PAL_ERR_CLOSED;
  }
  if (session->failed)
    return session->failure_result;
  if (session->head == NULL)
    return H2_PAL_EXIT;
  int16_t *pcm = h2_pal_mem_alloc(&session->allocator, session->head->bytes);
  if (pcm == NULL) return H2_PAL_ERR_NO_MEMORY;
  memcpy(pcm, session->head->samples, session->head->bytes);
  free(session->head->samples);
  session->head->samples = pcm;
  session->head->pcm_owned = 1;
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
  H2_WEB_STATE_GUARD();
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
  H2_WEB_STATE_GUARD();
  (void)user;
  if (session->acquired != frame)
    return H2_PAL_ERR_INVALID_ARG;
  h2_pal_mem_free(&session->allocator, frame->samples);
  free(frame);
  session->acquired = NULL;
  return H2_PAL_OK;
}

static h2_pal_result_t
h2_web_audio_decoder_reset(void *user,
                           h2_pal_audio_decoder_session_t *session) {
  H2_WEB_STATE_GUARD();
  (void)user;
  if (session->acquired != NULL)
    return H2_PAL_ERR_INVALID_STATE;
  (void)h2_web_main_call(h2_web_audio_decoder_drop_js,
                         (const void *[]){&(uintptr_t){(uintptr_t)session}});
  h2_web_audio_decoder_free_frames(session);
  session->configured = 0;
  session->eos_submitted = 0;
  session->eos_reached = 0;
  session->failed = 0;
  session->failure_result = H2_PAL_OK;
  return H2_PAL_OK;
}

static h2_pal_result_t
h2_web_audio_decoder_close(void *user,
                           h2_pal_audio_decoder_session_t *session) {
  H2_WEB_STATE_GUARD();
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
  H2_WEB_STATE_GUARD();
  platform->audio_decoder_api = (h2_pal_audio_decoder_api_t){
      .user = platform,
      .vtable = &h2_web_audio_decoder_vtable,
  };
}
