#include "h2_gizclaw_speech.h"
#include "h2_gizclaw_response_internal.h"
#include "h2_gizclaw_service_internal.h"
#include "payload/ai.pb.h"
#include "pb_decode.h"
#include "pb_encode.h"

#include <limits.h>
#include <string.h>

#define SPEECH_FRAME_BYTES 640u

struct h2_gizclaw_speech_context {
  h2_gizclaw_service_t *service;
  const h2_pal_mem_api_t *allocator;
  h2_pal_mutex_t *input_mutex;
  h2_gizclaw_req_t *request; /* Lifetime held until uplink detaches. */
  /* The following input state is protected by input_mutex. */
  h2_gizclaw_pcm_input_t input;
  uint8_t capture_pending[SPEECH_FRAME_BYTES];
  size_t capture_pending_len;
  /* Only the uplink worker sets this, after handing off the frozen tail. */
  atomic_bool producer_done;
  size_t uplink_refs; /* Protected by Service mutex. */
  bool response_seen;
  bool eos_seen;
  h2_pal_result_t frame_error;
};
typedef struct h2_gizclaw_speech_context speech_t;
static const char transcribe_tag, extract_tag;

static bool valid_text(h2_gizclaw_str_t s) {
  if (s.data == NULL)
    return s.len == 0u;
  const uint8_t *p = (const uint8_t *)s.data;
  for (size_t i = 0u; i < s.len;) {
    uint32_t cp = p[i++];
    if (cp == 0u)
      return false;
    if (cp < 0x80u)
      continue;
    unsigned tail;
    uint32_t minimum;
    if (cp >= 0xc2u && cp <= 0xdfu) {
      tail = 1u;
      minimum = 0x80u;
      cp &= 0x1fu;
    } else if (cp >= 0xe0u && cp <= 0xefu) {
      tail = 2u;
      minimum = 0x800u;
      cp &= 0x0fu;
    } else if (cp >= 0xf0u && cp <= 0xf4u) {
      tail = 3u;
      minimum = 0x10000u;
      cp &= 7u;
    } else
      return false;
    if (s.len - i < tail)
      return false;
    while (tail-- != 0u) {
      if ((p[i] & 0xc0u) != 0x80u)
        return false;
      cp = (cp << 6u) | (p[i++] & 0x3fu);
    }
    if (cp < minimum || cp > 0x10ffffu || (cp >= 0xd800u && cp <= 0xdfffu))
      return false;
  }
  return true;
}

static bool copy_text(char *out, size_t capacity, h2_gizclaw_str_t value,
                      bool required) {
  if ((required && value.len == 0u) || value.len >= capacity ||
      !valid_text(value))
    return false;
  if (value.len != 0u)
    memcpy(out, value.data, value.len);
  out[value.len] = '\0';
  return true;
}

