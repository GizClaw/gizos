#include "h2_gizclaw_speech.h"
#include "h2_gizclaw_internal.h"
#include "h2_gizclaw_service_internal.h"

#include "gzc_common.h"
#include "gzc_rpc.h"
#include "payload/ai.pb.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <string.h>

#define H2_GIZCLAW_SPEECH_REQUEST_QUEUE_ITEMS 8u
#define H2_GIZCLAW_SPEECH_REQUEST_WAIT_MS 100u
#define H2_GIZCLAW_SPEECH_REQUEST_TEXT_MAX 128u
#define H2_GIZCLAW_SPEECH_REQUEST_SCHEMA_MAX 1024u
#define H2_GIZCLAW_SPEECH_REQUEST_INSTRUCTION_MAX 512u
#define H2_GIZCLAW_SPEECH_REQUEST_TRANSCRIPT_MAX 1024u
#define H2_GIZCLAW_SPEECH_REQUEST_RESULT_MAX 2048u

typedef struct h2_gizclaw_speech_request_message {
  size_t len;
  uint8_t opus[H2_GIZCLAW_SPEECH_OPUS_MAX_BYTES];
} h2_gizclaw_speech_request_message_t;

struct h2_gizclaw_speech_extract_request {
  h2_gizclaw_service_t *service;
  h2_gizclaw_operation_t *operation;
  h2_gizclaw_rpc_request_t *rpc_request;
  h2_pal_queue_t *queue;
  h2_gizclaw_speech_extract_request_completion_fn completion;
  void *completion_user;
  h2_gizclaw_speech_extract_options_t options;
  char asr_model[H2_GIZCLAW_SPEECH_REQUEST_TEXT_MAX];
  char extract_model[H2_GIZCLAW_SPEECH_REQUEST_TEXT_MAX];
  char content_type[H2_GIZCLAW_SPEECH_REQUEST_TEXT_MAX];
  char language[H2_GIZCLAW_SPEECH_REQUEST_TEXT_MAX];
  char schema[H2_GIZCLAW_SPEECH_REQUEST_SCHEMA_MAX];
  char instruction[H2_GIZCLAW_SPEECH_REQUEST_INSTRUCTION_MAX];
  char transcript[H2_GIZCLAW_SPEECH_REQUEST_TRANSCRIPT_MAX];
  char result_json[H2_GIZCLAW_SPEECH_REQUEST_RESULT_MAX];
  h2_gizclaw_speech_request_message_t pending_message;
  bool has_pending_message;
  bool write_finished;
  atomic_bool committed;
  atomic_bool terminal;
};

typedef enum h2_gizclaw_speech_upload_kind {
  H2_GIZCLAW_SPEECH_UPLOAD_TRANSCRIBE = 0,
  H2_GIZCLAW_SPEECH_UPLOAD_EXTRACT,
} h2_gizclaw_speech_upload_kind_t;

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

static int speech_upload_stream_event(
    void *user, const h2_gizclaw_rpc_stream_event_t *event) {
  (void)user;
  return event != NULL ? GZC_OK : GZC_ERR_INVALID_ARGUMENT;
}

