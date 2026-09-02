#include "h2_gizclaw_speech.h"
#include "h2_gizclaw_internal.h"
#include "h2_gizclaw_pcm_ring.h"
#include "h2_gizclaw_service_internal.h"

#include "gzc_common.h"
#include "gzc_rpc.h"
#include "payload/ai.pb.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <string.h>

#define H2_GIZCLAW_SPEECH_PCM_RING_BYTES (16u * 1024u)
#define H2_GIZCLAW_SPEECH_PCM_FRAME_BYTES 640u
#define H2_GIZCLAW_SPEECH_REQUEST_TEXT_MAX 128u
#define H2_GIZCLAW_SPEECH_REQUEST_SCHEMA_MAX 1024u
#define H2_GIZCLAW_SPEECH_REQUEST_INSTRUCTION_MAX 512u
#define H2_GIZCLAW_SPEECH_REQUEST_TRANSCRIPT_MAX 1024u
#define H2_GIZCLAW_SPEECH_REQUEST_RESULT_MAX 2048u

typedef enum h2_gizclaw_speech_upload_kind {
  H2_GIZCLAW_SPEECH_UPLOAD_TRANSCRIBE = 0,
  H2_GIZCLAW_SPEECH_UPLOAD_EXTRACT,
} h2_gizclaw_speech_upload_kind_t;

struct h2_gizclaw_speech_request {
  h2_gizclaw_request_t base;
  h2_gizclaw_service_t *service;
  h2_gizclaw_operation_t *operation;
  h2_gizclaw_rpc_request_t *rpc_request;
  h2_gizclaw_pcm_ring_t pcm;
  h2_gizclaw_speech_upload_kind_t kind;
  union {
    h2_gizclaw_speech_extract_request_completion_fn extract;
    h2_gizclaw_speech_transcribe_request_completion_fn transcribe;
  } completion;
  void *completion_user;
  union {
    h2_gizclaw_speech_extract_options_t extract;
    h2_gizclaw_speech_transcribe_options_t transcribe;
  } options;
  char asr_model[H2_GIZCLAW_SPEECH_REQUEST_TEXT_MAX];
  char extract_model[H2_GIZCLAW_SPEECH_REQUEST_TEXT_MAX];
  char content_type[H2_GIZCLAW_SPEECH_REQUEST_TEXT_MAX];
  char language[H2_GIZCLAW_SPEECH_REQUEST_TEXT_MAX];
  char schema[H2_GIZCLAW_SPEECH_REQUEST_SCHEMA_MAX];
  char instruction[H2_GIZCLAW_SPEECH_REQUEST_INSTRUCTION_MAX];
  char transcript[H2_GIZCLAW_SPEECH_REQUEST_TRANSCRIPT_MAX];
  char result_json[H2_GIZCLAW_SPEECH_REQUEST_RESULT_MAX];
  h2_gizclaw_operation_result_t operation_result;
  uint8_t pending_audio[H2_GIZCLAW_SPEECH_PCM_FRAME_BYTES];
  size_t pending_audio_len;
  uint64_t identity;
  atomic_size_t queued_frames;
  atomic_size_t queued_bytes;
  atomic_size_t dropped_bytes;
  bool write_finished;
  atomic_bool committed;
  atomic_bool terminal;
  atomic_bool started;
  h2_gizclaw_request_callback_fn callback;
};

struct h2_gizclaw_speech_upload {
  const h2_pal_mem_api_t *allocator;
  h2_gizclaw_client_t *client;
  h2_gizclaw_rpc_request_t *request;
  h2_gizclaw_speech_upload_kind_t kind;
};

static int result_from_gzc(int result) {
  switch (result) {
  case GZC_OK:
    return H2_PAL_OK;
  case GZC_ERR_INVALID_ARGUMENT:
    return H2_PAL_ERR_INVALID_ARG;
  case GZC_ERR_NO_MEMORY:
    return H2_PAL_ERR_NO_MEMORY;
  case GZC_ERR_TIMEOUT:
    return H2_PAL_ERR_TIMEOUT;
  case GZC_ERR_CLOSED:
    return H2_PAL_ERR_CLOSED;
  case GZC_ERR_UNSUPPORTED:
    return H2_PAL_ERR_UNSUPPORTED;
  case GZC_ERR_WOULD_BLOCK:
    return H2_PAL_ERR_WOULD_BLOCK;
  default:
    return H2_PAL_ERR_IO;
  }
}

static bool copy_request_text(char *out, size_t capacity,
                              h2_gizclaw_str_t value, bool required) {
  if (value.len == 0u)
    return !required;
  if (value.data == NULL || value.len >= capacity ||
      memchr(value.data, '\0', value.len) != NULL) {
    return false;
  }
  memcpy(out, value.data, value.len);
  out[value.len] = '\0';
  return true;
}

static size_t bounded_string_length(const char *value, size_t capacity) {
  size_t len = 0u;
  while (len < capacity && value[len] != '\0')
    len++;
  return len;
}

static h2_gizclaw_speech_upload_t *
allocate_upload(h2_gizclaw_client_t *client,
                h2_gizclaw_speech_upload_kind_t kind) {
  const h2_pal_mem_api_t *allocator =
      h2_gizclaw_client_allocator_internal(client);
  h2_gizclaw_speech_upload_t *upload =
      h2_pal_mem_alloc(allocator, sizeof(*upload));
  if (upload == NULL)
    return NULL;
  memset(upload, 0, sizeof(*upload));
  upload->allocator = allocator;
  upload->client = client;
  upload->kind = kind;
  return upload;
}

static void free_upload(h2_gizclaw_speech_upload_t *upload) {
  h2_pal_mem_free(upload->allocator, upload);
}

static void cancel_upload(h2_gizclaw_speech_upload_t *upload) {
  if (upload == NULL)
    return;
  h2_gizclaw_rpc_request_destroy(upload->request);
  free_upload(upload);
}

static int
speech_upload_stream_event(void *user,
                           const h2_gizclaw_rpc_stream_event_t *event) {
  (void)user;
  return event != NULL ? GZC_OK : GZC_ERR_INVALID_ARGUMENT;
}

