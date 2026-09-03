#include "h2_runtime_internal.h"

#include <string.h>

_Static_assert(sizeof(h2_runtime_custom_event_payload_t) ==
                   H2_RUNTIME_EVENT_PAYLOAD_MAX,
               "custom event payload must match the Runtime payload bound");
_Static_assert(offsetof(h2_runtime_custom_event_payload_t, data) ==
                   H2_RUNTIME_CUSTOM_EVENT_HEADER_SIZE,
               "custom event header size must match the delivered layout");

/* Bounded so a stuck poster cannot hang h2_runtime_deinit() forever. */
#define H2_RUNTIME_CUSTOM_EVENT_DRAIN_TIMEOUT_MS 2000u

static void custom_event_lock(h2_runtime_private_t *private_state) {
    while (atomic_flag_test_and_set_explicit(
        &private_state->custom_event_lock, memory_order_acquire)) {
    }
}

static void custom_event_unlock(h2_runtime_private_t *private_state) {
    atomic_flag_clear_explicit(
        &private_state->custom_event_lock, memory_order_release);
}

/* Claims a posting slot unless deinit already closed the door. */
static int custom_event_enter(h2_runtime_private_t *private_state) {
    custom_event_lock(private_state);
    int entered = private_state->custom_event_closed == 0;
    if (entered) {
        private_state->custom_event_in_flight += 1u;
    }
    custom_event_unlock(private_state);
    return entered;
}

static void custom_event_leave(h2_runtime_private_t *private_state) {
    custom_event_lock(private_state);
    if (private_state->custom_event_in_flight > 0u) {
        private_state->custom_event_in_flight -= 1u;
    }
    custom_event_unlock(private_state);
}

static size_t custom_event_capacity(const h2_runtime_private_t *private_state) {
    size_t capacity = private_state->event_payload_capacity;
    return capacity <= H2_RUNTIME_CUSTOM_EVENT_HEADER_SIZE
               ? 0u
               : capacity - H2_RUNTIME_CUSTOM_EVENT_HEADER_SIZE;
}

static h2_pal_result_t post_custom_event(
    h2_runtime_t *runtime,
    const h2_runtime_custom_event_t *event,
    uint32_t timeout_ms) {
    if (!h2_runtime_ready(runtime) || event == NULL ||
        (event->payload_size > 0u && event->payload == NULL)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (event->payload_size > custom_event_capacity(runtime->private_state)) {
        return H2_PAL_ERR_TRUNCATED;
    }
    if (!custom_event_enter(runtime->private_state)) {
        return H2_PAL_ERR_INVALID_STATE;
    }

    h2_runtime_queued_event_t queued = {
        .kind = H2_RUNTIME_EVENT_CUSTOM,
        .component = H2_RUNTIME_COMPONENT_APP,
        .component_id = H2_RUNTIME_COMPONENT_ID_NONE,
        .sequence = h2_runtime_next_sequence(runtime),
        .timestamp_ms = h2_runtime_now_ms(runtime->time),
        .payload_size =
            H2_RUNTIME_CUSTOM_EVENT_HEADER_SIZE + event->payload_size,
    };
    queued.payload.custom.id = event->id;
    queued.payload.custom.size = (uint32_t)event->payload_size;
    if (event->payload_size > 0u) {
        memcpy(queued.payload.custom.data, event->payload, event->payload_size);
    }

    h2_pal_result_t rc =
        h2_runtime_enqueue_event_strict(runtime, &queued, timeout_ms);
    custom_event_leave(runtime->private_state);
    return rc;
}

h2_pal_result_t h2_runtime_post_custom_event(
    h2_runtime_t *runtime,
    const h2_runtime_custom_event_t *event) {
    return post_custom_event(runtime, event, H2_PAL_QUEUE_NO_WAIT);
}

h2_pal_result_t h2_runtime_post_custom_event_timeout(
    h2_runtime_t *runtime,
    const h2_runtime_custom_event_t *event,
    uint32_t timeout_ms) {
    if (timeout_ms == H2_PAL_QUEUE_WAIT_FOREVER) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    return post_custom_event(runtime, event, timeout_ms);
}

h2_pal_result_t h2_runtime_custom_event_payload_capacity(
    const h2_runtime_t *runtime,
    size_t *out_capacity) {
    if (!h2_runtime_ready(runtime) || out_capacity == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_capacity = custom_event_capacity(runtime->private_state);
    return H2_PAL_OK;
}

void h2_runtime_custom_event_close(h2_runtime_t *runtime) {
    if (!h2_runtime_ready(runtime)) {
        return;
    }
    h2_runtime_private_t *private_state = runtime->private_state;

    /*
     * Closing the queue first releases posters blocked on a full queue, so
     * the drain below only waits for the short window between claiming a slot
     * and returning from the send.
     */
    if (private_state->event_queue != NULL) {
        (void)h2_pal_queue_close(runtime->queue, private_state->event_queue);
    }

    for (uint32_t waited_ms = 0u;
         waited_ms <= H2_RUNTIME_CUSTOM_EVENT_DRAIN_TIMEOUT_MS;
         ++waited_ms) {
        custom_event_lock(private_state);
        private_state->custom_event_closed = 1;
        uint32_t in_flight = private_state->custom_event_in_flight;
        custom_event_unlock(private_state);
        if (in_flight == 0u) {
            return;
        }
        (void)h2_pal_time_sleep_ms(runtime->time, 1u);
    }
}