/* Called only by the Service uplink worker, never by the network task. */
void h2_gizclaw_speech_uplink_step_internal(h2_gizclaw_service_t *service) {
  const h2_pal_sync_api_t *sync = service->config.sync;
  if (h2_pal_mutex_lock(sync, service->mutex) != H2_PAL_OK)
    return;
  speech_t *speech = atomic_load(&service->speech_request);
  if (speech != NULL)
    ++speech->uplink_refs;
  (void)h2_pal_mutex_unlock(sync, service->mutex);
  if (speech == NULL)
    return;

  h2_pal_result_t rc = h2_pal_mutex_lock(sync, speech->input_mutex);
  if (rc == H2_PAL_OK) {
    if (speech->input.active && !atomic_load(&speech->producer_done)) {
      rc = h2_gizclaw_service_pcm_input_internal(service, &speech->input,
                                                 H2_GIZCLAW_PCM_INPUT_PREPARE,
                                                 NULL, 0u, NULL);
      if (rc == H2_PAL_OK &&
          h2_gizclaw_req_pcm_ready_internal(speech->request) &&
          speech->capture_pending_len == 0u) {
        rc = h2_gizclaw_service_pcm_input_internal(
            service, &speech->input, H2_GIZCLAW_PCM_INPUT_READ,
            speech->capture_pending, sizeof(speech->capture_pending),
            &speech->capture_pending_len);
      }
      if (rc == H2_PAL_OK && speech->capture_pending_len != 0u) {
        rc = h2_gizclaw_req_pcm_write_internal(speech->request,
                                               speech->capture_pending,
                                               speech->capture_pending_len);
        if (rc == H2_PAL_OK)
          speech->capture_pending_len = 0u;
      }
      if (rc != H2_PAL_OK && rc != H2_PAL_ERR_WOULD_BLOCK &&
          rc != H2_PAL_ERR_TIMEOUT) {
        h2_gizclaw_req_pcm_end_internal(speech->request, rc);
        atomic_store(&speech->producer_done, true);
      } else if (speech->input.ended && speech->input.tail_taken &&
                 speech->input.tail_offset == speech->input.tail_len &&
                 speech->capture_pending_len == 0u) {
        h2_gizclaw_req_pcm_end_internal(speech->request, H2_PAL_OK);
        atomic_store(&speech->producer_done, true);
      }
    }
    (void)h2_pal_mutex_unlock(sync, speech->input_mutex);
  } else {
    h2_gizclaw_req_pcm_end_internal(speech->request, rc);
    atomic_store(&speech->producer_done, true);
  }
  (void)h2_pal_mutex_lock(sync, service->mutex);
  --speech->uplink_refs;
  (void)h2_pal_cond_broadcast(sync, service->progress_cond);
  (void)h2_pal_mutex_unlock(sync, service->mutex);
}

static void detach_input(speech_t *speech) {
  h2_gizclaw_service_t *service = speech->service;
  const h2_pal_sync_api_t *sync = service->config.sync;
  (void)h2_pal_mutex_lock(sync, service->mutex);
  speech_t *expected = speech;
  (void)atomic_compare_exchange_strong(&service->speech_request, &expected,
                                       NULL);
  while (speech->uplink_refs != 0u)
    (void)h2_pal_cond_wait(sync, service->progress_cond, service->mutex,
                           H2_PAL_SYNC_WAIT_FOREVER);
  (void)h2_pal_mutex_unlock(sync, service->mutex);
}

static h2_pal_result_t
speech_audio_control(void *context, h2_gizclaw_pcm_input_action_t action) {
  speech_t *speech = context;
  const h2_pal_sync_api_t *sync = speech->service->config.sync;
  h2_pal_result_t rc = h2_pal_mutex_lock(sync, speech->input_mutex);
  if (rc != H2_PAL_OK)
    return rc;
  rc = h2_gizclaw_service_pcm_input_internal(speech->service, &speech->input,
                                             action, NULL, 0u, NULL);
  (void)h2_pal_mutex_unlock(sync, speech->input_mutex);
  return rc;
}

h2_pal_result_t h2_gizclaw_speech_audio_start_internal(void *context) {
  return speech_audio_control(context, H2_GIZCLAW_PCM_INPUT_START);
}

h2_pal_result_t h2_gizclaw_speech_audio_end_internal(void *context) {
  return speech_audio_control(context, H2_GIZCLAW_PCM_INPUT_END);
}

static int speech_frame(void *context,
                        const h2_gizclaw_rpc_stream_event_t *event) {
  speech_t *speech = context;
  if (speech->frame_error != H2_PAL_OK)
    return speech->frame_error;
  if (event == NULL || speech->eos_seen)
    speech->frame_error = H2_PAL_ERR_FORMAT;
  else if (event->has_error)
    speech->frame_error =
        h2_gizclaw_rpc_error_result_internal(event->error_code);
  else if (event->kind == H2_GIZCLAW_RPC_STREAM_RESPONSE &&
           !speech->response_seen)
    speech->response_seen = true;
  else if (event->kind == H2_GIZCLAW_RPC_STREAM_EOS && speech->response_seen &&
           event->input_finished)
    speech->eos_seen = true;
  else
    speech->frame_error = H2_PAL_ERR_FORMAT;
  return speech->frame_error;
}