static int speech_upload_open(h2_gizclaw_client_t *client,
                              h2_gizclaw_speech_upload_kind_t kind,
                              h2_gizclaw_rpc_method_t method,
                              const pb_msgdesc_t *request_fields,
                              const void *request_message,
                              h2_gizclaw_speech_upload_t **out_upload) {
  h2_gizclaw_speech_upload_t *upload = allocate_upload(client, kind);
  if (upload == NULL)
    return H2_PAL_ERR_NO_MEMORY;
  gzc_buf_t params;
  gzc_buf_init(&params);
  int rc = h2_gizclaw_encode_pb_message(client, request_fields, request_message,
                                        &params);
  if (rc == GZC_OK) {
    rc = h2_gizclaw_client_rpc_request_start_stream(
        client, method,
        (h2_gizclaw_rpc_bytes_t){.data = params.data, .len = params.len},
        30000u, speech_upload_stream_event, upload, &upload->request);
  }
  gzc_buf_free(&params,
               gzc_client_platform(h2_gizclaw_client_gzc_internal(client)));
  if (rc != H2_PAL_OK && rc != GZC_OK) {
    free_upload(upload);
    return rc;
  }
  *out_upload = upload;
  return H2_PAL_OK;
}

static int speech_upload_write(h2_gizclaw_speech_upload_t *upload,
                               const uint8_t *data, size_t len) {
  size_t offset = 0u;
  while (offset < len) {
    size_t count = len - offset;
    if (count > GZC_RPC_MAX_FRAME_SIZE)
      count = GZC_RPC_MAX_FRAME_SIZE;
    int rc =
        h2_gizclaw_rpc_request_write(upload->request, data + offset, count);
    if (rc == H2_PAL_OK) {
      offset += count;
      continue;
    }
    if (rc != H2_PAL_ERR_WOULD_BLOCK)
      return rc;
    rc = h2_gizclaw_client_poll(upload->client, 10);
    if (rc != H2_PAL_OK && rc != H2_PAL_ERR_TIMEOUT &&
        rc != H2_PAL_ERR_WOULD_BLOCK) {
      return rc;
    }
  }
  return H2_PAL_OK;
}

static int speech_upload_finish(h2_gizclaw_speech_upload_t *upload,
                                h2_gizclaw_rpc_response_t *out_response) {
  int rc;
  while ((rc = h2_gizclaw_rpc_request_finish_write(upload->request)) ==
         H2_PAL_ERR_WOULD_BLOCK) {
    rc = h2_gizclaw_client_poll(upload->client, 10);
    if (rc != H2_PAL_OK && rc != H2_PAL_ERR_TIMEOUT &&
        rc != H2_PAL_ERR_WOULD_BLOCK) {
      return rc;
    }
  }
  while (rc == H2_PAL_OK &&
         (rc = h2_gizclaw_rpc_request_result(upload->request, out_response)) ==
             H2_PAL_ERR_WOULD_BLOCK) {
    rc = h2_gizclaw_client_poll(upload->client, 10);
    if (rc == H2_PAL_ERR_TIMEOUT || rc == H2_PAL_ERR_WOULD_BLOCK)
      rc = H2_PAL_OK;
  }
  return rc;
}

