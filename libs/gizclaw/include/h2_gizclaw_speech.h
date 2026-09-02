#ifndef H2_GIZCLAW_SPEECH_H
#define H2_GIZCLAW_SPEECH_H

#include "h2_gizclaw_config.h"
#include "h2_gizclaw_service.h"
#include "h2_gizclaw_types.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct h2_gizclaw_speech_upload h2_gizclaw_speech_upload_t;
typedef struct h2_gizclaw_speech_request h2_gizclaw_speech_extract_request_t;
typedef struct h2_gizclaw_speech_request h2_gizclaw_speech_transcribe_request_t;
typedef h2_gizclaw_request_t h2_gizclaw_asr_request_t;

#define H2_GIZCLAW_SPEECH_AUDIO_CHUNK_MAX_BYTES 1280u

typedef void (*h2_gizclaw_speech_extract_request_completion_fn)(
    void *user, h2_gizclaw_speech_extract_request_t *request);

typedef void (*h2_gizclaw_speech_transcribe_request_completion_fn)(
    void *user, h2_gizclaw_speech_transcribe_request_t *request);

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

/**
 * Consume one successful speech extraction result synchronously.
 *
 * Both strings are borrowed and remain valid only until the callback returns.
 */
typedef int (*h2_gizclaw_speech_extract_result_fn)(
    void *user, h2_gizclaw_str_t transcript, h2_gizclaw_str_t result_json);

/**
 * Open an incremental server.speech.transcribe upload.
 *
 * The returned upload is owned by the caller until finish or cancel consumes
 * it. All functions must run on the GizClaw client owner task.
 */
#if defined(H2_GIZCLAW_TESTING)
int h2_gizclaw_client_speech_transcribe_open(
    h2_gizclaw_client_t *client,
    const h2_gizclaw_speech_transcribe_options_t *options,
    h2_gizclaw_speech_upload_t **out_upload);

/** Write one borrowed audio chunk to an open transcription upload. */
int h2_gizclaw_speech_transcribe_write(h2_gizclaw_speech_upload_t *upload,
                                       const uint8_t *data, size_t len);

/**
 * Finish an upload and copy the NUL-terminated transcript to caller storage.
 *
 * After argument validation succeeds, finish consumes the upload whether the
 * remote operation succeeds or fails.
 */
int h2_gizclaw_speech_transcribe_finish(h2_gizclaw_speech_upload_t *upload,
                                        char *out_transcript,
                                        size_t transcript_capacity,
                                        size_t *out_transcript_len);

/** Cancel and consume an open upload. A NULL upload is ignored. */
void h2_gizclaw_speech_transcribe_cancel(h2_gizclaw_speech_upload_t *upload);

/**
 * Open an incremental server.speech.extract upload.
 *
 * The returned upload is owned by the caller until finish or cancel consumes
 * it. All functions must run on the GizClaw client owner task.
 */
int h2_gizclaw_client_speech_extract_open(
    h2_gizclaw_client_t *client,
    const h2_gizclaw_speech_extract_options_t *options,
    h2_gizclaw_speech_upload_t **out_upload);

/** Write one borrowed audio chunk to an open extraction upload. */
int h2_gizclaw_speech_extract_write(h2_gizclaw_speech_upload_t *upload,
                                    const uint8_t *data, size_t len);

/**
 * Finish an extraction upload and synchronously deliver its borrowed result.
 *
 * After argument validation succeeds, finish consumes the upload whether the
 * remote operation or result callback succeeds or fails.
 */
int h2_gizclaw_speech_extract_finish(
    h2_gizclaw_speech_upload_t *upload,
    h2_gizclaw_speech_extract_result_fn on_result, void *user);

/** Cancel and consume an open extraction upload. A NULL upload is ignored. */
void h2_gizclaw_speech_extract_cancel(h2_gizclaw_speech_upload_t *upload);
#endif

