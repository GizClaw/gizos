#include "h2_gizclaw_speech.h"
#include "h2_gizclaw_internal.h"

#include "gzc_common.h"
#include "gzc_rpc.h"
#include "payload/ai.pb.h"

#include <stdbool.h>
#include <string.h>

typedef enum h2_gizclaw_speech_upload_kind {
  H2_GIZCLAW_SPEECH_UPLOAD_TRANSCRIBE = 0,
  H2_GIZCLAW_SPEECH_UPLOAD_EXTRACT,
} h2_gizclaw_speech_upload_kind_t;

struct h2_gizclaw_speech_upload {
  const h2_pal_mem_api_t *allocator;
  h2_gizclaw_client_t *client;
  gzc_rpc_speech_upload_t *gzc_upload;
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
  if (upload->gzc_upload != NULL) {
    if (upload->kind == H2_GIZCLAW_SPEECH_UPLOAD_EXTRACT)
      gzc_rpc_speech_extract_cancel(upload->gzc_upload);
    else
      gzc_rpc_speech_transcribe_cancel(upload->gzc_upload);
  }
  free_upload(upload);
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

  h2_gizclaw_speech_upload_t *upload =
      allocate_upload(client, H2_GIZCLAW_SPEECH_UPLOAD_TRANSCRIBE);
  if (upload == NULL)
    return H2_PAL_ERR_NO_MEMORY;

  const int rc = gzc_rpc_speech_transcribe_open(
      h2_gizclaw_client_gzc_internal(client), &request, &upload->gzc_upload);
  if (rc != GZC_OK) {
    free_upload(upload);
    return result_from_gzc(rc);
  }
  *out_upload = upload;
  return H2_PAL_OK;
}

int h2_gizclaw_speech_transcribe_write(h2_gizclaw_speech_upload_t *upload,
                                       const uint8_t *data, size_t len) {
  if (upload == NULL || upload->kind != H2_GIZCLAW_SPEECH_UPLOAD_TRANSCRIBE ||
      upload->gzc_upload == NULL || data == NULL || len == 0u) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  return result_from_gzc(
      gzc_rpc_speech_transcribe_write(upload->gzc_upload, data, len));
}

int h2_gizclaw_speech_transcribe_finish(h2_gizclaw_speech_upload_t *upload,
                                        char *out_transcript,
                                        size_t transcript_capacity,
                                        size_t *out_transcript_len) {
  if (upload == NULL || upload->kind != H2_GIZCLAW_SPEECH_UPLOAD_TRANSCRIBE ||
      upload->gzc_upload == NULL || out_transcript == NULL ||
      transcript_capacity == 0u || out_transcript_len == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  out_transcript[0] = '\0';
  *out_transcript_len = 0u;
  const h2_pal_mem_api_t *allocator = upload->allocator;
  gizclaw_rpc_v1_SpeechTranscribeResponse *response =
      h2_pal_mem_alloc(allocator, sizeof(*response));
  if (response == NULL) {
    gzc_rpc_speech_transcribe_cancel(upload->gzc_upload);
    free_upload(upload);
    return H2_PAL_ERR_NO_MEMORY;
  }
  *response = (gizclaw_rpc_v1_SpeechTranscribeResponse)
      gizclaw_rpc_v1_SpeechTranscribeResponse_init_zero;
  gzc_rpc_error_t error = {0};
  const int gzc_rc =
      gzc_rpc_speech_transcribe_finish(upload->gzc_upload, response, &error);
  upload->gzc_upload = NULL;
  if (gzc_rc == GZC_ERR_RPC) {
    h2_gizclaw_client_log_rpc_error_internal(
        upload->client, H2_GIZCLAW_RPC_SERVER_SPEECH_TRANSCRIBE, error.code,
        error.message.data, error.message.len);
  }
  int rc = result_from_gzc(gzc_rc);
  if (rc == H2_PAL_OK) {
    const size_t len = bounded_string_length(response->transcript,
                                             sizeof(response->transcript));
    if (len >= sizeof(response->transcript) || len >= transcript_capacity) {
      rc = H2_PAL_ERR_NO_MEMORY;
    } else {
      memcpy(out_transcript, response->transcript, len + 1u);
      *out_transcript_len = len;
    }
  }
  h2_pal_mem_free(allocator, response);
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

  h2_gizclaw_speech_upload_t *upload =
      allocate_upload(client, H2_GIZCLAW_SPEECH_UPLOAD_EXTRACT);
  if (upload == NULL)
    return H2_PAL_ERR_NO_MEMORY;
  const int rc = gzc_rpc_speech_extract_open(
      h2_gizclaw_client_gzc_internal(client), &request, &upload->gzc_upload);
  if (rc != GZC_OK) {
    free_upload(upload);
    return result_from_gzc(rc);
  }
  *out_upload = upload;
  return H2_PAL_OK;
}

int h2_gizclaw_speech_extract_write(h2_gizclaw_speech_upload_t *upload,
                                    const uint8_t *data, size_t len) {
  if (upload == NULL || upload->kind != H2_GIZCLAW_SPEECH_UPLOAD_EXTRACT ||
      upload->gzc_upload == NULL || data == NULL || len == 0u) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  return result_from_gzc(
      gzc_rpc_speech_extract_write(upload->gzc_upload, data, len));
}

int h2_gizclaw_speech_extract_finish(
    h2_gizclaw_speech_upload_t *upload,
    h2_gizclaw_speech_extract_result_fn on_result, void *user) {
  if (upload == NULL || upload->kind != H2_GIZCLAW_SPEECH_UPLOAD_EXTRACT ||
      upload->gzc_upload == NULL || on_result == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  const h2_pal_mem_api_t *allocator = upload->allocator;
  gizclaw_rpc_v1_SpeechExtractResponse *response =
      h2_pal_mem_alloc(allocator, sizeof(*response));
  if (response == NULL) {
    gzc_rpc_speech_extract_cancel(upload->gzc_upload);
    free_upload(upload);
    return H2_PAL_ERR_NO_MEMORY;
  }
  *response = (gizclaw_rpc_v1_SpeechExtractResponse)
      gizclaw_rpc_v1_SpeechExtractResponse_init_zero;
  gzc_rpc_error_t error = {0};
  const int gzc_rc =
      gzc_rpc_speech_extract_finish(upload->gzc_upload, response, &error);
  upload->gzc_upload = NULL;
  if (gzc_rc == GZC_ERR_RPC) {
    h2_gizclaw_client_log_rpc_error_internal(
        upload->client, H2_GIZCLAW_RPC_SERVER_SPEECH_EXTRACT, error.code,
        error.message.data, error.message.len);
  }
  int rc = result_from_gzc(gzc_rc);
  if (rc == H2_PAL_OK) {
    const size_t transcript_len = bounded_string_length(
        response->transcript, sizeof(response->transcript));
    const size_t result_json_len = bounded_string_length(
        response->result_json, sizeof(response->result_json));
    if (transcript_len >= sizeof(response->transcript) ||
        result_json_len >= sizeof(response->result_json)) {
      rc = H2_PAL_ERR_FORMAT;
    } else {
      rc = on_result(user,
                     (h2_gizclaw_str_t){.data = response->transcript,
                                        .len = transcript_len},
                     (h2_gizclaw_str_t){.data = response->result_json,
                                        .len = result_json_len});
    }
  }
  h2_pal_mem_free(allocator, response);
  free_upload(upload);
  return rc;
}

void h2_gizclaw_speech_extract_cancel(h2_gizclaw_speech_upload_t *upload) {
  cancel_upload(upload);
}
