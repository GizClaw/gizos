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

/** One ordered result emitted by a conversation generation. */
typedef enum h2_gizclaw_conversation_event_kind {
  H2_GIZCLAW_CONVERSATION_EVENT_NONE = 0,
  /** The first decoded chunk of a reply reached the bound PCM Track. */
  H2_GIZCLAW_CONVERSATION_EVENT_REPLY_AUDIO_STARTED,
  H2_GIZCLAW_CONVERSATION_EVENT_TEXT_DELTA,
  H2_GIZCLAW_CONVERSATION_EVENT_TEXT_DONE,
  H2_GIZCLAW_CONVERSATION_EVENT_REPLY_DONE,
  H2_GIZCLAW_CONVERSATION_EVENT_ERROR,
} h2_gizclaw_conversation_event_kind_t;

/**
 * One event delivered in service_poll() context. Views are borrowed only for
 * the duration of the callback. `generation` identifies the current begin/end
 * cycle. Reply PCM (signed PCM16LE, 16 kHz mono) never travels through
 * events: the decoder writes every chunk to the service's downlink Track and
 * the application's speaker pump reads it there. Per reply the hook observes
 * at most one REPLY_AUDIO_STARTED, then the text events, then exactly one
 * REPLY_DONE or ERROR, so the number of notifications does not grow with the
 * reply length. A reply that ends before any audio was decoded produces no
 * REPLY_AUDIO_STARTED. Completion follows draining accepted playback.
 * REPLY_DONE marks one server reply, not necessarily the end of the begin/end
 * cycle: while input remains open, server-side VAD may produce further replies,
 * each with its own REPLY_AUDIO_STARTED.
 * A reply the server cuts short because the user spoke over it (barge-in)
 * also ends with REPLY_DONE while input remains open; its queued playback is
 * discarded. After the input is committed such an interruption is ERROR.
 * TEXT_DONE may have empty text when earlier TEXT_DELTA events carried it.
 */
typedef struct h2_gizclaw_conversation_event {
  h2_gizclaw_conversation_event_kind_t kind;
  uint64_t generation;
  const char *text;
  size_t text_len;
  const char *error_code;
  bool retryable;
} h2_gizclaw_conversation_event_t;

/** Optional service_poll() hook. Return OK to continue; any other result aborts
 * this generation. This is an observation hook, not a retryable audio sink.
 * Network I/O and other requests continue while the hook runs. */
typedef h2_pal_result_t (*h2_gizclaw_conversation_callback_fn)(
    void *user, h2_gizclaw_conversation_t *conversation,
    const h2_gizclaw_conversation_event_t *event);

typedef void (*h2_gizclaw_conversation_completion_fn)(
    void *user, h2_gizclaw_conversation_t *conversation,
    const h2_gizclaw_operation_result_t *result);

/** Configure the Service's conversation route without starting recording.
 * One configured Conversation per Service; another create returns BUSY.
 * Use service_audio_start to start recording. For PTT, service_audio_end
 * submits the input and the generation waits for its reply. Realtime is a
 * continuous call: reply EOS ends a VAD round, not the call; use
 * conversation_cancel to hang up without waiting for another reply EOS.
 * Speech may use the same Service while this Conversation is idle. */
h2_pal_result_t h2_gizclaw_conversation_create(
    h2_gizclaw_service_t *service, h2_gizclaw_str_t workspace,
    h2_gizclaw_conversation_callback_fn callback,
    h2_gizclaw_conversation_completion_fn completion, void *user,
    h2_gizclaw_conversation_t **out_conversation);

/** Cancel the active generation (hang up a Realtime call), without closing
 * the Service or Peer. Completion is still delivered by poll. */
h2_pal_result_t
h2_gizclaw_conversation_cancel(h2_gizclaw_conversation_t *conversation);

/** Release an idle conversation. Active audio must first reach terminal. */
void h2_gizclaw_conversation_release(h2_gizclaw_conversation_t *conversation);

#ifdef __cplusplus
}
#endif

#endif
