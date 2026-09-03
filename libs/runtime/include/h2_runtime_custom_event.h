#ifndef H2_RUNTIME_CUSTOM_EVENT_H
#define H2_RUNTIME_CUSTOM_EVENT_H

/*
 * Scope: App and library owned custom events delivered through the Runtime
 * event queue.
 *
 * A background task posts a custom event with h2_runtime_post_custom_event();
 * the consumer keeps waiting on h2_runtime_wait_event() /
 * h2_runtime_poll_event() and receives it as H2_RUNTIME_EVENT_CUSTOM. The
 * Runtime copies the payload into the queue and never interprets the id or
 * the bytes: both belong to the posting owner.
 *
 * Payloads must be self-contained values. Do not post raw callbacks, task
 * handles or pointers into objects whose lifetime the consumer cannot verify;
 * post an identity plus a generation counter and resolve the object in the
 * consumer instead.
 */

#include "h2/pal/core/h2_pal_errors.h"
#include "h2_runtime_event.h"
#include "h2_runtime_types.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Owner-defined custom event id. Runtime never interprets this value. */
typedef uint32_t h2_runtime_custom_event_id_t;

/**
 * Builds an id from a 16-bit owner namespace and a 16-bit owner-local event
 * number so ids from different libraries and apps cannot collide.
 */
#define H2_RUNTIME_CUSTOM_EVENT_ID(owner, event) \
    ((h2_runtime_custom_event_id_t)((((uint32_t)(owner)) << 16u) | \
                                    ((uint32_t)(event) & 0xffffu)))
#define H2_RUNTIME_CUSTOM_EVENT_ID_OWNER(id) \
    ((uint16_t)(((h2_runtime_custom_event_id_t)(id)) >> 16u))
#define H2_RUNTIME_CUSTOM_EVENT_ID_EVENT(id) \
    ((uint16_t)(((h2_runtime_custom_event_id_t)(id)) & 0xffffu))

/** Bytes reserved by the delivered payload header (id plus size). */
#define H2_RUNTIME_CUSTOM_EVENT_HEADER_SIZE 8u

/**
 * Largest custom payload a Runtime built with the default event payload
 * capacity accepts. An instance configured with a smaller
 * event_payload_capacity accepts less; query it with
 * h2_runtime_custom_event_payload_capacity().
 */
#define H2_RUNTIME_CUSTOM_EVENT_PAYLOAD_MAX \
    (H2_RUNTIME_EVENT_PAYLOAD_MAX - H2_RUNTIME_CUSTOM_EVENT_HEADER_SIZE)

/** What the poster hands to Runtime. `payload` is copied, never retained. */
typedef struct h2_runtime_custom_event {
    h2_runtime_custom_event_id_t id;
    const void *payload;
    size_t payload_size;
} h2_runtime_custom_event_t;

/**
 * What the consumer reads through h2_runtime_event_t::payload when
 * `kind == H2_RUNTIME_EVENT_CUSTOM`. `size` bytes of `data` are valid; the
 * rest is unspecified.
 */
typedef struct h2_runtime_custom_event_payload {
    h2_runtime_custom_event_id_t id;
    uint32_t size;
    unsigned char data[H2_RUNTIME_CUSTOM_EVENT_PAYLOAD_MAX];
} h2_runtime_custom_event_payload_t;

/**
 * Recommended event payload buffer type. Any buffer works as long as it is at
 * least H2_RUNTIME_EVENT_PAYLOAD_MAX bytes and aligned for
 * h2_runtime_custom_event_payload_t; this union guarantees both.
 */
typedef union h2_runtime_event_payload_buffer {
    unsigned char bytes[H2_RUNTIME_EVENT_PAYLOAD_MAX];
    h2_runtime_custom_event_payload_t custom;
} h2_runtime_event_payload_buffer_t;

/**
 * Posts a custom event into the Runtime event queue and wakes a consumer
 * blocked in h2_runtime_wait_event(). Callable from any task, including
 * concurrently with other posters and with Runtime's own producers. Never
 * blocks.
 *
 * Custom events keep FIFO order with each other and share ordering with
 * system and input events already queued.
 *
 * Returns H2_PAL_ERR_FULL when the event queue has no room (the event is not
 * queued and nothing is dropped silently), H2_PAL_ERR_TRUNCATED when
 * `payload_size` exceeds this instance's custom payload capacity,
 * H2_PAL_ERR_INVALID_STATE once Runtime deinit has closed the queue to
 * producers, and H2_PAL_ERR_INVALID_ARG for a malformed request.
 *
 * Shutdown: h2_runtime_deinit() closes the queue and waits for posts that are
 * already in flight before it frees the Runtime, so a poster racing with
 * deinit is released with an error instead of touching a destroyed queue. The
 * caller still owns the other half of the contract: no post may *begin* after
 * deinit has started, because `runtime` itself is gone once it returns.
 */
h2_pal_result_t h2_runtime_post_custom_event(
    h2_runtime_t *runtime,
    const h2_runtime_custom_event_t *event);

/**
 * h2_runtime_post_custom_event() with a bounded wait for queue space.
 * `timeout_ms` must be a finite millisecond budget; H2_PAL_QUEUE_WAIT_FOREVER
 * is rejected with H2_PAL_ERR_INVALID_ARG so a poster can never stall
 * forever. Returns H2_PAL_ERR_FULL or H2_PAL_ERR_TIMEOUT when the queue stays
 * full for the whole budget.
 */
h2_pal_result_t h2_runtime_post_custom_event_timeout(
    h2_runtime_t *runtime,
    const h2_runtime_custom_event_t *event,
    uint32_t timeout_ms);

/** Reports the largest custom payload this Runtime instance accepts. */
h2_pal_result_t h2_runtime_custom_event_payload_capacity(
    const h2_runtime_t *runtime,
    size_t *out_capacity);

#ifdef __cplusplus
}
#endif

#endif