static int speech_upload_open(
    h2_gizclaw_client_t *client, h2_gizclaw_speech_upload_kind_t kind,
    h2_gizclaw_rpc_method_t method, const pb_msgdesc_t *request_fields,
    const void *request_message, h2_gizclaw_speech_upload_t **out_upload) {
  h2_gizclaw_speech_upload_t *upload = allocate_upload(client, kind);
  if (upload == NULL)
    return H2_PAL_ERR_NO_MEMORY;
  gzc_buf_t params;
  gzc_buf_init(&params);
  int rc = h2_gizclaw_encode_pb_message(client, request_fields,
                                        request_message, &params);
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
    int rc = h2_gizclaw_rpc_request_write(upload->request, data + offset,
                                          count);
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

  return speech_upload_open(
      client, H2_GIZCLAW_SPEECH_UPLOAD_TRANSCRIBE,
      H2_GIZCLAW_RPC_SERVER_SPEECH_TRANSCRIBE,
      gizclaw_rpc_v1_SpeechTranscribeRequest_fields, &request, out_upload);
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

  return speech_upload_open(
      client, H2_GIZCLAW_SPEECH_UPLOAD_EXTRACT,
      H2_GIZCLAW_RPC_SERVER_SPEECH_EXTRACT,
      gizclaw_rpc_v1_SpeechExtractRequest_fields, &request, out_upload);
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
    const size_t transcript_len = bounded_string_length(
        response.transcript, sizeof(response.transcript));
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

static int speech_request_stream_event(
    void *user, const h2_gizclaw_rpc_stream_event_t *event) {
  (void)user;
  return event != NULL ? GZC_OK : GZC_ERR_INVALID_ARGUMENT;
}

static void speech_request_destroy_rpc(
    h2_gizclaw_speech_extract_request_t *request) {
  h2_gizclaw_rpc_request_destroy(request->rpc_request);
  request->rpc_request = NULL;
}

static h2_pal_result_t speech_request_poll(
    void *user, h2_gizclaw_client_t *client,
    const h2_gizclaw_cancel_token_t *cancel_token) {
  h2_gizclaw_speech_extract_request_t *request = user;
  if (h2_gizclaw_cancel_requested(cancel_token)) {
    speech_request_destroy_rpc(request);
    return H2_PAL_ERR_CLOSED;
  }
  if (!request->write_finished) {
    if (!request->has_pending_message) {
      const int queue_rc = h2_pal_queue_recv(
          request->service->config.queue, request->queue,
          &request->pending_message, H2_PAL_QUEUE_NO_WAIT);
      if (queue_rc == H2_PAL_QUEUE_OK) {
        request->has_pending_message = true;
      } else if (queue_rc != H2_PAL_QUEUE_ERR_TIMEOUT &&
                 queue_rc != H2_PAL_ERR_WOULD_BLOCK) {
        speech_request_destroy_rpc(request);
        return queue_rc == H2_PAL_QUEUE_ERR_CLOSED ? H2_PAL_ERR_CLOSED
                                                   : H2_PAL_ERR_IO;
      }
    }
    if (request->has_pending_message) {
      h2_pal_result_t rc;
      if (request->pending_message.len == 0u) {
        rc = h2_gizclaw_rpc_request_finish_write(request->rpc_request);
        if (rc == H2_PAL_OK)
          request->write_finished = true;
      } else {
        rc = h2_gizclaw_rpc_request_write(
            request->rpc_request, request->pending_message.opus,
            request->pending_message.len);
      }
      if (rc == H2_PAL_ERR_WOULD_BLOCK)
        return rc;
      if (rc != H2_PAL_OK) {
        speech_request_destroy_rpc(request);
        return rc;
      }
      request->has_pending_message = false;
      memset(&request->pending_message, 0, sizeof(request->pending_message));
    }
    if (!request->write_finished)
      return H2_PAL_ERR_WOULD_BLOCK;
  }
  h2_gizclaw_rpc_response_t rpc_response = {0};
  h2_pal_result_t rc = h2_gizclaw_rpc_request_result(request->rpc_request,
                                                      &rpc_response);
  if (rc == H2_PAL_ERR_WOULD_BLOCK)
    return rc;
  if (rc == H2_PAL_OK && rpc_response.has_error)
    rc = H2_PAL_ERR_IO;
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
    const size_t result_len =
        bounded_string_length(response.result_json, sizeof(response.result_json));
    if (transcript_len >= sizeof(request->transcript) ||
        result_len >= sizeof(request->result_json)) {
      rc = H2_PAL_ERR_NO_MEMORY;
    } else {
      memcpy(request->transcript, response.transcript, transcript_len + 1u);
      memcpy(request->result_json, response.result_json, result_len + 1u);
    }
  }
  h2_gizclaw_rpc_response_deinit(client, &rpc_response);
  speech_request_destroy_rpc(request);
  return rc;
}