/** Create one task-safe, service-owned incremental transcription request. */
#if defined(H2_GIZCLAW_TESTING)
int h2_gizclaw_service_speech_transcribe_create(
    h2_gizclaw_service_t *service, uint64_t identity,
    const h2_gizclaw_speech_transcribe_options_t *options,
    h2_gizclaw_speech_transcribe_request_completion_fn completion, void *user,
    h2_gizclaw_speech_transcribe_request_t **out_request);
#endif

/** Create an ASR request without starting network I/O. */
h2_pal_result_t h2_gizclaw_asr_request_create(
    h2_gizclaw_service_t *service, uint64_t identity,
    const h2_gizclaw_speech_transcribe_options_t *options,
    h2_gizclaw_asr_request_t **out_request);

/** Borrow the terminal transcript, or return an empty view while pending. */
h2_gizclaw_str_t
h2_gizclaw_asr_request_response(const h2_gizclaw_asr_request_t *request);

#if defined(H2_GIZCLAW_TESTING)
int h2_gizclaw_speech_transcribe_request_write_audio(
    h2_gizclaw_speech_transcribe_request_t *request, const uint8_t *audio,
    size_t audio_len, uint32_t timeout_ms);

int h2_gizclaw_speech_transcribe_request_commit(
    h2_gizclaw_speech_transcribe_request_t *request, uint32_t timeout_ms);

int h2_gizclaw_speech_transcribe_request_cancel(
    h2_gizclaw_speech_transcribe_request_t *request);
int h2_gizclaw_speech_transcribe_request_wait(
    h2_gizclaw_speech_transcribe_request_t *request, uint32_t timeout_ms);
const h2_gizclaw_operation_result_t *
h2_gizclaw_speech_transcribe_request_operation_result(
    const h2_gizclaw_speech_transcribe_request_t *request);
h2_gizclaw_str_t h2_gizclaw_speech_transcribe_request_transcript(
    const h2_gizclaw_speech_transcribe_request_t *request);

void h2_gizclaw_speech_transcribe_request_release(
    h2_gizclaw_speech_transcribe_request_t *request);

/**
 * Create one task-safe, service-owned incremental extraction request.
 *
 * The service worker owns all client calls. Callers may feed audio chunks that
 * match `options.content_type` from another task and then commit or cancel the
 * request. Options are copied before this function returns. Completion runs
 * from service dispatch.
 */
int h2_gizclaw_service_speech_extract_create(
    h2_gizclaw_service_t *service, uint64_t identity,
    const h2_gizclaw_speech_extract_options_t *options,
    h2_gizclaw_speech_extract_request_completion_fn completion, void *user,
    h2_gizclaw_speech_extract_request_t **out_request);

/** Queue one audio chunk, waiting at most `timeout_ms` for backpressure. */
int h2_gizclaw_speech_extract_request_write_audio(
    h2_gizclaw_speech_extract_request_t *request, const uint8_t *audio,
    size_t audio_len, uint32_t timeout_ms);

/** Queue EOS after every previously accepted audio chunk. */
int h2_gizclaw_speech_extract_request_commit(
    h2_gizclaw_speech_extract_request_t *request, uint32_t timeout_ms);

/** Request task-safe cooperative cancellation. */
int h2_gizclaw_speech_extract_request_cancel(
    h2_gizclaw_speech_extract_request_t *request);
int h2_gizclaw_speech_extract_request_wait(
    h2_gizclaw_speech_extract_request_t *request, uint32_t timeout_ms);
const h2_gizclaw_operation_result_t *
h2_gizclaw_speech_extract_request_operation_result(
    const h2_gizclaw_speech_extract_request_t *request);
h2_gizclaw_str_t h2_gizclaw_speech_extract_request_transcript(
    const h2_gizclaw_speech_extract_request_t *request);
h2_gizclaw_str_t h2_gizclaw_speech_extract_request_result_json(
    const h2_gizclaw_speech_extract_request_t *request);

/** Release a terminal request after its completion callback returns. */
void h2_gizclaw_speech_extract_request_release(
    h2_gizclaw_speech_extract_request_t *request);
#endif

#ifdef __cplusplus
}
#endif

#endif