static h2_pal_result_t speech_admit(void *context) {
  speech_t *speech = context;
  h2_gizclaw_service_t *service = speech->service;
  h2_pal_result_t rc =
      h2_pal_mutex_lock(service->config.sync, service->audio_mutex);
  if (rc != H2_PAL_OK)
    return rc;
  rc = h2_pal_mutex_lock(service->config.sync, service->mutex);
  if (rc != H2_PAL_OK) {
    (void)h2_pal_mutex_unlock(service->config.sync, service->audio_mutex);
    return rc;
  }
  h2_gizclaw_track_t *track = atomic_load(&service->pcm_track);
  if (track == NULL || service->pcm_track_unsetting ||
      track->vtable->read == NULL)
    rc = H2_PAL_ERR_INVALID_STATE;
  else if (atomic_load(&service->speech_request) != NULL ||
           atomic_load(&service->media_request) != NULL)
    rc = H2_PAL_ERR_BUSY;
  else {
    atomic_store(&service->speech_request, speech);
    service->audio_ended = false;
  }
  (void)h2_pal_mutex_unlock(service->config.sync, service->mutex);
  (void)h2_pal_mutex_unlock(service->config.sync, service->audio_mutex);
  return rc;
}

static void speech_stop(void *context) {
  speech_t *speech = context;
  (void)h2_pal_mutex_lock(speech->service->config.sync,
                          speech->service->audio_mutex);
  detach_input(speech);
  (void)h2_pal_mutex_unlock(speech->service->config.sync,
                            speech->service->audio_mutex);
}

static void speech_destroy(void *context) {
  speech_t *speech = context;
  if (speech->input_mutex != NULL)
    (void)h2_pal_mutex_destroy(speech->service->config.sync,
                               speech->input_mutex);
  h2_gizclaw_pcm_input_deinit(&speech->input);
  h2_pal_mem_free(speech->allocator, speech);
}

static h2_pal_result_t create_speech(h2_gizclaw_service_t *service,
                                     uint64_t identity, const void *tag,
                                     h2_gizclaw_rpc_method_t method,
                                     const pb_msgdesc_t *fields,
                                     const void *message, uint32_t timeout_ms,
                                     h2_gizclaw_req_t **out_request) {
  if (out_request != NULL)
    *out_request = NULL;
  if (service == NULL || out_request == NULL || timeout_ms == 0u ||
      timeout_ms > INT32_MAX)
    return H2_PAL_ERR_INVALID_ARG;
  size_t size;
  if (!pb_get_encoded_size(&size, fields, message))
    return H2_PAL_ERR_FORMAT;
  speech_t *speech =
      h2_pal_mem_alloc(service->client_config.allocator, sizeof(*speech));
  if (speech == NULL)
    return H2_PAL_ERR_NO_MEMORY;
  memset(speech, 0, sizeof(*speech));
  speech->service = service;
  speech->allocator = service->client_config.allocator;
  atomic_init(&speech->producer_done, false);
  uint8_t *payload =
      h2_pal_mem_alloc(speech->allocator, size == 0u ? 1u : size);
  h2_pal_result_t rc = H2_PAL_ERR_NO_MEMORY;
  if (payload == NULL)
    goto fail;
  pb_ostream_t stream = pb_ostream_from_buffer(payload, size);
  if (!pb_encode(&stream, fields, message)) {
    rc = H2_PAL_ERR_FORMAT;
    goto fail;
  }
  h2_pal_mutex_config_t mutex_config = {.name = "$gizclaw/speech-input",
                                        .allocator = speech->allocator};
  rc = h2_pal_mutex_create(service->config.sync, &mutex_config,
                           &speech->input_mutex);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_req_create_pcm_stream_internal(
        service, identity, method, tag,
        (h2_gizclaw_rpc_bytes_t){payload, stream.bytes_written}, timeout_ms,
        speech_frame, speech_admit, speech_stop, speech_destroy, speech,
        out_request);
  if (rc == H2_PAL_OK) {
    speech->request = *out_request;
    h2_pal_mem_free(speech->allocator, payload);
    return rc;
  }
fail:
  h2_pal_mem_free(speech->allocator, payload);
  speech_destroy(speech);
  return rc;
}