static h2_pal_result_t speech_request_start(
    void *user, h2_gizclaw_client_t *client,
    const h2_gizclaw_cancel_token_t *cancel_token) {
  h2_gizclaw_speech_extract_request_t *request = user;
  if (h2_gizclaw_cancel_requested(cancel_token))
    return H2_PAL_ERR_CLOSED;
  gizclaw_rpc_v1_SpeechExtractRequest params_message =
      gizclaw_rpc_v1_SpeechExtractRequest_init_zero;
  if (!copy_request_text(params_message.asr_model_name,
                         sizeof(params_message.asr_model_name),
                         request->options.asr_model_name, true) ||
      !copy_request_text(params_message.extract_model_name,
                         sizeof(params_message.extract_model_name),
                         request->options.extract_model_name, true) ||
      !copy_request_text(params_message.content_type,
                         sizeof(params_message.content_type),
                         request->options.content_type, true) ||
      !copy_request_text(params_message.language,
                         sizeof(params_message.language),
                         request->options.language, false) ||
      !copy_request_text(params_message.schema_json,
                         sizeof(params_message.schema_json),
                         request->options.schema_json, true) ||
      !copy_request_text(params_message.instruction,
                         sizeof(params_message.instruction),
                         request->options.instruction, false)) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  params_message.has_language = request->options.language.len > 0u;
  params_message.has_instruction = request->options.instruction.len > 0u;
  gzc_buf_t params;
  gzc_buf_init(&params);
  int rc = h2_gizclaw_encode_pb_message(
      client, gizclaw_rpc_v1_SpeechExtractRequest_fields, &params_message,
      &params);
  if (rc == GZC_OK) {
    rc = h2_gizclaw_client_rpc_request_start_stream(
        client, H2_GIZCLAW_RPC_SERVER_SPEECH_EXTRACT,
        (h2_gizclaw_rpc_bytes_t){.data = params.data, .len = params.len},
        30000u, speech_request_stream_event, request, &request->rpc_request);
  }
  gzc_buf_free(&params,
               gzc_client_platform(h2_gizclaw_client_gzc_internal(client)));
  if (rc != H2_PAL_OK && rc != GZC_OK)
    return rc;
  return speech_request_poll(user, client, cancel_token);
}

static void
speech_request_complete(void *user, h2_gizclaw_operation_t *operation,
                        const h2_gizclaw_operation_result_t *result) {
  (void)operation;
  h2_gizclaw_speech_extract_request_t *request = user;
  atomic_store_explicit(&request->terminal, true, memory_order_release);
  request->completion(request->completion_user, request, result,
                      (h2_gizclaw_str_t){.data = request->transcript,
                                         .len = strlen(request->transcript)},
                      (h2_gizclaw_str_t){.data = request->result_json,
                                         .len = strlen(request->result_json)});
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
  request->completion = completion;
  request->completion_user = user;
  if (!copy_owned_text(request->asr_model, sizeof(request->asr_model),
                       options->asr_model_name, true,
                       &request->options.asr_model_name) ||
      !copy_owned_text(request->extract_model, sizeof(request->extract_model),
                       options->extract_model_name, true,
                       &request->options.extract_model_name) ||
      !copy_owned_text(request->content_type, sizeof(request->content_type),
                       options->content_type, true,
                       &request->options.content_type) ||
      !copy_owned_text(request->language, sizeof(request->language),
                       options->language, false, &request->options.language) ||
      !copy_owned_text(request->schema, sizeof(request->schema),
                       options->schema_json, true,
                       &request->options.schema_json) ||
      !copy_owned_text(request->instruction, sizeof(request->instruction),
                       options->instruction, false,
                       &request->options.instruction)) {
    h2_pal_mem_free(allocator, request);
    return H2_PAL_ERR_INVALID_ARG;
  }
  const h2_pal_queue_config_t queue_config = {
      .name = "$gizclaw/rec-request",
      .item_size = sizeof(h2_gizclaw_speech_request_message_t),
      .item_count = H2_GIZCLAW_SPEECH_REQUEST_QUEUE_ITEMS,
      .allocator = allocator,
  };
  int rc = h2_pal_queue_create(service->config.queue, &queue_config,
                               &request->queue);
  if (rc == H2_PAL_QUEUE_OK) {
    rc = h2_gizclaw_service_submit_async_internal(
        service, identity, speech_request_start, speech_request_poll,
        speech_request_complete, request, &request->operation);
  }
  if (rc != H2_PAL_OK) {
    if (request->queue != NULL)
      h2_pal_queue_destroy(service->config.queue, request->queue);
    h2_pal_mem_free(allocator, request);
    return rc;
  }
  *out_request = request;
  return H2_PAL_OK;
}

