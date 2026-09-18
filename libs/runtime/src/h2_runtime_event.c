#include "h2_runtime_internal.h"

#include <string.h>

static int component_requires_id(h2_runtime_component_t component) {
    switch (component) {
    case H2_RUNTIME_COMPONENT_BUTTON:
    case H2_RUNTIME_COMPONENT_NFC_READER:
    case H2_RUNTIME_COMPONENT_IMU:
        return 1;
    default:
        return 0;
    }
}

static h2_pal_result_t recv_event(
    h2_runtime_t *runtime,
    h2_runtime_event_t *out_event,
    uint32_t timeout_ms) {
    if (!h2_runtime_ready(runtime) || out_event == NULL || runtime->private_state->event_queue == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (out_event->payload == NULL ||
        out_event->payload_capacity < H2_RUNTIME_EVENT_PAYLOAD_MAX) {
        return H2_PAL_ERR_TRUNCATED;
    }

    h2_runtime_queued_event_t queued;
    h2_pal_result_t rc =
        h2_pal_queue_recv(runtime->queue, runtime->private_state->event_queue, &queued, timeout_ms);
    if (rc != H2_PAL_OK) {
        return rc;
    }

    void *payload = out_event->payload;
    size_t payload_capacity = out_event->payload_capacity;
    memset(out_event, 0, sizeof(*out_event));
    out_event->kind = queued.kind;
    out_event->component = queued.component;
    out_event->component_id = queued.component_id;
    out_event->sequence = queued.sequence;
    out_event->timestamp_ms = queued.timestamp_ms;
    out_event->payload = payload;
    out_event->payload_capacity = payload_capacity;
    out_event->payload_size = queued.payload_size;
    if (queued.payload_size > 0u) {
        memcpy(payload, queued.payload.bytes, queued.payload_size);
    }
    return H2_PAL_OK;
}

static int queued_event_is_valid(
    const h2_runtime_t *runtime,
    const h2_runtime_queued_event_t *queued) {
    return h2_runtime_ready(runtime) &&
           runtime->private_state->event_queue != NULL && queued != NULL &&
           queued->component != H2_RUNTIME_COMPONENT_NONE &&
           !(component_requires_id(queued->component) &&
             queued->component_id == H2_RUNTIME_COMPONENT_ID_NONE) &&
           queued->kind != H2_RUNTIME_EVENT_NONE &&
           queued->payload_size <=
               runtime->private_state->event_payload_capacity;
}

void h2_runtime_notify_internal(h2_runtime_t *runtime) {
    h2_pal_queue_t *wake = runtime->private_state->wake_queue;
    if (wake == NULL) {
        return;
    }
    const uint8_t token = 1u;
    /*
     * The slot is the coalescing rule: FULL means a wake is already pending
     * and CLOSED means deinit owns the queue. Neither is an error here.
     */
    (void)h2_pal_queue_send(runtime->queue, wake, &token, H2_PAL_QUEUE_NO_WAIT);
}

static uint32_t add_dropped_event(h2_runtime_private_t *private_state) {
#if H2_RUNTIME_ATOMIC_ADD_LOCK_FREE
    return atomic_fetch_add_explicit(
               &private_state->dropped_event_count, 1u,
               memory_order_relaxed) +
           1u;
#else
    h2_runtime_flag_lock(&private_state->dropped_event_lock);
    const uint32_t total =
        atomic_load_explicit(
            &private_state->dropped_event_count, memory_order_relaxed) +
        1u;
    atomic_store_explicit(
        &private_state->dropped_event_count, total, memory_order_relaxed);
    h2_runtime_flag_unlock(&private_state->dropped_event_lock);
    return total;
#endif
}

void h2_runtime_record_dropped_event(
    h2_runtime_t *runtime,
    h2_runtime_event_kind_t kind,
    h2_runtime_component_t component,
    h2_runtime_component_id_t component_id) {
    if (!h2_runtime_ready(runtime)) {
        return;
    }
    h2_runtime_private_t *private_state = runtime->private_state;
    atomic_store_explicit(
        &private_state->last_dropped_kind, (int)kind, memory_order_relaxed);
    atomic_store_explicit(
        &private_state->last_dropped_component, (int)component,
        memory_order_relaxed);
    atomic_store_explicit(
        &private_state->last_dropped_component_id, component_id,
        memory_order_relaxed);
    (void)add_dropped_event(private_state);
    h2_runtime_report_dropped_events(runtime);
}

/* Appends `text` and returns the new length; never writes past `capacity`. */
static size_t append_text(
    char *buffer, size_t length, size_t capacity, const char *text) {
    while (*text != '\0' && length + 1u < capacity) {
        buffer[length++] = *text++;
    }
    buffer[length] = '\0';
    return length;
}

/* Decimal without printf, which some Runtime targets do not link. */
static size_t append_decimal(
    char *buffer, size_t length, size_t capacity, uint32_t value) {
    char digits[11];
    size_t count = 0u;
    do {
        digits[count++] = (char)('0' + (value % 10u));
        value /= 10u;
    } while (value != 0u);
    while (count > 0u && length + 1u < capacity) {
        buffer[length++] = digits[--count];
    }
    buffer[length] = '\0';
    return length;
}

void h2_runtime_report_dropped_events(h2_runtime_t *runtime) {
    if (!h2_runtime_ready(runtime)) {
        return;
    }
    h2_runtime_private_t *private_state = runtime->private_state;
    if (atomic_flag_test_and_set_explicit(
            &private_state->drop_report_lock, memory_order_acquire)) {
        return;
    }
    const uint32_t total = atomic_load_explicit(
        &private_state->dropped_event_count, memory_order_relaxed);
    if (total != private_state->drop_reported_count) {
        const h2_runtime_timestamp_ms_t now_ms =
            h2_runtime_now_ms(runtime->time);
        if (private_state->drop_reported_once == 0 ||
            now_ms - private_state->drop_reported_at_ms >=
                H2_RUNTIME_DROPPED_EVENT_WARN_INTERVAL_MS) {
            char message[128];
            size_t length = 0u;
            message[0] = '\0';
            length = append_text(
                message, length, sizeof(message),
                "event queue full: dropped ");
            length = append_decimal(
                message, length, sizeof(message),
                total - private_state->drop_reported_count);
            length = append_text(
                message, length, sizeof(message), " event(s), total ");
            length = append_decimal(message, length, sizeof(message), total);
            length = append_text(
                message, length, sizeof(message), "; last kind ");
            length = append_decimal(
                message, length, sizeof(message),
                (uint32_t)atomic_load_explicit(
                    &private_state->last_dropped_kind,
                    memory_order_relaxed));
            length = append_text(
                message, length, sizeof(message), " component ");
            length = append_decimal(
                message, length, sizeof(message),
                (uint32_t)atomic_load_explicit(
                    &private_state->last_dropped_component,
                    memory_order_relaxed));
            length = append_text(message, length, sizeof(message), " id ");
            (void)append_decimal(
                message, length, sizeof(message),
                atomic_load_explicit(
                    &private_state->last_dropped_component_id,
                    memory_order_relaxed));
            (void)h2_pal_log_write(
                runtime->log, H2_PAL_LOG_WARN, "runtime/event", message);
            private_state->drop_reported_once = 1;
            private_state->drop_reported_count = total;
            private_state->drop_reported_at_ms = now_ms;
        }
    }
    atomic_flag_clear_explicit(
        &private_state->drop_report_lock, memory_order_release);
}

h2_pal_result_t h2_runtime_dropped_event_count(
    const h2_runtime_t *runtime,
    uint32_t *out_count) {
    if (!h2_runtime_ready(runtime) || out_count == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_count = atomic_load_explicit(
        &runtime->private_state->dropped_event_count, memory_order_relaxed);
    return H2_PAL_OK;
}

h2_pal_result_t h2_runtime_enqueue_event(
    h2_runtime_t *runtime,
    const h2_runtime_queued_event_t *queued) {
    if (!queued_event_is_valid(runtime, queued)) {
        return H2_PAL_ERR_INVALID_ARG;
    }

    h2_pal_result_t rc =
        h2_pal_queue_send(runtime->queue, runtime->private_state->event_queue,
                          queued, H2_PAL_QUEUE_NO_WAIT);
    if (rc == H2_PAL_ERR_FULL || rc == H2_PAL_QUEUE_ERR_TIMEOUT) {
        h2_runtime_record_dropped_event(
            runtime, queued->kind, queued->component, queued->component_id);
        return H2_PAL_OK;
    }
    if (rc == H2_PAL_OK) {
        h2_runtime_notify_internal(runtime);
    }
    return rc;
}

h2_pal_result_t h2_runtime_enqueue_event_strict(
    h2_runtime_t *runtime,
    const h2_runtime_queued_event_t *queued,
    uint32_t timeout_ms) {
    if (!queued_event_is_valid(runtime, queued)) {
        return H2_PAL_ERR_INVALID_ARG;
    }

    h2_pal_result_t rc =
        h2_pal_queue_send(runtime->queue, runtime->private_state->event_queue,
                          queued, timeout_ms);
    /* Providers report a full non-blocking send as either code. */
    if (timeout_ms == H2_PAL_QUEUE_NO_WAIT &&
        (rc == H2_PAL_QUEUE_ERR_TIMEOUT || rc == H2_PAL_ERR_WOULD_BLOCK)) {
        return H2_PAL_ERR_FULL;
    }
    if (rc == H2_PAL_OK) {
        h2_runtime_notify_internal(runtime);
    }
    return rc;
}

h2_pal_result_t h2_runtime_emit_event(
    h2_runtime_t *runtime,
    h2_runtime_event_kind_t kind,
    h2_runtime_component_t component,
    h2_runtime_component_id_t component_id,
    h2_runtime_sequence_t sequence,
    h2_runtime_timestamp_ms_t timestamp_ms,
    const void *payload,
    size_t payload_size) {
    if (!h2_runtime_ready(runtime) ||
        payload_size > runtime->private_state->event_payload_capacity ||
        (payload_size > 0u && payload == NULL)) {
        return H2_PAL_ERR_INVALID_ARG;
    }

    h2_runtime_queued_event_t queued = {
        .kind = kind,
        .component = component,
        .component_id = component_id,
        .sequence = sequence,
        .timestamp_ms = timestamp_ms,
        .payload_size = payload_size,
    };
    if (payload_size > 0u) {
        memcpy(queued.payload.bytes, payload, payload_size);
    }

    return h2_runtime_enqueue_event(runtime, &queued);
}

h2_pal_result_t h2_runtime_poll_event(
    h2_runtime_t *runtime,
    h2_runtime_event_t *out_event) {
    return recv_event(runtime, out_event, H2_PAL_QUEUE_NO_WAIT);
}

static int recv_would_block(h2_pal_result_t rc) {
    return rc == H2_PAL_ERR_WOULD_BLOCK || rc == H2_PAL_ERR_TIMEOUT ||
           rc == H2_PAL_QUEUE_ERR_TIMEOUT;
}

h2_pal_result_t h2_runtime_wait_notify(
    h2_runtime_t *runtime,
    uint32_t timeout_ms) {
    if (!h2_runtime_ready(runtime) || runtime->private_state->wake_queue == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    uint8_t token = 0u;
    h2_pal_result_t rc = h2_pal_queue_recv(
        runtime->queue, runtime->private_state->wake_queue, &token, timeout_ms);
    return recv_would_block(rc) ? H2_PAL_ERR_TIMEOUT : rc;
}
