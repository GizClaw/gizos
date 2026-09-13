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

/** One result emitted while a conversation generation's input is active. */
typedef enum h2_gizclaw_conversation_event_kind {
  H2_GIZCLAW_CONVERSATION_EVENT_NONE = 0,
  H2_GIZCLAW_CONVERSATION_EVENT_TEXT_DELTA,
  H2_GIZCLAW_CONVERSATION_EVENT_TEXT_DONE,
  /** The server refused this generation's input. */
  H2_GIZCLAW_CONVERSATION_EVENT_ERROR,
} h2_gizclaw_conversation_event_kind_t;

/**
 * One event delivered in service_poll() context. Views are borrowed only for
 * the duration of the callback. `generation` identifies the current begin/end
 * cycle, which covers one input only: a push-to-talk generation completes
 * once its input end is on the wire, a realtime one when it is canceled.
 * Downstream audio is not part of any generation and never travels through
 * events: whatever the server sends is decoded into the service's downlink
 * Track, whatever its stream IDs, BOS or EOS, and the application's speaker
 * pump plays it from there. Ending push-to-talk input clears the audio
 * buffered at that moment. Server text is forwarded while the input is
 * active and carries no state. ERROR reports that the server refused this
 * input; the end of a downstream stream, with or without an error code, is
 * never an error. TEXT_DONE may have empty text when earlier TEXT_DELTA
 * events carried it.
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
 * Downstream audio belongs to the connection: the first route starts it and
 * it keeps playing after a route is released. Use service_audio_start to
 * start recording. For PTT, service_audio_end submits
 * the input, clears buffered downstream audio and completes the generation
 * once the input end is sent. Realtime is a continuous call; use
 * conversation_cancel to hang up. Speech may use the same Service while
 * this Conversation is idle. */
h2_pal_result_t h2_gizclaw_conversation_create(
    h2_gizclaw_service_t *service, h2_gizclaw_str_t workspace,
    h2_gizclaw_conversation_callback_fn callback,
    h2_gizclaw_conversation_completion_fn completion, void *user,
    h2_gizclaw_conversation_t **out_conversation);

/** Submit one complete UTF-8 user input on the configured conversation route.
 * The caller must first activate the desired Workspace on the Service.
 * Copies text before returning; no terminator is required in the supplied span.
 * Like service_audio_end, OK means asynchronous admission, not server acceptance
 * or an Agent reply. The existing completion hook runs exactly once from
 * service_poll() when input is sent, fails, or is canceled. Admission errors do
 * not call completion. Server audio uses the existing connection downlink;
 * text observations retain the existing active-input callback lifetime.
 *
 * INVALID_ARG: NULL handle/data, empty text, more than
 * H2_GIZCLAW_CONVERSATION_TEXT_MAX_BYTES bytes,
 * embedded NUL, or invalid UTF-8. INVALID_STATE: Service not started.
 * CLOSED: Service stopping/stopped. BUSY: an input awaits completion, recording
 * is active, or Speech/audio playback owns the route. Recording is never
 * interrupted. NO_MEMORY and WOULD_BLOCK report allocation/admission failure.
 * A started Service may still be connecting, as with service_audio_start;
 * connection/send failures are reported asynchronously through completion.
 *
 * Control calls serialize with audio start/end/cancel. Keep the handle alive
 * until completion; serialize release with callers, as for other route APIs.
 * No network I/O or application callback runs inline. No PCM Track is required
 * for text input. Audio start is unavailable until this input completes.
 */
h2_pal_result_t h2_gizclaw_conversation_send_text(
    h2_gizclaw_conversation_t *conversation, h2_gizclaw_str_t text);

/** Cancel the active generation (hang up a Realtime call), without closing
 * the Service or Peer, and stop buffered downstream audio. Completion is
 * still delivered by poll. */
h2_pal_result_t
h2_gizclaw_conversation_cancel(h2_gizclaw_conversation_t *conversation);

/** Release an idle conversation. Active audio must first reach terminal. */
void h2_gizclaw_conversation_release(h2_gizclaw_conversation_t *conversation);

#ifdef __cplusplus
}
#endif

#endif
