#include "h2_runtime_internal.h"

#include <string.h>

_Static_assert(sizeof(h2_runtime_custom_event_payload_t) ==
                   H2_RUNTIME_EVENT_PAYLOAD_MAX,
               "custom event payload must match the Runtime payload bound");
_Static_assert(offsetof(h2_runtime_custom_event_payload_t, data) ==
                   H2_RUNTIME_CUSTOM_EVENT_HEADER_SIZE,
               "custom event header size must match the delivered layout");

#if !H2_RUNTIME_ATOMIC_ADD_LOCK_FREE
static void custom_event_lock(h2_runtime_t *runtime) {
    h2_runtime_flag_lock(&runtime->private_state->custom_event_lock);
}

static void custom_event_unlock(h2_runtime_t *runtime) {
    h2_runtime_flag_unlock(&runtime->private_state->custom_event_lock);
}
#endif

/* Claims a posting slot unless deinit already closed the door. */
static int custom_event_enter(h2_runtime_t *runtime) {
    h2_runtime_private_t *private_state = runtime->private_state;
#if H2_RUNTIME_ATOMIC_ADD_LOCK_FREE
    /*
     * Claim first, then check the door: deinit closes the door and then reads
     * in_flight, so a claim it does not see was made after it read, and that
     * poster sees the closed door and backs out.
     */
    atomic_fetch_add_explicit(
        &private_state->custom_event_in_flight, 1u, memory_order_seq_cst);
    if (atomic_load_explicit(
            &private_state->custom_event_closed, memory_order_seq_cst) != 0) {
        atomic_fetch_sub_explicit(
            &private_state->custom_event_in_flight, 1u, memory_order_seq_cst);
        return 0;
    }
    return 1;
#else
    custom_event_lock(runtime);
    int entered = atomic_load_explicit(
        &private_state->custom_event_closed, memory_order_relaxed) == 0;
    if (entered) {
        atomic_store_explicit(
            &private_state->custom_event_in_flight,
            atomic_load_explicit(
                &private_state->custom_event_in_flight, memory_order_relaxed) + 1u,
            memory_order_relaxed);
    }
    custom_event_unlock(runtime);
    return entered;
#endif
}

static void custom_event_leave(h2_runtime_t *runtime) {
    h2_runtime_private_t *private_state = runtime->private_state;
#if H2_RUNTIME_ATOMIC_ADD_LOCK_FREE
    atomic_fetch_sub_explicit(
        &private_state->custom_event_in_flight, 1u, memory_order_seq_cst);
#else
    custom_event_lock(runtime);
    unsigned int in_flight = atomic_load_explicit(
        &private_state->custom_event_in_flight, memory_order_relaxed);
    if (in_flight > 0u) {
        atomic_store_explicit(
            &private_state->custom_event_in_flight, in_flight - 1u,
            memory_order_relaxed);
    }
    custom_event_unlock(runtime);
#endif
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
    if (!custom_event_enter(runtime)) {
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
    custom_event_leave(runtime);
    return rc;
}

h2_pal_result_t h2_runtime_post_custom_event(
    h2_runtime_t *runtime,
    const h2_runtime_custom_event_t *event) {
    return post_custom_event(runtime, event, H2_PAL_QUEUE_NO_WAIT);
}

h2_pal_result_t h2_runtime_notify(h2_runtime_t *runtime) {
    if (!h2_runtime_ready(runtime)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    /* Same door as a post: deinit drains notifiers before the queue goes. */
    if (!custom_event_enter(runtime)) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    h2_runtime_notify_internal(runtime);
    custom_event_leave(runtime);
    return H2_PAL_OK;
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
    /* A consumer still blocked in h2_runtime_wait_notify() gets CLOSED. */
    if (private_state->wake_queue != NULL) {
        (void)h2_pal_queue_close(runtime->queue, private_state->wake_queue);
    }

    /*
     * An admitted poster still reads private_state after its send returns, so
     * this wait cannot be bounded: falling through would free the state under
     * it. Termination is guaranteed instead by the poster side. A post holds
     * its slot only across one queue send, the send takes a finite timeout
     * (h2_runtime_post_custom_event_timeout() rejects
     * H2_PAL_QUEUE_WAIT_FOREVER), and the close above releases a send that is
     * already blocked.
     */
    for (;;) {
        unsigned int in_flight;
#if H2_RUNTIME_ATOMIC_ADD_LOCK_FREE
        atomic_store_explicit(
            &private_state->custom_event_closed, 1, memory_order_seq_cst);
        in_flight = atomic_load_explicit(
            &private_state->custom_event_in_flight, memory_order_seq_cst);
#else
        custom_event_lock(runtime);
        atomic_store_explicit(
            &private_state->custom_event_closed, 1, memory_order_relaxed);
        in_flight = atomic_load_explicit(
            &private_state->custom_event_in_flight, memory_order_relaxed);
        custom_event_unlock(runtime);
#endif
        if (in_flight == 0u) {
            return;
        }
        (void)h2_pal_time_sleep_ms(runtime->time, 1u);
    }
}
