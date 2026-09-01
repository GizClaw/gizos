#ifndef H2_GIZCLAW_CONVERSATION_H
#define H2_GIZCLAW_CONVERSATION_H

#include "h2_gizclaw_config.h"
#include "h2_gizclaw_service.h"
#include "h2_gizclaw_types.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define H2_GIZCLAW_CONVERSATION_OPUS_MAX_BYTES 1275u
#define H2_GIZCLAW_CONVERSATION_PCM_CHUNK_MAX_BYTES 1280u
#define H2_GIZCLAW_CONVERSATION_PCM_SAMPLE_RATE_HZ 16000u
#define H2_GIZCLAW_CONVERSATION_PCM_CHANNELS 1u
#define H2_GIZCLAW_CONVERSATION_TEXT_MAX_BYTES 4096u
#define H2_GIZCLAW_CONVERSATION_STREAM_ID_MAX_BYTES 63u

typedef struct h2_gizclaw_conversation h2_gizclaw_conversation_t;
typedef struct h2_gizclaw_conversation_request
    h2_gizclaw_conversation_request_t;

/** One ordered result emitted by a conversation generation. */
typedef enum h2_gizclaw_conversation_event_kind {
  H2_GIZCLAW_CONVERSATION_EVENT_NONE = 0,
  H2_GIZCLAW_CONVERSATION_EVENT_REPLY_AUDIO,
  H2_GIZCLAW_CONVERSATION_EVENT_TEXT_DELTA,
  H2_GIZCLAW_CONVERSATION_EVENT_TEXT_DONE,
  H2_GIZCLAW_CONVERSATION_EVENT_REPLY_DONE,
  H2_GIZCLAW_CONVERSATION_EVENT_ERROR,
} h2_gizclaw_conversation_event_kind_t;

/**
 * Result of one poll.
 *
 * `generation` is the caller-provided generation from `open`. `audio` contains
 * signed 16-bit little-endian, 16 kHz mono PCM for `REPLY_AUDIO`. Audio, text,
 * and error views are
 * borrowed until the next poll or deinit. `REPLY_DONE` is the service terminal;
 * callers must still drain locally accepted playback before publishing their
 * own conversation completion.
 */
typedef struct h2_gizclaw_conversation_event {
  h2_gizclaw_conversation_event_kind_t kind;
  uint64_t generation;
  const uint8_t *audio;
  size_t audio_len;
  const char *text;
  size_t text_len;
  const char *error_code;
  bool retryable;
} h2_gizclaw_conversation_event_t;

typedef h2_pal_result_t (*h2_gizclaw_conversation_request_event_fn)(
    void *user, h2_gizclaw_conversation_request_t *request,
    const h2_gizclaw_conversation_event_t *event);

typedef void (*h2_gizclaw_conversation_request_completion_fn)(
    void *user, h2_gizclaw_conversation_request_t *request);

/** Create one service-owned conversation generation. */
h2_pal_result_t h2_gizclaw_service_conversation_create(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_str_t workspace_name, uint64_t generation, int timeout_ms,
    h2_gizclaw_conversation_request_event_fn on_event,
    h2_gizclaw_conversation_request_completion_fn completion, void *user,
    h2_gizclaw_conversation_request_t **out_request);

/**
 * Try to append one 16 kHz mono S16LE PCM chunk to the SPSC uplink ring.
 *
 * This call never waits. `H2_PAL_ERR_WOULD_BLOCK` means the chunk was not
 * accepted and the realtime producer may drop it rather than accumulating
 * latency. The producer that writes PCM also owns `commit()` ordering.
 */
h2_pal_result_t h2_gizclaw_conversation_request_write_pcm(
    h2_gizclaw_conversation_request_t *request, const uint8_t *pcm,
    size_t pcm_len);

/** Publish EOS after all previously accepted PCM bytes. This call never waits.
 */
h2_pal_result_t h2_gizclaw_conversation_request_commit(
    h2_gizclaw_conversation_request_t *request);

h2_pal_result_t h2_gizclaw_conversation_request_cancel(
    h2_gizclaw_conversation_request_t *request);
h2_pal_result_t
h2_gizclaw_conversation_request_wait(h2_gizclaw_conversation_request_t *request,
                                     uint32_t timeout_ms);
const h2_gizclaw_operation_result_t *
h2_gizclaw_conversation_request_operation_result(
    const h2_gizclaw_conversation_request_t *request);

void h2_gizclaw_conversation_request_release(
    h2_gizclaw_conversation_request_t *request);

/**
 * Opens one generation for an already active Workspace.
 *
 * The returned conversation borrows the client-owned Peer Event access handle.
 * Only one conversation may hold a logical lease for a client at a time; a
 * simultaneous open returns `H2_PAL_ERR_INVALID_STATE`. Success sends the input
 * BOS and transfers ownership of the returned conversation to the caller. The
 * caller must eventually call `deinit`. Conversation operations are not thread
 * safe; one caller must own and serialize open, input, poll, commit, cancel,
 * and deinit for the complete lifetime.
 */
#if defined(H2_GIZCLAW_TESTING)
int h2_gizclaw_conversation_open(h2_gizclaw_client_t *client,
                                 h2_gizclaw_str_t workspace_name,
                                 uint64_t generation, int timeout_ms,
                                 h2_gizclaw_conversation_t **out_conversation);

/** True while the generation accepts encoded Opus input. */
bool h2_gizclaw_conversation_input_ready(
    const h2_gizclaw_conversation_t *conversation);

/**
 * Sends one complete caller-encoded Opus packet.
 *
 * GizClaw does not read microphones or encode PCM. The caller owns capture,
 * framing, Opus encoder state, and packet lifetime. The packet is borrowed
 * only for this call. `timestamp_ms` remains for source compatibility and is
 * not serialized into the media payload. `H2_PAL_ERR_WOULD_BLOCK` means the
 * packet was not accepted and the caller must retain it, wait for transport
 * progress, and retry the same packet before advancing its input stream.
 */
int h2_gizclaw_conversation_write_opus(h2_gizclaw_conversation_t *conversation,
                                       const uint8_t *opus, size_t opus_len,
                                       uint64_t timestamp_ms);

/**
 * Ends encoded Opus input after all accepted packets have been sent.
 *
 * The operation is idempotent after success. `H2_PAL_ERR_WOULD_BLOCK` means
 * the EOS boundary was not accepted and must be retried.
 */
int h2_gizclaw_conversation_commit(h2_gizclaw_conversation_t *conversation,
                                   uint64_t timestamp_ms);

/**
 * Polls the ordered reply stream.
 *
 * A successful poll can return `NONE` for a protocol event with no app-facing
 * projection or for an event belonging to an earlier logical stream. Timeout
 * and would-block are non-terminal; `ERROR` and `REPLY_DONE` are terminal
 * app-facing events.
 */
int h2_gizclaw_conversation_poll(h2_gizclaw_conversation_t *conversation,
                                 int timeout_ms,
                                 h2_gizclaw_conversation_event_t *out_event);

/**
 * Invalidates the generation and prevents future input.
 *
 * This operation is idempotent. A caller must still call `deinit`.
 */
void h2_gizclaw_conversation_cancel(h2_gizclaw_conversation_t *conversation);

/**
 * Cancel if needed and release the logical Event lease.
 *
 * The client-owned Event access handle and physical Peer Event transport stay
 * open until the complete client connection is closed.
 */
void h2_gizclaw_conversation_deinit(h2_gizclaw_conversation_t *conversation);
#endif

#ifdef __cplusplus
}
#endif

#endif