h2_pal_result_t h2_gizclaw_req_create_speech_transcribe(
    h2_gizclaw_service_t *service, uint64_t identity,
    const h2_gizclaw_speech_transcribe_options_t *options, uint32_t timeout_ms,
    h2_gizclaw_req_t **out_request) {
  if (out_request != NULL)
    *out_request = NULL;
  if (options == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  gizclaw_rpc_v1_SpeechTranscribeRequest message =
      gizclaw_rpc_v1_SpeechTranscribeRequest_init_zero;
  if (!copy_text(message.model_name, sizeof(message.model_name),
                 options->model_name, true) ||
      !copy_text(message.content_type, sizeof(message.content_type),
                 options->content_type, true) ||
      !copy_text(message.language, sizeof(message.language), options->language,
                 false))
    return H2_PAL_ERR_INVALID_ARG;
  message.has_language = options->language.len != 0u;
  return create_speech(service, identity, &transcribe_tag,
                       H2_GIZCLAW_RPC_SERVER_SPEECH_TRANSCRIBE,
                       gizclaw_rpc_v1_SpeechTranscribeRequest_fields, &message,
                       timeout_ms, out_request);
}

h2_pal_result_t h2_gizclaw_req_create_speech_extract(
    h2_gizclaw_service_t *service, uint64_t identity,
    const h2_gizclaw_speech_extract_options_t *options, uint32_t timeout_ms,
    h2_gizclaw_req_t **out_request) {
  if (out_request != NULL)
    *out_request = NULL;
  if (service == NULL || options == NULL || out_request == NULL ||
      timeout_ms == 0u || timeout_ms > INT32_MAX)
    return H2_PAL_ERR_INVALID_ARG;
  /* The schema can be 16 KiB. Do not put the generated message on an MCU stack.
   */
  gizclaw_rpc_v1_SpeechExtractRequest *message =
      h2_pal_mem_alloc(service->client_config.allocator, sizeof(*message));
  if (message == NULL)
    return H2_PAL_ERR_NO_MEMORY;
  memset(message, 0, sizeof(*message));
  h2_pal_result_t rc = H2_PAL_ERR_INVALID_ARG;
  if (copy_text(message->asr_model_name, sizeof(message->asr_model_name),
                options->asr_model_name, true) &&
      copy_text(message->extract_model_name,
                sizeof(message->extract_model_name),
                options->extract_model_name, true) &&
      copy_text(message->content_type, sizeof(message->content_type),
                options->content_type, true) &&
      copy_text(message->language, sizeof(message->language), options->language,
                false) &&
      copy_text(message->schema_json, sizeof(message->schema_json),
                options->schema_json, true) &&
      copy_text(message->instruction, sizeof(message->instruction),
                options->instruction, false)) {
    message->has_language = options->language.len != 0u;
    message->has_instruction = options->instruction.len != 0u;
    rc = create_speech(service, identity, &extract_tag,
                       H2_GIZCLAW_RPC_SERVER_SPEECH_EXTRACT,
                       gizclaw_rpc_v1_SpeechExtractRequest_fields, message,
                       timeout_ms, out_request);
  }
  h2_pal_mem_free(service->client_config.allocator, message);
  return rc;
}

static h2_pal_result_t parse_speech(const h2_gizclaw_req_t *request,
                                    const void *tag,
                                    h2_gizclaw_resp_storage_t *storage,
                                    h2_gizclaw_speech_extract_response_t *out) {
  const h2_gizclaw_rpc_response_t *response;
  h2_pal_result_t rc =
      h2_gizclaw_req_response_internal(request, tag, &response);
  if (rc != H2_PAL_OK)
    return rc;
  h2_gizclaw_resp_arena_t arena;
  rc = h2_gizclaw_resp_arena_begin(storage, &arena);
  if (rc != H2_PAL_OK)
    return rc;
  pb_istream_t stream = pb_istream_from_buffer(response->result_payload,
                                               response->result_payload_len);
  bool seen[2] = {false, false};
  while (stream.bytes_left != 0u && rc == H2_PAL_OK) {
    pb_wire_type_t wire;
    uint32_t field;
    bool eof;
    if (!pb_decode_tag(&stream, &wire, &field, &eof) || field == 0u) {
      rc = H2_PAL_ERR_FORMAT;
      break;
    }
    if (field != 1u && !(field == 2u && tag == &extract_tag)) {
      if (!pb_skip_field(&stream, wire))
        rc = H2_PAL_ERR_FORMAT;
      continue;
    }
    if (wire != PB_WT_STRING || seen[field - 1u]) {
      rc = H2_PAL_ERR_FORMAT;
      break;
    }
    seen[field - 1u] = true;
    pb_istream_t text;
    if (!pb_make_string_substream(&stream, &text)) {
      rc = H2_PAL_ERR_FORMAT;
      break;
    }
    const size_t len = text.bytes_left;
    if (len > (field == 1u ? 8192u : 16384u)) {
      rc = H2_PAL_ERR_FORMAT;
      break;
    }
    char *copy = h2_pal_mem_alloc(&arena.allocator, len + 1u);
    if (copy == NULL) {
      rc = H2_PAL_ERR_NO_SPACE;
      break;
    }
    if (!pb_read(&text, (pb_byte_t *)copy, len) ||
        !pb_close_string_substream(&stream, &text)) {
      rc = H2_PAL_ERR_FORMAT;
      break;
    }
    copy[len] = '\0';
    h2_gizclaw_str_t value = {copy, len};
    if (!valid_text(value)) {
      rc = H2_PAL_ERR_FORMAT;
      break;
    }
    if (field == 1u)
      out->transcript = value;
    else
      out->result_json = value;
  }
  if (rc == H2_PAL_OK && tag == &extract_tag && out->result_json.len == 0u)
    rc = H2_PAL_ERR_FORMAT;
  return h2_gizclaw_resp_arena_end(&arena, rc);
}

h2_pal_result_t h2_gizclaw_resp_parse_speech_transcribe(
    const h2_gizclaw_req_t *request, h2_gizclaw_resp_storage_t *storage,
    h2_gizclaw_speech_transcribe_response_t *out_response) {
  if (out_response == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out_response, 0, sizeof(*out_response));
  h2_gizclaw_speech_extract_response_t parsed = {0};
  h2_pal_result_t rc = parse_speech(request, &transcribe_tag, storage, &parsed);
  if (rc == H2_PAL_OK)
    out_response->transcript = parsed.transcript;
  return rc;
}

h2_pal_result_t h2_gizclaw_resp_parse_speech_extract(
    const h2_gizclaw_req_t *request, h2_gizclaw_resp_storage_t *storage,
    h2_gizclaw_speech_extract_response_t *out_response) {
  if (out_response == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out_response, 0, sizeof(*out_response));
  h2_gizclaw_speech_extract_response_t parsed = {0};
  h2_pal_result_t rc = parse_speech(request, &extract_tag, storage, &parsed);
  if (rc == H2_PAL_OK)
    *out_response = parsed;
  return rc;
}
