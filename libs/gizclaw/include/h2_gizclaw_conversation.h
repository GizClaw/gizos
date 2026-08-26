#ifndef H2_GIZCLAW_CONVERSATION_H
#define H2_GIZCLAW_CONVERSATION_H

#include "h2_gizclaw_config.h"
#include "h2_gizclaw_types.h"
#include "h2/pal/hal/h2_pal_audio.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define H2_GIZCLAW_CONVERSATION_OPUS_MAX_BYTES 1275u
#define H2_GIZCLAW_CONVERSATION_TEXT_MAX_BYTES 4096u
#define H2_GIZCLAW_CONVERSATION_STREAM_ID_MAX_BYTES 63u

typedef struct h2_gizclaw_conversation h2_gizclaw_conversation_t;

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
 * `generation` is the caller-provided generation from `open`. Audio, text, and
 * error views are borrowed until the next poll or deinit. `REPLY_DONE` is the
 * service terminal; callers must still drain locally accepted playback before
 * publishing their own conversation completion.
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

/**
 * Opens one generation for an already active Workspace.
 *
 * The returned conversation borrows the client-owned Peer Event access handle.
 * Only one conversation may hold a logical lease for a client at a time; a
 * simultaneous open returns `H2_PAL_ERR_INVALID_STATE`. Success sends the input
 * BOS and transfers ownership of the returned conversation to the caller. The
 * caller must eventually call `deinit`. Conversation operations are not thread
 * safe; one caller must own and serialize open, input, poll, commit, cancel, and
 * deinit for the complete lifetime.
 */
int h2_gizclaw_conversation_open(h2_gizclaw_client_t *client,
                                 h2_gizclaw_str_t workspace_name,
                                 uint64_t generation, int timeout_ms,
                                 h2_gizclaw_conversation_t **out_conversation);

/** True while the generation accepts microphone input. */
bool h2_gizclaw_conversation_input_ready(
    const h2_gizclaw_conversation_t *conversation);

/**
 * Selects conversation-owned PCM input and copies its provider format.
 *
 * The format must describe S16LE mono or stereo PCM at an Opus-supported
 * sample rate. `frame_samples_per_channel` is the largest complete provider
 * chunk accepted by one `write_pcm` call; it is not an Opus frame duration.
 * Configure exactly once, before any raw Opus packet is written. The caller
 * retains ownership of `format`.
 */
int h2_gizclaw_conversation_configure_pcm(
    h2_gizclaw_conversation_t *conversation,
    const h2_audio_pcm_format_t *format);

/**
 * Consumes one complete provider-sized PCM frame.
 *
 * The frame data is borrowed only for this call. Success means the complete
 * frame was copied into conversation-owned state, including when transport
 * backpressure leaves encoded Opus packets in the bounded transmit FIFO. If
 * that FIFO and the PCM accumulator cannot accept the complete frame,
 * `H2_PAL_ERR_WOULD_BLOCK` means the frame was not consumed and the caller must
 * retry the same frame. Other errors are terminal for PCM input and the frame
 * must not be retried.
 */
int h2_gizclaw_conversation_write_pcm(h2_gizclaw_conversation_t *conversation,
                                      const h2_audio_frame_t *frame);

/**
 * Sends one complete raw Opus packet. `timestamp_ms` remains for source
 * compatibility and is not serialized into the media payload.
 *
 * `H2_PAL_ERR_WOULD_BLOCK` means the packet was not accepted and the caller
 * must retain and retry the same packet before advancing its input stream.
 */
int h2_gizclaw_conversation_write_opus(h2_gizclaw_conversation_t *conversation,
                                       const uint8_t *opus, size_t opus_len,
                                       uint64_t timestamp_ms);

/**
 * Ends microphone input after all accepted audio has been sent.
 *
 * In PCM mode, commit drains complete 20 ms intervals and zero-pads one
 * non-empty final partial interval exactly once before EOS. Empty PCM input
 * does not manufacture an Opus packet. The operation is idempotent after
 * success. `H2_PAL_ERR_WOULD_BLOCK` preserves pending PCM or Opus state and
 * must be retried.
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

#ifdef __cplusplus
}
#endif

#endif
