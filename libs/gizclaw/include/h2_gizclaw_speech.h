#ifndef H2_GIZCLAW_SPEECH_H
#define H2_GIZCLAW_SPEECH_H

#include "h2_gizclaw_service.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct h2_gizclaw_speech_transcribe_options {
  h2_gizclaw_str_t model_name;
  h2_gizclaw_str_t content_type;
  h2_gizclaw_str_t language;
} h2_gizclaw_speech_transcribe_options_t;

typedef struct h2_gizclaw_speech_extract_options {
  h2_gizclaw_str_t asr_model_name;
  h2_gizclaw_str_t extract_model_name;
  h2_gizclaw_str_t content_type;
  h2_gizclaw_str_t language;
  h2_gizclaw_str_t schema_json;
  h2_gizclaw_str_t instruction;
} h2_gizclaw_speech_extract_options_t;

typedef struct h2_gizclaw_speech_transcribe_response {
  h2_gizclaw_str_t transcript;
} h2_gizclaw_speech_transcribe_response_t;

typedef struct h2_gizclaw_speech_extract_response {
  h2_gizclaw_str_t transcript;
  h2_gizclaw_str_t result_json;
} h2_gizclaw_speech_extract_response_t;

/** Copy options without I/O. do reserves the bound PCM Track's uplink route;
 * service_audio_start explicitly starts capture. A conflicting route rejects
 * do without replacing its owner. content_type must describe the
 * Track's actual PCM format; no microphone or codec is created by this API.
 * service_audio_end freezes the current PCM prefix, drains it and sends EOS.
 * wait alone does not end recording. timeout_ms bounds the network operation.
 */
h2_pal_result_t h2_gizclaw_req_create_speech_transcribe(
    h2_gizclaw_service_t *service, uint64_t identity,
    const h2_gizclaw_speech_transcribe_options_t *options, uint32_t timeout_ms,
    h2_gizclaw_req_t **out_request);
h2_pal_result_t h2_gizclaw_req_create_speech_extract(
    h2_gizclaw_service_t *service, uint64_t identity,
    const h2_gizclaw_speech_extract_options_t *options, uint32_t timeout_ms,
    h2_gizclaw_req_t **out_request);

/** Parse a successful terminal response into caller-owned storage. Views remain
 * valid after request release / Service deinit. Failure clears out_response
 * and leaves storage->used unchanged. */
h2_pal_result_t h2_gizclaw_resp_parse_speech_transcribe(
    const h2_gizclaw_req_t *request, h2_gizclaw_resp_storage_t *storage,
    h2_gizclaw_speech_transcribe_response_t *out_response);
h2_pal_result_t h2_gizclaw_resp_parse_speech_extract(
    const h2_gizclaw_req_t *request, h2_gizclaw_resp_storage_t *storage,
    h2_gizclaw_speech_extract_response_t *out_response);

#ifdef __cplusplus
}
#endif
#endif