int h2_gizclaw_client_speech_transcribe_open(
    h2_gizclaw_client_t *client,
    const h2_gizclaw_speech_transcribe_options_t *options,
    h2_gizclaw_speech_upload_t **out_upload) {
  if (client == NULL || options == NULL || out_upload == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  *out_upload = NULL;
  gizclaw_rpc_v1_SpeechTranscribeRequest request =
      gizclaw_rpc_v1_SpeechTranscribeRequest_init_zero;
  if (!copy_request_text(request.model_name, sizeof(request.model_name),
                         options->model_name, true) ||
      !copy_request_text(request.content_type, sizeof(request.content_type),
                         options->content_type, true) ||
      !copy_request_text(request.language, sizeof(request.language),
                         options->language, false)) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  request.has_language = options->language.len > 0u;

  return speech_upload_open(client, H2_GIZCLAW_SPEECH_UPLOAD_TRANSCRIBE,
                            H2_GIZCLAW_RPC_SERVER_SPEECH_TRANSCRIBE,
                            gizclaw_rpc_v1_SpeechTranscribeRequest_fields,
                            &request, out_upload);
}

int h2_gizclaw_speech_transcribe_write(h2_gizclaw_speech_upload_t *upload,
                                       const uint8_t *data, size_t len) {
  if (upload == NULL || upload->kind != H2_GIZCLAW_SPEECH_UPLOAD_TRANSCRIBE ||
      upload->request == NULL || data == NULL || len == 0u) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  return speech_upload_write(upload, data, len);
}

int h2_gizclaw_speech_transcribe_finish(h2_gizclaw_speech_upload_t *upload,
                                        char *out_transcript,
                                        size_t transcript_capacity,
                                        size_t *out_transcript_len) {
  if (upload == NULL || upload->kind != H2_GIZCLAW_SPEECH_UPLOAD_TRANSCRIBE ||
      upload->request == NULL || out_transcript == NULL ||
      transcript_capacity == 0u || out_transcript_len == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  out_transcript[0] = '\0';
  *out_transcript_len = 0u;
  h2_gizclaw_rpc_response_t rpc_response = {0};
  int rc = speech_upload_finish(upload, &rpc_response);
  gizclaw_rpc_v1_SpeechTranscribeResponse response =
      gizclaw_rpc_v1_SpeechTranscribeResponse_init_zero;
  if (rc == H2_PAL_OK && rpc_response.has_error)
    rc = H2_PAL_ERR_IO;
  if (rc == H2_PAL_OK) {
    rc = result_from_gzc(h2_gizclaw_decode_pb_message(
        gzc_str_from_parts((const char *)rpc_response.result_payload,
                           rpc_response.result_payload_len),
        gizclaw_rpc_v1_SpeechTranscribeResponse_fields, &response));
  }
  if (rc == H2_PAL_OK) {
    const size_t len =
        bounded_string_length(response.transcript, sizeof(response.transcript));
    if (len >= sizeof(response.transcript) || len >= transcript_capacity) {
      rc = H2_PAL_ERR_NO_MEMORY;
    } else {
      memcpy(out_transcript, response.transcript, len + 1u);
      *out_transcript_len = len;
    }
  }
  h2_gizclaw_rpc_response_deinit(upload->client, &rpc_response);
  h2_gizclaw_rpc_request_destroy(upload->request);
  upload->request = NULL;
  free_upload(upload);
  return rc;
}

void h2_gizclaw_speech_transcribe_cancel(h2_gizclaw_speech_upload_t *upload) {
  cancel_upload(upload);
}

int h2_gizclaw_client_speech_extract_open(
    h2_gizclaw_client_t *client,
    const h2_gizclaw_speech_extract_options_t *options,
    h2_gizclaw_speech_upload_t **out_upload) {
  if (client == NULL || options == NULL || out_upload == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  *out_upload = NULL;
  gizclaw_rpc_v1_SpeechExtractRequest request =
      gizclaw_rpc_v1_SpeechExtractRequest_init_zero;
  if (!copy_request_text(request.asr_model_name, sizeof(request.asr_model_name),
                         options->asr_model_name, true) ||
      !copy_request_text(request.extract_model_name,
                         sizeof(request.extract_model_name),
                         options->extract_model_name, true) ||
      !copy_request_text(request.content_type, sizeof(request.content_type),
                         options->content_type, true) ||
      !copy_request_text(request.language, sizeof(request.language),
                         options->language, false) ||
      !copy_request_text(request.schema_json, sizeof(request.schema_json),
                         options->schema_json, true) ||
      !copy_request_text(request.instruction, sizeof(request.instruction),
                         options->instruction, false)) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  request.has_language = options->language.len > 0u;
  request.has_instruction = options->instruction.len > 0u;

  return speech_upload_open(client, H2_GIZCLAW_SPEECH_UPLOAD_EXTRACT,
                            H2_GIZCLAW_RPC_SERVER_SPEECH_EXTRACT,
                            gizclaw_rpc_v1_SpeechExtractRequest_fields,
                            &request, out_upload);
}

int h2_gizclaw_speech_extract_write(h2_gizclaw_speech_upload_t *upload,
                                    const uint8_t *data, size_t len) {
  if (upload == NULL || upload->kind != H2_GIZCLAW_SPEECH_UPLOAD_EXTRACT ||
      upload->request == NULL || data == NULL || len == 0u) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  return speech_upload_write(upload, data, len);
}

int h2_gizclaw_speech_extract_finish(
    h2_gizclaw_speech_upload_t *upload,
    h2_gizclaw_speech_extract_result_fn on_result, void *user) {
  if (upload == NULL || upload->kind != H2_GIZCLAW_SPEECH_UPLOAD_EXTRACT ||
      upload->request == NULL || on_result == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  h2_gizclaw_rpc_response_t rpc_response = {0};
  int rc = speech_upload_finish(upload, &rpc_response);
  gizclaw_rpc_v1_SpeechExtractResponse response =
      gizclaw_rpc_v1_SpeechExtractResponse_init_zero;
  if (rc == H2_PAL_OK && rpc_response.has_error)
    rc = H2_PAL_ERR_IO;
  if (rc == H2_PAL_OK) {
    rc = result_from_gzc(h2_gizclaw_decode_pb_message(
        gzc_str_from_parts((const char *)rpc_response.result_payload,
                           rpc_response.result_payload_len),
        gizclaw_rpc_v1_SpeechExtractResponse_fields, &response));
  }
  if (rc == H2_PAL_OK) {
    const size_t transcript_len =
        bounded_string_length(response.transcript, sizeof(response.transcript));
    const size_t result_json_len = bounded_string_length(
        response.result_json, sizeof(response.result_json));
    if (transcript_len >= sizeof(response.transcript) ||
        result_json_len >= sizeof(response.result_json)) {
      rc = H2_PAL_ERR_FORMAT;
    } else {
      rc = on_result(user,
                     (h2_gizclaw_str_t){.data = response.transcript,
                                        .len = transcript_len},
                     (h2_gizclaw_str_t){.data = response.result_json,
                                        .len = result_json_len});
    }
  }
  h2_gizclaw_rpc_response_deinit(upload->client, &rpc_response);
  h2_gizclaw_rpc_request_destroy(upload->request);
  upload->request = NULL;
  free_upload(upload);
  return rc;
}

void h2_gizclaw_speech_extract_cancel(h2_gizclaw_speech_upload_t *upload) {
  cancel_upload(upload);
}

static bool copy_owned_text(char *storage, size_t capacity,
                            h2_gizclaw_str_t value, bool required,
                            h2_gizclaw_str_t *out) {
  if (value.len == 0u) {
    if (required)
      return false;
    storage[0] = '\0';
    *out = (h2_gizclaw_str_t){.data = storage, .len = 0u};
    return true;
  }
  if (value.data == NULL || value.len >= capacity ||
      memchr(value.data, '\0', value.len) != NULL)
    return false;
  memcpy(storage, value.data, value.len);
  storage[value.len] = '\0';
  *out = (h2_gizclaw_str_t){.data = storage, .len = value.len};
  return true;
}

static int
speech_request_stream_event(void *user,
                            const h2_gizclaw_rpc_stream_event_t *event) {
  (void)user;
  return event != NULL ? GZC_OK : GZC_ERR_INVALID_ARGUMENT;
}

static void
speech_request_destroy_rpc(h2_gizclaw_speech_extract_request_t *request) {
  h2_gizclaw_rpc_request_destroy(request->rpc_request);
  request->rpc_request = NULL;
}

static void
speech_request_detach_route(h2_gizclaw_speech_extract_request_t *request) {
  h2_gizclaw_speech_extract_request_t *expected = request;
  (void)atomic_compare_exchange_strong_explicit(
      &request->service->speech_request, &expected, NULL, memory_order_acq_rel,
      memory_order_acquire);
}

static h2_pal_result_t
speech_request_poll(void *user, h2_gizclaw_client_t *client,
                    const h2_gizclaw_cancel_token_t *cancel_token) {
  h2_gizclaw_speech_extract_request_t *request = user;
  if (h2_gizclaw_cancel_requested(cancel_token)) {
    speech_request_detach_route(request);
    h2_gizclaw_service_log_request(request->service, H2_PAL_LOG_WARN, "speech",
                                   "poll_cancelled", request->identity,
                                   H2_PAL_ERR_CLOSED, 0, request->queued_frames,
                                   request->queued_bytes);
    speech_request_destroy_rpc(request);
    return H2_PAL_ERR_CLOSED;
  }
  if (!request->write_finished && request->pending_audio_len == 0u &&
      h2_gizclaw_pcm_ring_available(&request->pcm) == 0u &&
      !atomic_load_explicit(&request->committed, memory_order_acquire)) {
    h2_gizclaw_track_t *track = atomic_load_explicit(
        &request->service->pcm_track, memory_order_acquire);
    if (track != NULL && track->vtable != NULL && track->vtable->read != NULL) {
      size_t audio_len = 0u;
      h2_pal_result_t read_rc =
          track->vtable->read(track->user, request->pending_audio,
                              sizeof(request->pending_audio), &audio_len);
      if (read_rc == H2_PAL_OK) {
        if (audio_len == 0u || audio_len > sizeof(request->pending_audio))
          return H2_PAL_ERR_FORMAT;
        request->pending_audio_len = audio_len;
        atomic_fetch_add_explicit(&request->queued_frames, 1u,
                                  memory_order_relaxed);
        atomic_fetch_add_explicit(&request->queued_bytes, audio_len,
                                  memory_order_relaxed);
      } else if (read_rc != H2_PAL_ERR_WOULD_BLOCK &&
                 read_rc != H2_PAL_ERR_TIMEOUT) {
        speech_request_detach_route(request);
        return read_rc;
      }
    }
  }
  if (!request->write_finished) {
    if (request->pending_audio_len == 0u) {
      size_t available = h2_gizclaw_pcm_ring_available(&request->pcm);
      if (available > 0u) {
        if (available > sizeof(request->pending_audio))
          available = sizeof(request->pending_audio);
        const h2_pal_result_t read_rc = h2_gizclaw_pcm_ring_read(
            &request->pcm, request->pending_audio, available);
        if (read_rc == H2_PAL_OK)
          request->pending_audio_len = available;
        else if (read_rc != H2_PAL_ERR_WOULD_BLOCK) {
          speech_request_destroy_rpc(request);
          return read_rc;
        }
      }
    }
    if (request->pending_audio_len > 0u) {
      h2_pal_result_t rc = h2_gizclaw_rpc_request_write(
          request->rpc_request, request->pending_audio,
          request->pending_audio_len);
      if (rc == H2_PAL_ERR_WOULD_BLOCK)
        return rc;
      if (rc != H2_PAL_OK) {
        h2_gizclaw_service_log_request(
            request->service, H2_PAL_LOG_ERROR, "speech", "write_failed",
            request->identity, rc, 0, request->queued_frames,
            request->queued_bytes);
        speech_request_destroy_rpc(request);
        return rc;
      }
      request->pending_audio_len = 0u;
    }
    if (request->pending_audio_len == 0u &&
        h2_gizclaw_pcm_ring_available(&request->pcm) == 0u &&
        atomic_load_explicit(&request->committed, memory_order_acquire)) {
      const h2_pal_result_t rc =
          h2_gizclaw_rpc_request_finish_write(request->rpc_request);
      if (rc == H2_PAL_OK)
        request->write_finished = true;
      else if (rc != H2_PAL_ERR_WOULD_BLOCK) {
        h2_gizclaw_service_log_request(
            request->service, H2_PAL_LOG_ERROR, "speech", "finish_write_failed",
            request->identity, rc, 0, request->queued_frames,
            request->queued_bytes);
        speech_request_destroy_rpc(request);
        return rc;
      }
    }
    if (!request->write_finished)
      return H2_PAL_ERR_WOULD_BLOCK;
  }
  h2_gizclaw_rpc_response_t rpc_response = {0};
  h2_pal_result_t rc =
      h2_gizclaw_rpc_request_result(request->rpc_request, &rpc_response);
  if (rc == H2_PAL_ERR_WOULD_BLOCK)
    return rc;
  const int rpc_error_code =
      rc == H2_PAL_OK && rpc_response.has_error ? rpc_response.error_code : 0;
  if (rc == H2_PAL_OK && rpc_response.has_error)
    rc = H2_PAL_ERR_IO;
  if (request->kind == H2_GIZCLAW_SPEECH_UPLOAD_EXTRACT) {
    gizclaw_rpc_v1_SpeechExtractResponse response =
        gizclaw_rpc_v1_SpeechExtractResponse_init_zero;
    if (rc == H2_PAL_OK) {
      rc = result_from_gzc(h2_gizclaw_decode_pb_message(
          gzc_str_from_parts((const char *)rpc_response.result_payload,
                             rpc_response.result_payload_len),
          gizclaw_rpc_v1_SpeechExtractResponse_fields, &response));
    }
    if (rc == H2_PAL_OK) {
      const size_t transcript_len = bounded_string_length(
          response.transcript, sizeof(response.transcript));
      const size_t result_len = bounded_string_length(
          response.result_json, sizeof(response.result_json));
      if (transcript_len >= sizeof(request->transcript) ||
          result_len >= sizeof(request->result_json)) {
        rc = H2_PAL_ERR_NO_MEMORY;
      } else {
        memcpy(request->transcript, response.transcript, transcript_len + 1u);
        memcpy(request->result_json, response.result_json, result_len + 1u);
      }
    }
  } else {
    gizclaw_rpc_v1_SpeechTranscribeResponse response =
        gizclaw_rpc_v1_SpeechTranscribeResponse_init_zero;
    if (rc == H2_PAL_OK) {
      rc = result_from_gzc(h2_gizclaw_decode_pb_message(
          gzc_str_from_parts((const char *)rpc_response.result_payload,
                             rpc_response.result_payload_len),
          gizclaw_rpc_v1_SpeechTranscribeResponse_fields, &response));
    }
    if (rc == H2_PAL_OK) {
      const size_t transcript_len = bounded_string_length(
          response.transcript, sizeof(response.transcript));
      if (transcript_len >= sizeof(request->transcript)) {
        rc = H2_PAL_ERR_NO_MEMORY;
      } else {
        memcpy(request->transcript, response.transcript, transcript_len + 1u);
      }
    }
  }
  if (rc != H2_PAL_OK) {
    h2_gizclaw_service_log_request(request->service, H2_PAL_LOG_ERROR, "speech",
                                   "result_failed", request->identity, rc,
                                   rpc_error_code, request->queued_frames,
                                   request->queued_bytes);
  }
  h2_gizclaw_rpc_response_deinit(client, &rpc_response);
  speech_request_destroy_rpc(request);
  return rc;
}

static h2_pal_result_t
speech_request_start(void *user, h2_gizclaw_client_t *client,
                     const h2_gizclaw_cancel_token_t *cancel_token) {
  h2_gizclaw_speech_extract_request_t *request = user;
  if (h2_gizclaw_cancel_requested(cancel_token)) {
    h2_gizclaw_service_log_request(request->service, H2_PAL_LOG_WARN, "speech",
                                   "start_cancelled", request->identity,
                                   H2_PAL_ERR_CLOSED, 0, request->queued_frames,
                                   request->queued_bytes);
    return H2_PAL_ERR_CLOSED;
  }
  gzc_buf_t params;
  gzc_buf_init(&params);
  int rc;
  h2_gizclaw_rpc_method_t method;
  if (request->kind == H2_GIZCLAW_SPEECH_UPLOAD_EXTRACT) {
    gizclaw_rpc_v1_SpeechExtractRequest params_message =
        gizclaw_rpc_v1_SpeechExtractRequest_init_zero;
    if (!copy_request_text(params_message.asr_model_name,
                           sizeof(params_message.asr_model_name),
                           request->options.extract.asr_model_name, true) ||
        !copy_request_text(params_message.extract_model_name,
                           sizeof(params_message.extract_model_name),
                           request->options.extract.extract_model_name, true) ||
        !copy_request_text(params_message.content_type,
                           sizeof(params_message.content_type),
                           request->options.extract.content_type, true) ||
        !copy_request_text(params_message.language,
                           sizeof(params_message.language),
                           request->options.extract.language, false) ||
        !copy_request_text(params_message.schema_json,
                           sizeof(params_message.schema_json),
                           request->options.extract.schema_json, true) ||
        !copy_request_text(params_message.instruction,
                           sizeof(params_message.instruction),
                           request->options.extract.instruction, false)) {
      h2_gizclaw_service_log_request(
          request->service, H2_PAL_LOG_ERROR, "speech", "options_invalid",
          request->identity, H2_PAL_ERR_INVALID_ARG, 0, request->queued_frames,
          request->queued_bytes);
      return H2_PAL_ERR_INVALID_ARG;
    }
    params_message.has_language = request->options.extract.language.len > 0u;
    params_message.has_instruction =
        request->options.extract.instruction.len > 0u;
    rc = h2_gizclaw_encode_pb_message(
        client, gizclaw_rpc_v1_SpeechExtractRequest_fields, &params_message,
        &params);
    method = H2_GIZCLAW_RPC_SERVER_SPEECH_EXTRACT;
  } else {
    gizclaw_rpc_v1_SpeechTranscribeRequest params_message =
        gizclaw_rpc_v1_SpeechTranscribeRequest_init_zero;
    if (!copy_request_text(params_message.model_name,
                           sizeof(params_message.model_name),
                           request->options.transcribe.model_name, true) ||
        !copy_request_text(params_message.content_type,
                           sizeof(params_message.content_type),
                           request->options.transcribe.content_type, true) ||
        !copy_request_text(params_message.language,
                           sizeof(params_message.language),
                           request->options.transcribe.language, false)) {
      h2_gizclaw_service_log_request(
          request->service, H2_PAL_LOG_ERROR, "speech", "options_invalid",
          request->identity, H2_PAL_ERR_INVALID_ARG, 0, request->queued_frames,
          request->queued_bytes);
      return H2_PAL_ERR_INVALID_ARG;
    }
    params_message.has_language = request->options.transcribe.language.len > 0u;
    rc = h2_gizclaw_encode_pb_message(
        client, gizclaw_rpc_v1_SpeechTranscribeRequest_fields, &params_message,
        &params);
    method = H2_GIZCLAW_RPC_SERVER_SPEECH_TRANSCRIBE;
  }
  if (rc == GZC_OK) {
    rc = h2_gizclaw_client_rpc_request_start_stream(
        client, method,
        (h2_gizclaw_rpc_bytes_t){.data = params.data, .len = params.len},
        30000u, speech_request_stream_event, request, &request->rpc_request);
  }
  gzc_buf_free(&params,
               gzc_client_platform(h2_gizclaw_client_gzc_internal(client)));
  if (rc != H2_PAL_OK && rc != GZC_OK) {
    h2_gizclaw_service_log_request(
        request->service, H2_PAL_LOG_ERROR, "speech", "rpc_start_failed",
        request->identity, (h2_pal_result_t)rc, rc, request->queued_frames,
        request->queued_bytes);
    return rc;
  }
  h2_gizclaw_service_log_request(request->service, H2_PAL_LOG_INFO, "speech",
                                 "rpc_started", request->identity, H2_PAL_OK, 0,
                                 request->queued_frames, request->queued_bytes);
  return speech_request_poll(user, client, cancel_token);
}

static void
speech_request_complete(void *user, h2_gizclaw_operation_t *operation,
                        const h2_gizclaw_operation_result_t *result) {
  (void)operation;
  h2_gizclaw_speech_extract_request_t *request = user;
  speech_request_detach_route(request);
  request->operation_result = *result;
  atomic_store_explicit(&request->terminal, true, memory_order_release);
  h2_gizclaw_service_log_request(
      request->service,
      result->result == H2_PAL_OK ? H2_PAL_LOG_INFO : H2_PAL_LOG_ERROR,
      "speech", "completed", request->identity, result->result, 0,
      request->queued_frames, request->queued_bytes);
  if (request->kind == H2_GIZCLAW_SPEECH_UPLOAD_EXTRACT) {
    if (request->completion.extract != NULL)
    request->completion.extract(request->completion_user, request);
  } else if (request->completion.transcribe != NULL) {
    request->completion.transcribe(request->completion_user, request);
  }
  if (request->callback != NULL)
    request->callback(&request->base);
}

int h2_gizclaw_service_speech_extract_create(
    h2_gizclaw_service_t *service, uint64_t identity,
    const h2_gizclaw_speech_extract_options_t *options,
    h2_gizclaw_speech_extract_request_completion_fn completion, void *user,
    h2_gizclaw_speech_extract_request_t **out_request) {
  if (service == NULL || options == NULL || completion == NULL ||
      out_request == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  *out_request = NULL;
  const h2_pal_mem_api_t *allocator = service->config.client_config->allocator;
  h2_gizclaw_speech_extract_request_t *request =
      h2_pal_mem_alloc(allocator, sizeof(*request));
  if (request == NULL)
    return H2_PAL_ERR_NO_MEMORY;
  memset(request, 0, sizeof(*request));
  request->service = service;
  request->identity = identity;
  request->kind = H2_GIZCLAW_SPEECH_UPLOAD_EXTRACT;
  request->completion.extract = completion;
  request->completion_user = user;
  if (!copy_owned_text(request->asr_model, sizeof(request->asr_model),
                       options->asr_model_name, true,
                       &request->options.extract.asr_model_name) ||
      !copy_owned_text(request->extract_model, sizeof(request->extract_model),
                       options->extract_model_name, true,
                       &request->options.extract.extract_model_name) ||
      !copy_owned_text(request->content_type, sizeof(request->content_type),
                       options->content_type, true,
                       &request->options.extract.content_type) ||
      !copy_owned_text(request->language, sizeof(request->language),
                       options->language, false,
                       &request->options.extract.language) ||
      !copy_owned_text(request->schema, sizeof(request->schema),
                       options->schema_json, true,
                       &request->options.extract.schema_json) ||
      !copy_owned_text(request->instruction, sizeof(request->instruction),
                       options->instruction, false,
                       &request->options.extract.instruction)) {
    h2_pal_mem_free(allocator, request);
    return H2_PAL_ERR_INVALID_ARG;
  }
  int rc = h2_gizclaw_pcm_ring_init(&request->pcm, allocator,
                                    H2_GIZCLAW_SPEECH_PCM_RING_BYTES);
  if (rc == H2_PAL_OK) {
    rc = h2_gizclaw_service_submit_async_internal(
        service, identity, speech_request_start, speech_request_poll,
        speech_request_complete, request, &request->operation);
  }
  if (rc != H2_PAL_OK) {
    h2_gizclaw_service_log_request(service, H2_PAL_LOG_ERROR, "speech",
                                   "create_failed", identity,
                                   (h2_pal_result_t)rc, rc, 0u, 0u);
    h2_gizclaw_pcm_ring_deinit(&request->pcm);
    h2_pal_mem_free(allocator, request);
    return rc;
  }
  h2_gizclaw_service_log_request(service, H2_PAL_LOG_INFO, "speech", "created",
                                 identity, H2_PAL_OK, 0, 0u, 0u);
  *out_request = request;
  return H2_PAL_OK;
}

int h2_gizclaw_service_speech_transcribe_create(
    h2_gizclaw_service_t *service, uint64_t identity,
    const h2_gizclaw_speech_transcribe_options_t *options,
    h2_gizclaw_speech_transcribe_request_completion_fn completion, void *user,
    h2_gizclaw_speech_transcribe_request_t **out_request) {
  if (service == NULL || options == NULL || completion == NULL ||
      out_request == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  *out_request = NULL;
  const h2_pal_mem_api_t *allocator = service->config.client_config->allocator;
  h2_gizclaw_speech_transcribe_request_t *request =
      h2_pal_mem_alloc(allocator, sizeof(*request));
  if (request == NULL)
    return H2_PAL_ERR_NO_MEMORY;
  memset(request, 0, sizeof(*request));
  request->service = service;
  request->identity = identity;
  request->kind = H2_GIZCLAW_SPEECH_UPLOAD_TRANSCRIBE;
  request->completion.transcribe = completion;
  request->completion_user = user;
  if (!copy_owned_text(request->asr_model, sizeof(request->asr_model),
                       options->model_name, true,
                       &request->options.transcribe.model_name) ||
      !copy_owned_text(request->content_type, sizeof(request->content_type),
                       options->content_type, true,
                       &request->options.transcribe.content_type) ||
      !copy_owned_text(request->language, sizeof(request->language),
                       options->language, false,
                       &request->options.transcribe.language)) {
    h2_pal_mem_free(allocator, request);
    return H2_PAL_ERR_INVALID_ARG;
  }
  int rc = h2_gizclaw_pcm_ring_init(&request->pcm, allocator,
                                    H2_GIZCLAW_SPEECH_PCM_RING_BYTES);
  if (rc == H2_PAL_OK) {
    rc = h2_gizclaw_service_submit_async_internal(
        service, identity, speech_request_start, speech_request_poll,
        speech_request_complete, request, &request->operation);
  }
  if (rc != H2_PAL_OK) {
    h2_gizclaw_service_log_request(service, H2_PAL_LOG_ERROR, "speech",
                                   "create_failed", identity,
                                   (h2_pal_result_t)rc, rc, 0u, 0u);
    h2_gizclaw_pcm_ring_deinit(&request->pcm);
    h2_pal_mem_free(allocator, request);
    return rc;
  }
  h2_gizclaw_service_log_request(service, H2_PAL_LOG_INFO, "speech", "created",
                                 identity, H2_PAL_OK, 0, 0u, 0u);
  *out_request = request;
  return H2_PAL_OK;
}

int h2_gizclaw_speech_extract_request_write_audio(
    h2_gizclaw_speech_extract_request_t *request, const uint8_t *audio,
    size_t audio_len, uint32_t timeout_ms) {
  (void)timeout_ms;
  if (request == NULL || audio == NULL || audio_len == 0u ||
      audio_len > H2_GIZCLAW_SPEECH_AUDIO_CHUNK_MAX_BYTES)
    return H2_PAL_ERR_INVALID_ARG;
  if (atomic_load_explicit(&request->committed, memory_order_acquire) ||
      atomic_load_explicit(&request->terminal, memory_order_acquire)) {
    return H2_PAL_ERR_CLOSED;
  }
  size_t dropped = 0u;
  const h2_pal_result_t rc = h2_gizclaw_pcm_ring_write_latest(
      &request->pcm, audio, audio_len, &dropped);
  if (rc == H2_PAL_OK) {
    atomic_fetch_add_explicit(&request->queued_frames, 1u,
                              memory_order_relaxed);
    atomic_fetch_add_explicit(&request->queued_bytes, audio_len,
                              memory_order_relaxed);
    if (dropped > 0u)
      atomic_fetch_add_explicit(&request->dropped_bytes, dropped,
                                memory_order_relaxed);
  }
  if (rc != H2_PAL_OK && rc != H2_PAL_ERR_WOULD_BLOCK) {
    h2_gizclaw_service_log_request(request->service, H2_PAL_LOG_ERROR, "speech",
                                   "enqueue_failed", request->identity, rc, 0,
                                   request->queued_frames,
                                   request->queued_bytes);
  }
  return rc;
}

int h2_gizclaw_speech_extract_request_commit(
    h2_gizclaw_speech_extract_request_t *request, uint32_t timeout_ms) {
  (void)timeout_ms;
  if (request == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  bool expected = false;
  h2_pal_result_t rc = H2_PAL_OK;
  if (atomic_load_explicit(&request->terminal, memory_order_acquire) ||
      !atomic_compare_exchange_strong_explicit(&request->committed, &expected,
                                               true, memory_order_acq_rel,
                                               memory_order_acquire))
    rc = H2_PAL_ERR_CLOSED;
  h2_gizclaw_service_log_request(
      request->service, rc == H2_PAL_OK ? H2_PAL_LOG_INFO : H2_PAL_LOG_ERROR,
      "speech", "commit", request->identity, rc, 0, request->queued_frames,
      request->queued_bytes);
  return rc;
}

int h2_gizclaw_speech_extract_request_cancel(
    h2_gizclaw_speech_extract_request_t *request) {
  if (request == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  return h2_gizclaw_operation_cancel(request->operation);
}

int h2_gizclaw_speech_extract_request_wait(
    h2_gizclaw_speech_extract_request_t *request, uint32_t timeout_ms) {
  return request == NULL
             ? H2_PAL_ERR_INVALID_ARG
             : h2_gizclaw_operation_wait(request->operation, timeout_ms);
}

const h2_gizclaw_operation_result_t *
h2_gizclaw_speech_extract_request_operation_result(
    const h2_gizclaw_speech_extract_request_t *request) {
  if (request == NULL ||
      !atomic_load_explicit(&request->terminal, memory_order_acquire))
    return NULL;
  return &request->operation_result;
}

h2_gizclaw_str_t h2_gizclaw_speech_extract_request_transcript(
    const h2_gizclaw_speech_extract_request_t *request) {
  if (h2_gizclaw_speech_extract_request_operation_result(request) == NULL)
    return (h2_gizclaw_str_t){0};
  return (h2_gizclaw_str_t){.data = request->transcript,
                            .len = strlen(request->transcript)};
}

h2_gizclaw_str_t h2_gizclaw_speech_extract_request_result_json(
    const h2_gizclaw_speech_extract_request_t *request) {
  if (h2_gizclaw_speech_extract_request_operation_result(request) == NULL)
    return (h2_gizclaw_str_t){0};
  return (h2_gizclaw_str_t){.data = request->result_json,
                            .len = strlen(request->result_json)};
}

void h2_gizclaw_speech_extract_request_release(
    h2_gizclaw_speech_extract_request_t *request) {
  if (request == NULL ||
      !atomic_load_explicit(&request->terminal, memory_order_acquire))
    return;
  h2_gizclaw_operation_release(request->operation);
  h2_gizclaw_pcm_ring_close(&request->pcm);
  h2_gizclaw_pcm_ring_deinit(&request->pcm);
  h2_pal_mem_free(request->service->config.client_config->allocator, request);
}

int h2_gizclaw_speech_transcribe_request_write_audio(
    h2_gizclaw_speech_transcribe_request_t *request, const uint8_t *audio,
    size_t audio_len, uint32_t timeout_ms) {
  return h2_gizclaw_speech_extract_request_write_audio(request, audio,
                                                       audio_len, timeout_ms);
}

int h2_gizclaw_speech_transcribe_request_commit(
    h2_gizclaw_speech_transcribe_request_t *request, uint32_t timeout_ms) {
  return h2_gizclaw_speech_extract_request_commit(request, timeout_ms);
}

int h2_gizclaw_speech_transcribe_request_cancel(
    h2_gizclaw_speech_transcribe_request_t *request) {
  return h2_gizclaw_speech_extract_request_cancel(request);
}

int h2_gizclaw_speech_transcribe_request_wait(
    h2_gizclaw_speech_transcribe_request_t *request, uint32_t timeout_ms) {
  return h2_gizclaw_speech_extract_request_wait(request, timeout_ms);
}

const h2_gizclaw_operation_result_t *
h2_gizclaw_speech_transcribe_request_operation_result(
    const h2_gizclaw_speech_transcribe_request_t *request) {
  return h2_gizclaw_speech_extract_request_operation_result(request);
}

h2_gizclaw_str_t h2_gizclaw_speech_transcribe_request_transcript(
    const h2_gizclaw_speech_transcribe_request_t *request) {
  return h2_gizclaw_speech_extract_request_transcript(request);
}

void h2_gizclaw_speech_transcribe_request_release(
    h2_gizclaw_speech_transcribe_request_t *request) {
  h2_gizclaw_speech_extract_request_release(request);
}

static h2_pal_result_t asr_request_do(h2_gizclaw_request_t *base,
                                      h2_gizclaw_request_callback_fn callback) {
  h2_gizclaw_speech_transcribe_request_t *request =
      (h2_gizclaw_speech_transcribe_request_t *)base;
  bool expected = false;
  if (!atomic_compare_exchange_strong_explicit(&request->started, &expected,
                                               true, memory_order_acq_rel,
                                               memory_order_acquire)) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  h2_gizclaw_track_t *track =
      atomic_load_explicit(&request->service->pcm_track, memory_order_acquire);
  if (track == NULL || track->vtable == NULL || track->vtable->read == NULL) {
    atomic_store_explicit(&request->started, false, memory_order_release);
    return H2_PAL_ERR_INVALID_STATE;
  }
  h2_gizclaw_speech_extract_request_t *expected_request = NULL;
  if (!atomic_compare_exchange_strong_explicit(
          &request->service->speech_request, &expected_request, request,
          memory_order_acq_rel, memory_order_acquire)) {
    atomic_store_explicit(&request->started, false, memory_order_release);
    return H2_PAL_ERR_WOULD_BLOCK;
  }
  request->callback = callback;
  h2_pal_result_t rc = h2_gizclaw_service_submit_async_internal(
      request->service, request->identity, speech_request_start,
      speech_request_poll, speech_request_complete, request,
      &request->operation);
  if (rc != H2_PAL_OK) {
    speech_request_detach_route(request);
    request->callback = NULL;
    atomic_store_explicit(&request->started, false, memory_order_release);
  }
  return rc;
}

static h2_pal_result_t asr_request_finish_input(h2_gizclaw_request_t *base) {
  h2_gizclaw_speech_transcribe_request_t *request =
      (h2_gizclaw_speech_transcribe_request_t *)base;
  if (!atomic_load_explicit(&request->started, memory_order_acquire))
    return H2_PAL_ERR_INVALID_STATE;
  if (atomic_load_explicit(&request->terminal, memory_order_acquire))
    return H2_PAL_ERR_CLOSED;
  speech_request_detach_route(request);
  atomic_store_explicit(&request->committed, true, memory_order_release);
  return H2_PAL_OK;
}

static h2_pal_result_t asr_request_wait(h2_gizclaw_request_t *base,
                                        uint32_t timeout_ms) {
  h2_gizclaw_speech_transcribe_request_t *request =
      (h2_gizclaw_speech_transcribe_request_t *)base;
  if (!atomic_load_explicit(&request->started, memory_order_acquire))
    return H2_PAL_ERR_INVALID_STATE;
  return h2_gizclaw_operation_wait(request->operation, timeout_ms);
}

static h2_pal_result_t asr_request_cancel(h2_gizclaw_request_t *base) {
  h2_gizclaw_speech_transcribe_request_t *request =
      (h2_gizclaw_speech_transcribe_request_t *)base;
  if (!atomic_load_explicit(&request->started, memory_order_acquire))
    return H2_PAL_ERR_INVALID_STATE;
  speech_request_detach_route(request);
  return h2_gizclaw_operation_cancel(request->operation);
}

static void asr_request_release(h2_gizclaw_request_t *base) {
  h2_gizclaw_speech_transcribe_request_t *request =
      (h2_gizclaw_speech_transcribe_request_t *)base;
  if (atomic_load_explicit(&request->started, memory_order_acquire)) {
    h2_gizclaw_speech_transcribe_request_release(request);
    return;
  }
  h2_gizclaw_pcm_ring_deinit(&request->pcm);
  h2_pal_mem_free(request->service->config.client_config->allocator, request);
}

static const h2_gizclaw_request_vtable_t asr_request_vtable = {
    .do_request = asr_request_do,
    .finish_input = asr_request_finish_input,
    .wait = asr_request_wait,
    .cancel = asr_request_cancel,
    .release = asr_request_release,
};

h2_pal_result_t h2_gizclaw_asr_request_create(
    h2_gizclaw_service_t *service, uint64_t identity,
    const h2_gizclaw_speech_transcribe_options_t *options,
    h2_gizclaw_asr_request_t **out_request) {
  if (service == NULL || options == NULL || out_request == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  *out_request = NULL;
  const h2_pal_mem_api_t *allocator = service->config.client_config->allocator;
  h2_gizclaw_speech_transcribe_request_t *request =
      h2_pal_mem_alloc(allocator, sizeof(*request));
  if (request == NULL)
    return H2_PAL_ERR_NO_MEMORY;
  memset(request, 0, sizeof(*request));
  request->base.vtable = &asr_request_vtable;
  request->service = service;
  request->identity = identity;
  request->kind = H2_GIZCLAW_SPEECH_UPLOAD_TRANSCRIBE;
  if (!copy_owned_text(request->asr_model, sizeof(request->asr_model),
                       options->model_name, true,
                       &request->options.transcribe.model_name) ||
      !copy_owned_text(request->content_type, sizeof(request->content_type),
                       options->content_type, true,
                       &request->options.transcribe.content_type) ||
      !copy_owned_text(request->language, sizeof(request->language),
                       options->language, false,
                       &request->options.transcribe.language)) {
    h2_pal_mem_free(allocator, request);
    return H2_PAL_ERR_INVALID_ARG;
  }
  h2_pal_result_t rc = h2_gizclaw_pcm_ring_init(
      &request->pcm, allocator, H2_GIZCLAW_SPEECH_PCM_RING_BYTES);
  if (rc != H2_PAL_OK) {
    h2_pal_mem_free(allocator, request);
    return rc;
  }
  atomic_init(&request->started, false);
  atomic_init(&request->committed, false);
  atomic_init(&request->terminal, false);
  *out_request = &request->base;
  return H2_PAL_OK;
}

h2_gizclaw_str_t
h2_gizclaw_asr_request_response(const h2_gizclaw_asr_request_t *base) {
  const h2_gizclaw_speech_transcribe_request_t *request =
      (const h2_gizclaw_speech_transcribe_request_t *)base;
  if (request == NULL ||
      !atomic_load_explicit(&request->terminal, memory_order_acquire) ||
      request->operation_result.result != H2_PAL_OK) {
    return (h2_gizclaw_str_t){0};
  }
  return (h2_gizclaw_str_t){.data = request->transcript,
                            .len = strlen(request->transcript)};
}

#ifdef H2_GIZCLAW_TESTING
int h2_gizclaw_speech_test_request_create(
    h2_gizclaw_service_t *service,
    h2_gizclaw_speech_extract_request_t **out_request) {
  if (service == NULL || out_request == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  *out_request = NULL;
  const h2_pal_mem_api_t *allocator = service->config.client_config->allocator;
  h2_gizclaw_speech_extract_request_t *request =
      h2_pal_mem_alloc(allocator, sizeof(*request));
  if (request == NULL)
    return H2_PAL_ERR_NO_MEMORY;
  memset(request, 0, sizeof(*request));
  request->service = service;
  const int rc = h2_gizclaw_pcm_ring_init(&request->pcm, allocator,
                                          H2_GIZCLAW_SPEECH_PCM_RING_BYTES);
  if (rc != H2_PAL_OK) {
    h2_pal_mem_free(allocator, request);
    return rc;
  }
  *out_request = request;
  return H2_PAL_OK;
}

int h2_gizclaw_speech_test_request_receive(
    h2_gizclaw_speech_extract_request_t *request, uint8_t *out_audio,
    size_t audio_capacity, size_t *out_audio_len, uint32_t timeout_ms) {
  if (request == NULL || out_audio_len == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  (void)timeout_ms;
  size_t available = h2_gizclaw_pcm_ring_available(&request->pcm);
  if (available == 0u) {
    if (atomic_load_explicit(&request->committed, memory_order_acquire)) {
      *out_audio_len = 0u;
      return H2_PAL_OK;
    }
    return H2_PAL_ERR_WOULD_BLOCK;
  }
  if (available > audio_capacity || out_audio == NULL)
    return H2_PAL_ERR_NO_SPACE;
  const h2_pal_result_t rc =
      h2_gizclaw_pcm_ring_read(&request->pcm, out_audio, available);
  if (rc == H2_PAL_OK)
    *out_audio_len = available;
  return rc;
}

void h2_gizclaw_speech_test_request_set_terminal(
    h2_gizclaw_speech_extract_request_t *request) {
  if (request != NULL)
    atomic_store_explicit(&request->terminal, true, memory_order_release);
}

void h2_gizclaw_speech_test_request_destroy(
    h2_gizclaw_speech_extract_request_t *request) {
  if (request == NULL)
    return;
  h2_gizclaw_pcm_ring_close(&request->pcm);
  h2_gizclaw_pcm_ring_deinit(&request->pcm);
  h2_pal_mem_free(request->service->config.client_config->allocator, request);
}
#endif
