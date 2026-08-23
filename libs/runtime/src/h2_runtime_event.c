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

h2_pal_result_t h2_runtime_enqueue_event(
    h2_runtime_t *runtime,
    const h2_runtime_queued_event_t *queued) {
    if (!h2_runtime_ready(runtime) ||
        runtime->private_state->event_queue == NULL || queued == NULL ||
        queued->component == H2_RUNTIME_COMPONENT_NONE ||
        (component_requires_id(queued->component) &&
         queued->component_id == H2_RUNTIME_COMPONENT_ID_NONE) ||
        queued->kind == H2_RUNTIME_EVENT_NONE ||
        queued->payload_size >
            runtime->private_state->event_payload_capacity) {
        return H2_PAL_ERR_INVALID_ARG;
    }

    h2_pal_result_t rc =
        h2_pal_queue_send(runtime->queue, runtime->private_state->event_queue,
                          queued, H2_PAL_QUEUE_NO_WAIT);
    if (rc == H2_PAL_ERR_FULL || rc == H2_PAL_QUEUE_ERR_TIMEOUT) {
        runtime->private_state->dropped_event_count += 1u;
        return H2_PAL_OK;
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

h2_pal_result_t h2_runtime_wait_event(
    h2_runtime_t *runtime,
    h2_runtime_event_t *out_event,
    uint32_t timeout_ms) {
    return recv_event(runtime, out_event, timeout_ms);
}
