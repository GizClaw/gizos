#ifndef H2_GIZCLAW_SPEECH_H
#define H2_GIZCLAW_SPEECH_H

#include "h2_gizclaw_config.h"
#include "h2_gizclaw_types.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct h2_gizclaw_speech_upload h2_gizclaw_speech_upload_t;

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

#ifdef __cplusplus
}
#endif

#endif