int h2_gizclaw_speech_extract_request_write_opus(
    h2_gizclaw_speech_extract_request_t *request, const uint8_t *opus,
    size_t opus_len) {
  if (request == NULL || opus == NULL || opus_len == 0u ||
      opus_len > H2_GIZCLAW_SPEECH_OPUS_MAX_BYTES)
    return H2_PAL_ERR_INVALID_ARG;
  h2_pal_result_t rc =
      h2_pal_mutex_lock(request->service->config.sync, request->service->mutex);
  if (rc != H2_PAL_OK)
    return rc;
  if (atomic_load_explicit(&request->committed, memory_order_acquire) ||
      atomic_load_explicit(&request->terminal, memory_order_acquire)) {
    rc = H2_PAL_ERR_CLOSED;
  } else {
    h2_gizclaw_speech_request_message_t message = {.len = opus_len};
    memcpy(message.opus, opus, opus_len);
    rc = (h2_pal_result_t)h2_pal_queue_send(request->service->config.queue,
                                            request->queue, &message,
                                            H2_PAL_QUEUE_NO_WAIT);
  }
  (void)h2_pal_mutex_unlock(request->service->config.sync,
                            request->service->mutex);
  return rc;
}

int h2_gizclaw_speech_extract_request_commit(
    h2_gizclaw_speech_extract_request_t *request) {
  if (request == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  h2_pal_result_t rc =
      h2_pal_mutex_lock(request->service->config.sync, request->service->mutex);
  if (rc != H2_PAL_OK)
    return rc;
  if (atomic_load_explicit(&request->committed, memory_order_acquire) ||
      atomic_load_explicit(&request->terminal, memory_order_acquire)) {
    rc = H2_PAL_ERR_CLOSED;
  } else {
    const h2_gizclaw_speech_request_message_t message = {0};
    rc = (h2_pal_result_t)h2_pal_queue_send(request->service->config.queue,
                                            request->queue, &message,
                                            H2_PAL_QUEUE_NO_WAIT);
    if (rc == H2_PAL_OK)
      atomic_store_explicit(&request->committed, true, memory_order_release);
  }
  (void)h2_pal_mutex_unlock(request->service->config.sync,
                            request->service->mutex);
  return rc;
}

int h2_gizclaw_speech_extract_request_cancel(
    h2_gizclaw_speech_extract_request_t *request) {
  if (request == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  return h2_gizclaw_operation_cancel(request->operation);
}

void h2_gizclaw_speech_extract_request_release(
    h2_gizclaw_speech_extract_request_t *request) {
  if (request == NULL ||
      !atomic_load_explicit(&request->terminal, memory_order_acquire))
    return;
  h2_gizclaw_operation_release(request->operation);
  h2_pal_queue_destroy(request->service->config.queue, request->queue);
  h2_pal_mem_free(request->service->config.client_config->allocator, request);
}
