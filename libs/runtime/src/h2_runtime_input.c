#include "h2_runtime_internal.h"
#include "h2_runtime_task_names.h"

#include <string.h>

_Static_assert(
    sizeof(h2_runtime_button_down_event_t) <=
        sizeof(h2_runtime_input_event_payload_t),
    "button down payload must fit pending input event");
_Static_assert(
    sizeof(h2_runtime_button_up_event_t) <=
        sizeof(h2_runtime_input_event_payload_t),
    "button up payload must fit pending input event");
_Static_assert(
    sizeof(h2_runtime_button_action_event_t) <=
        sizeof(h2_runtime_input_event_payload_t),
    "button action payload must fit pending input event");
_Static_assert(
    sizeof(h2_runtime_nfc_state_t) <= sizeof(h2_runtime_input_event_payload_t),
    "NFC payload must fit pending input event");
_Static_assert(
    sizeof(h2_runtime_imu_gesture_event_t) <=
        sizeof(h2_runtime_input_event_payload_t),
    "IMU payload must fit pending input event");
_Static_assert(
    sizeof(h2_pal_result_t) <= sizeof(h2_runtime_input_event_payload_t),
    "error payload must fit pending input event");

static uint32_t select_interval(uint32_t configured, uint32_t fallback) {
    return configured != 0u ? configured : fallback;
}

static int has_mapped_input_component(const h2_runtime_t *runtime) {
    for (size_t index = 0u;
         index < runtime->private_state->component_mapping_count;
         ++index) {
        h2_runtime_component_t component =
            runtime->private_state->component_mappings[index].component;
        if (component == H2_RUNTIME_COMPONENT_BUTTON ||
            component == H2_RUNTIME_COMPONENT_NFC_READER ||
            component == H2_RUNTIME_COMPONENT_IMU ||
            component == H2_RUNTIME_COMPONENT_BATTERY ||
            component == H2_RUNTIME_COMPONENT_TEMPERATURE_SENSOR) {
            return 1;
        }
    }
    return 0;
}

static int64_t abs_i32_to_i64(int32_t value) {
    int64_t widened = value;
    return widened < 0 ? -widened : widened;
}

static int64_t max3_abs_i32(int32_t a, int32_t b, int32_t c) {
    int64_t max = abs_i32_to_i64(a);
    int64_t value = abs_i32_to_i64(b);
    if (value > max) {
        max = value;
    }
    value = abs_i32_to_i64(c);
    if (value > max) {
        max = value;
    }
    return max;
}

static int32_t clamp_i64_to_i32(int64_t value) {
    return value > INT32_MAX ? INT32_MAX : (int32_t)value;
}

static h2_pal_result_t input_now(
    const h2_runtime_t *runtime,
    h2_runtime_timestamp_ms_t *out_now_ms) {
    uint64_t now_ms = 0u;
    h2_pal_result_t rc =
        h2_pal_time_get_monotonic_ms(runtime->time, &now_ms);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    *out_now_ms = (h2_runtime_timestamp_ms_t)now_ms;
    return H2_PAL_OK;
}

static void input_fill_state_bank(
    h2_runtime_t *runtime,
    h2_runtime_state_bank_t *bank) {
    bank->entry_count = runtime->private_state->input_source_count;
    for (size_t i = 0u; i < bank->entry_count; ++i) {
        const h2_runtime_input_source_t *source =
            &runtime->private_state->input_sources[i];
        h2_runtime_state_entry_t *entry = &bank->entries[i];
        entry->component = source->component;
        entry->component_id = source->component_id;
        switch (source->component) {
        case H2_RUNTIME_COMPONENT_BUTTON:
            entry->state.button = source->button_state;
            break;
        case H2_RUNTIME_COMPONENT_NFC_READER:
            entry->state.nfc = source->nfc_state;
            break;
        case H2_RUNTIME_COMPONENT_IMU:
            entry->state.imu = source->imu_state;
            break;
        case H2_RUNTIME_COMPONENT_BATTERY:
            entry->state.battery = source->battery_state;
            break;
        case H2_RUNTIME_COMPONENT_TEMPERATURE_SENSOR:
            entry->state.temperature = source->temperature_state;
            break;
        default:
            break;
        }
    }
}

/* Publishes the input source table through the Runtime state publication. */
static h2_pal_result_t publish_snapshot(
    h2_runtime_t *runtime,
    int empty) {
    return h2_runtime_state_publish(
        runtime,
        empty != 0 ? NULL : input_fill_state_bank,
        runtime->private_state->input_event_sequence_ceiling);
}

static h2_pal_result_t publish_snapshot_if_due(
    h2_runtime_t *runtime,
    h2_runtime_timestamp_ms_t now_ms,
    int force) {
    return h2_runtime_state_publish_if_due(
        runtime,
        now_ms,
        force,
        input_fill_state_bank,
        runtime->private_state->input_event_sequence_ceiling);
}

static void source_set_event_time(
    h2_runtime_input_source_t *source,
    h2_runtime_sequence_t sequence,
    h2_runtime_timestamp_ms_t now_ms) {
    source->sequence = sequence;
    source->timestamp_ms = now_ms;
}

static h2_pal_result_t append_input_event(
    h2_runtime_t *runtime,
    h2_runtime_input_source_t *source,
    h2_runtime_event_kind_t kind,
    h2_runtime_timestamp_ms_t now_ms,
    const void *payload,
    size_t payload_size) {
    if (runtime->private_state->input_pending_event_count >=
            runtime->private_state->input_pending_event_capacity ||
        payload == NULL ||
        payload_size > sizeof(h2_runtime_input_event_payload_t)) {
        return H2_PAL_ERR_NO_SPACE;
    }

    h2_runtime_sequence_t sequence = h2_runtime_next_sequence(runtime);
    h2_runtime_input_pending_event_t *pending =
        &runtime->private_state->input_pending_events[
            runtime->private_state->input_pending_event_count++];
    memset(pending, 0, sizeof(*pending));
    pending->kind = kind;
    pending->component = source->component;
    pending->component_id = source->component_id;
    pending->sequence = sequence;
    pending->timestamp_ms = now_ms;
    pending->payload_size = payload_size;
    memcpy(&pending->payload, payload, payload_size);

    source_set_event_time(source, sequence, now_ms);
    if (sequence > runtime->private_state->input_event_sequence_ceiling) {
        runtime->private_state->input_event_sequence_ceiling = sequence;
    }
    h2_runtime_state_mark_dirty(runtime);
    return H2_PAL_OK;
}

static h2_pal_result_t enqueue_pending_events(h2_runtime_t *runtime) {
    h2_pal_result_t first_error = H2_PAL_OK;
    for (size_t i = 0u;
         i < runtime->private_state->input_pending_event_count;
         ++i) {
        const h2_runtime_input_pending_event_t *pending =
            &runtime->private_state->input_pending_events[i];
        h2_pal_result_t rc = h2_runtime_emit_event(
            runtime,
            pending->kind,
            pending->component,
            pending->component_id,
            pending->sequence,
            pending->timestamp_ms,
            &pending->payload,
            pending->payload_size);
        if (rc != H2_PAL_OK) {
            first_error = rc;
            break;
        }
    }
    runtime->private_state->input_pending_event_count = 0u;
    return first_error;
}

static h2_pal_result_t publish_pending_events(h2_runtime_t *runtime) {
    if (runtime->private_state->input_pending_event_count == 0u) {
        return H2_PAL_OK;
    }
    /*
     * Publish the dirty snapshot before its events become dequeueable so
     * the component-state API never lags behind a delivered event.
     */
    if (runtime->private_state->state_dirty != 0) {
        const h2_pal_result_t snapshot_rc = publish_snapshot(runtime, 0);
        if (snapshot_rc == H2_PAL_ERR_WOULD_BLOCK) {
            /*
             * Every retired slot is pinned by a reader. Defer event
             * visibility: the pending events stay queued and are published
             * together with the snapshot on a later poll, so a dequeued
             * event never precedes its component state.
             */
            return H2_PAL_OK;
        }
        if (snapshot_rc != H2_PAL_OK) {
            return snapshot_rc;
        }
    }
    return enqueue_pending_events(runtime);
}

static h2_pal_result_t publish_error(
    h2_runtime_t *runtime,
    h2_runtime_input_source_t *source,
    h2_pal_result_t result,
    h2_runtime_timestamp_ms_t now_ms) {
    switch (source->component) {
    case H2_RUNTIME_COMPONENT_BUTTON:
        source->button_state.result = result;
        source->button_state.updated_at_ms = now_ms;
        break;
    case H2_RUNTIME_COMPONENT_NFC_READER:
        source->nfc_state.status = H2_RUNTIME_NFC_STATE_ERROR;
        source->nfc_state.stage = H2_PAL_NFC_STAGE_ERROR;
        source->nfc_state.result = result;
        source->nfc_state.updated_at_ms = now_ms;
        break;
    case H2_RUNTIME_COMPONENT_IMU:
        source->imu_state.result = result;
        source->imu_state.updated_at_ms = now_ms;
        break;
    default:
        break;
    }
    h2_runtime_state_mark_dirty(runtime);
    return append_input_event(
        runtime,
        source,
        H2_RUNTIME_COMPONENT_EVENT_ERROR,
        now_ms,
        &result,
        sizeof(result));
}

static int source_periph_duplicate(
    const h2_runtime_t *runtime,
    h2_pal_periph_id_t periph_id) {
    for (size_t i = 0u; i < runtime->private_state->input_source_count; ++i) {
        if (runtime->private_state->input_sources[i].periph_id == periph_id) {
            return 1;
        }
    }
    return 0;
}

static void reset_input_sources(h2_runtime_t *runtime) {
    memset(
        runtime->private_state->input_sources,
        0,
        runtime->private_state->input_source_capacity *
            sizeof(*runtime->private_state->input_sources));
    runtime->private_state->input_source_count = 0u;
    runtime->private_state->input_pending_event_count = 0u;
    runtime->private_state->state_dirty = 0;
}

static h2_pal_result_t add_source(
    h2_runtime_t *runtime,
    const h2_pal_periph_info_t *info,
    h2_runtime_timestamp_ms_t now_ms) {
    if (info == NULL || info->id == H2_RUNTIME_COMPONENT_ID_NONE ||
        source_periph_duplicate(runtime, info->id)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    h2_runtime_component_t component =
        h2_runtime_component_from_periph_type(info->type);
    if (component == H2_RUNTIME_COMPONENT_NONE) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    const h2_runtime_component_mapping_t *mapping =
        h2_runtime_find_component_mapping_by_periph(runtime, info->id);
    if (mapping == NULL || mapping->component != component) {
        return H2_PAL_OK;
    }
    if (runtime->private_state->input_source_count >=
        runtime->private_state->input_source_capacity) {
        return H2_PAL_ERR_NO_SPACE;
    }

    h2_runtime_input_source_t candidate;
    memset(&candidate, 0, sizeof(candidate));
    candidate.component = component;
    candidate.component_id = mapping->component_id;
    candidate.periph_id = info->id;
    candidate.timestamp_ms = now_ms;
    candidate.next_due_ms = now_ms;

    if (info->type == H2_PAL_PERIPH_TYPE_SINGLE_BUTTON) {
        candidate.kind = H2_RUNTIME_INPUT_SOURCE_SINGLE_BUTTON;
        candidate.button_delivery = H2_PAL_BUTTON_DELIVERY_POLL_STATE;
        if (info->payload != NULL || info->payload_size != 0u) {
            if (info->payload == NULL ||
                info->payload_size <
                    sizeof(h2_pal_periph_single_button_payload_t)) {
                return H2_PAL_ERR_INVALID_ARG;
            }
            const h2_pal_periph_single_button_payload_t *payload =
                (const h2_pal_periph_single_button_payload_t *)info->payload;
            if (payload->delivery != H2_PAL_BUTTON_DELIVERY_POLL_STATE &&
                payload->delivery != H2_PAL_BUTTON_DELIVERY_PUSH_EDGE) {
                return H2_PAL_ERR_INVALID_ARG;
            }
            candidate.button_delivery = payload->delivery;
        }
        candidate.button_state.result = H2_PAL_OK;
        candidate.poll_interval_ms =
            runtime->private_state->input_button_poll_interval_ms;
    } else if (info->type == H2_PAL_PERIPH_TYPE_RADIO_BUTTON) {
        if (info->payload == NULL ||
            info->payload_size <
                sizeof(h2_pal_periph_radio_button_payload_t)) {
            return H2_PAL_ERR_INVALID_ARG;
        }
        const h2_pal_periph_radio_button_payload_t *payload =
            (const h2_pal_periph_radio_button_payload_t *)info->payload;
        candidate.kind = H2_RUNTIME_INPUT_SOURCE_RADIO_BUTTON;
        candidate.group_id = payload->group_id;
        candidate.button_state.result = H2_PAL_OK;
        candidate.poll_interval_ms =
            runtime->private_state->input_button_poll_interval_ms;
    } else if (info->type == H2_PAL_PERIPH_TYPE_NFC_READER) {
        candidate.kind = H2_RUNTIME_INPUT_SOURCE_NFC_READER;
        candidate.nfc_state.status = H2_RUNTIME_NFC_STATE_NONE;
        candidate.nfc_state.result = H2_PAL_OK;
        candidate.poll_interval_ms =
            runtime->private_state->input_nfc_poll_interval_ms;
    } else if (info->type == H2_PAL_PERIPH_TYPE_IMU) {
        candidate.kind = H2_RUNTIME_INPUT_SOURCE_IMU;
        candidate.imu_state.gesture_kind =
            H2_RUNTIME_IMU_GESTURE_NONE;
        candidate.imu_state.result = H2_PAL_OK;
        candidate.poll_interval_ms =
            runtime->private_state->input_imu_poll_interval_ms;
    } else if (info->type == H2_PAL_PERIPH_TYPE_BATTERY) {
        candidate.kind = H2_RUNTIME_INPUT_SOURCE_BATTERY;
        candidate.battery_state.result = H2_PAL_ERR_WOULD_BLOCK;
        candidate.poll_interval_ms =
            runtime->private_state->input_battery_poll_interval_ms;
    } else if (info->type == H2_PAL_PERIPH_TYPE_TEMPERATURE_SENSOR) {
        candidate.kind = H2_RUNTIME_INPUT_SOURCE_TEMPERATURE;
        candidate.temperature_state.result = H2_PAL_ERR_WOULD_BLOCK;
        candidate.poll_interval_ms =
            runtime->private_state->input_temperature_poll_interval_ms;
    } else {
        return H2_PAL_ERR_INVALID_ARG;
    }

    runtime->private_state->input_sources[
        runtime->private_state->input_source_count++] = candidate;
    return H2_PAL_OK;
}

typedef struct h2_runtime_periph_collect_ctx {
    h2_runtime_t *runtime;
    h2_runtime_timestamp_ms_t now_ms;
} h2_runtime_periph_collect_ctx_t;

static h2_pal_result_t collect_periph(
    void *user,
    const h2_pal_periph_info_t *info) {
    h2_runtime_periph_collect_ctx_t *ctx =
        (h2_runtime_periph_collect_ctx_t *)user;
    if (info == NULL ||
        (info->type != H2_PAL_PERIPH_TYPE_SINGLE_BUTTON &&
         info->type != H2_PAL_PERIPH_TYPE_RADIO_BUTTON &&
         info->type != H2_PAL_PERIPH_TYPE_NFC_READER &&
         info->type != H2_PAL_PERIPH_TYPE_IMU &&
         info->type != H2_PAL_PERIPH_TYPE_BATTERY &&
         info->type != H2_PAL_PERIPH_TYPE_TEMPERATURE_SENSOR)) {
        return H2_PAL_OK;
    }
    return add_source(ctx->runtime, info, ctx->now_ms);
}

static h2_pal_result_t collect_input_sources(
    h2_runtime_t *runtime,
    h2_runtime_timestamp_ms_t now_ms) {
    if (runtime->private_state->component_mapping_count == 0u) {
        return H2_PAL_OK;
    }
    h2_runtime_periph_collect_ctx_t ctx = {
        .runtime = runtime,
        .now_ms = now_ms,
    };
    h2_pal_result_t rc = h2_pal_periph_list(
        runtime->periph,
        H2_PAL_PERIPH_TYPE_ANY,
        collect_periph,
        &ctx);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    return H2_PAL_OK;
}

static h2_pal_result_t ensure_input_sources(
    h2_runtime_t *runtime,
    h2_runtime_timestamp_ms_t now_ms) {
    if (runtime->private_state->input_sources_ready != 0) {
        return H2_PAL_OK;
    }
    h2_pal_result_t rc = h2_runtime_state_publication_init(runtime);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    reset_input_sources(runtime);
    rc = collect_input_sources(runtime, now_ms);
    if (rc != H2_PAL_OK) {
        reset_input_sources(runtime);
        return rc;
    }
    runtime->private_state->input_sources_ready = 1;
    h2_runtime_state_mark_dirty(runtime);
    return H2_PAL_OK;
}

static h2_runtime_input_source_t *find_push_button_source(
    h2_runtime_t *runtime,
    h2_pal_periph_id_t periph_id) {
    for (size_t index = 0u;
         index < runtime->private_state->input_source_count;
         ++index) {
        h2_runtime_input_source_t *candidate =
            &runtime->private_state->input_sources[index];
        if (candidate->periph_id == periph_id &&
            candidate->component == H2_RUNTIME_COMPONENT_BUTTON &&
            candidate->kind == H2_RUNTIME_INPUT_SOURCE_SINGLE_BUTTON &&
            candidate->button_delivery == H2_PAL_BUTTON_DELIVERY_PUSH_EDGE) {
            return candidate;
        }
    }
    return NULL;
}

static h2_pal_result_t update_button_pressed(
    h2_runtime_t *runtime,
    h2_runtime_input_source_t *source,
    int is_pressed,
    h2_runtime_timestamp_ms_t now_ms) {
    h2_runtime_button_recognizer_t *button = &source->button;
    int emit_down = 0;
    int emit_up = 0;
    int emit_action = 0;
    h2_runtime_button_action_phase_t action_phase =
        H2_RUNTIME_BUTTON_ACTION_PHASE_PRESSED;
    int state_changed = 0;
    if (button->initialized == 0) {
        button->initialized = 1;
        button->is_pressed = is_pressed;
        state_changed = 1;
        if (is_pressed != 0) {
            button->pressed_at_ms = now_ms;
            button->click_count = 1u;
            source->button_state.pressed = true;
            source->button_state.pressed_at_ms = now_ms;
            source->button_state.click_count = button->click_count;
            emit_down = 1;
            emit_action = 1;
        } else {
            source->button_state.pressed = false;
            source->button_state.pressed_at_ms = 0u;
        }
    } else if (is_pressed != 0 && button->is_pressed == 0) {
        button->is_pressed = 1;
        button->pressed_at_ms = now_ms;
        if (button->last_released_at_ms == 0u ||
            now_ms < button->last_released_at_ms ||
            now_ms - button->last_released_at_ms >
                H2_RUNTIME_BUTTON_CLICK_GAP_MS) {
            button->click_count = 1u;
        } else if (button->click_count < UINT16_MAX) {
            ++button->click_count;
        }
        source->button_state.pressed = true;
        source->button_state.pressed_at_ms = now_ms;
        source->button_state.click_count = button->click_count;
        emit_down = 1;
        emit_action = 1;
        state_changed = 1;
    } else if (is_pressed != 0 && button->is_pressed != 0) {
        emit_down = 1;
        emit_action = 1;
        action_phase = H2_RUNTIME_BUTTON_ACTION_PHASE_HOLDING;
        state_changed = 1;
    } else if (is_pressed == 0 && button->is_pressed != 0) {
        button->is_pressed = 0;
        button->last_released_at_ms = now_ms;
        source->button_state.pressed = false;
        source->button_state.pressed_at_ms = 0u;
        emit_up = 1;
        emit_action = 1;
        action_phase = H2_RUNTIME_BUTTON_ACTION_PHASE_RELEASED;
        state_changed = 1;
    }
    if (source->button_state.result != H2_PAL_OK) {
        state_changed = 1;
    }

    if (state_changed != 0) {
        source->button_state.updated_at_ms = now_ms;
        source->button_state.result = H2_PAL_OK;
        source->timestamp_ms = now_ms;
        h2_runtime_state_mark_dirty(runtime);
    }

    if (emit_down != 0) {
        const h2_runtime_button_down_event_t event = {
            .pressed_at_ms = button->pressed_at_ms,
        };
        h2_pal_result_t rc = append_input_event(
            runtime,
            source,
            H2_RUNTIME_COMPONENT_EVENT_BUTTON_DOWN,
            now_ms,
            &event,
            sizeof(event));
        if (rc != H2_PAL_OK) {
            return rc;
        }
    }
    if (emit_up != 0) {
        const h2_runtime_button_up_event_t event = {
            .pressed_at_ms = button->pressed_at_ms,
            .released_at_ms = now_ms,
        };
        h2_pal_result_t rc = append_input_event(
            runtime,
            source,
            H2_RUNTIME_COMPONENT_EVENT_BUTTON_UP,
            now_ms,
            &event,
            sizeof(event));
        if (rc != H2_PAL_OK) {
            return rc;
        }
    }
    if (emit_action != 0) {
        const uint64_t elapsed_ms = now_ms >= button->pressed_at_ms
                                        ? now_ms - button->pressed_at_ms
                                        : 0u;
        const h2_runtime_button_action_event_t event = {
            .pressed_at_ms = button->pressed_at_ms,
            .released_at_ms =
                action_phase == H2_RUNTIME_BUTTON_ACTION_PHASE_RELEASED
                    ? now_ms
                    : 0u,
            .click_count = button->click_count,
            .phase = action_phase,
            .duration_ms = elapsed_ms > UINT32_MAX ? UINT32_MAX
                                                   : (uint32_t)elapsed_ms,
        };
        return append_input_event(
            runtime,
            source,
            H2_RUNTIME_COMPONENT_EVENT_BUTTON_ACTION,
            now_ms,
            &event,
            sizeof(event));
    }
    return H2_PAL_OK;
}

static h2_pal_result_t poll_single_button(
    h2_runtime_t *runtime,
    h2_runtime_input_source_t *source,
    h2_runtime_timestamp_ms_t now_ms) {
    h2_pal_single_button_reading_t reading;
    h2_pal_result_t rc = h2_pal_button_read_single_button(
        runtime->button,
        source->periph_id,
        &reading);
    if (rc != H2_PAL_OK) {
        return publish_error(runtime, source, rc, now_ms);
    }
    return update_button_pressed(
        runtime,
        source,
        reading.state == H2_PAL_BUTTON_STATE_PRESSED,
        now_ms);
}

static h2_pal_result_t consume_push_button_edges(h2_runtime_t *runtime) {
    h2_runtime_button_push_edge_t edge;
    for (;;) {
        h2_pal_result_t rc = h2_pal_queue_recv(
            runtime->queue,
            runtime->private_state->input_push_edge_queue,
            &edge,
            H2_PAL_QUEUE_NO_WAIT);
        if (rc == H2_PAL_ERR_TIMEOUT || rc == H2_PAL_ERR_WOULD_BLOCK) {
            return H2_PAL_OK;
        }
        if (rc != H2_PAL_OK) {
            return rc;
        }
        h2_runtime_input_source_t *source =
            find_push_button_source(runtime, edge.periph_id);
        if (source == NULL) {
            /*
             * The edge was validated against the mapping when it was
             * pushed; it only misses a source when the source table was
             * replaced underneath it (test control took over, or the
             * sources are being rediscovered). Drop it instead of
             * faulting the input worker.
             */
            continue;
        }
        rc = update_button_pressed(
            runtime,
            source,
            edge.edge == H2_RUNTIME_BUTTON_EDGE_DOWN,
            edge.timestamp_ms);
        if (rc != H2_PAL_OK) {
            return rc;
        }
        source->skip_poll_once = 1;
        rc = publish_pending_events(runtime);
        if (rc != H2_PAL_OK) {
            return rc;
        }
    }
}

static h2_pal_result_t poll_radio_button_group(
    h2_runtime_t *runtime,
    h2_runtime_input_source_t *source,
    h2_runtime_timestamp_ms_t now_ms) {
    h2_pal_radio_button_group_reading_t reading;
    h2_pal_result_t rc = h2_pal_button_read_radio_button_group(
        runtime->button,
        source->group_id,
        &reading);
    /*
     * One pressed_button_id is projected across every child. A successful
     * group state therefore has at most one pressed recognizer, so a
     * transition emits at most old UP + CLICK and new DOWN.
     */
    for (size_t i = 0u; i < runtime->private_state->input_source_count; ++i) {
        h2_runtime_input_source_t *group_source =
            &runtime->private_state->input_sources[i];
        if (group_source->kind != H2_RUNTIME_INPUT_SOURCE_RADIO_BUTTON ||
            group_source->group_id != source->group_id) {
            continue;
        }
        h2_pal_result_t update_rc =
            rc == H2_PAL_OK
                ? update_button_pressed(
                      runtime,
                      group_source,
                      reading.pressed_button_id ==
                          group_source->periph_id,
                      now_ms)
                : publish_error(runtime, group_source, rc, now_ms);
        if (update_rc != H2_PAL_OK) {
            return update_rc;
        }
    }
    return H2_PAL_OK;
}

static int nfc_uid_same(
    const h2_runtime_nfc_state_t *state,
    const h2_pal_nfc_scan_t *scan) {
    return state->uid_len == scan->uid_len &&
           memcmp(state->uid, scan->uid, scan->uid_len) == 0;
}

static h2_pal_result_t apply_nfc_scan(
    h2_runtime_t *runtime,
    h2_runtime_input_source_t *source,
    const h2_pal_nfc_scan_t *scan,
    h2_runtime_timestamp_ms_t now_ms) {
    if (scan->uid_len > H2_PAL_NFC_UID_MAX_LEN) {
        return publish_error(
            runtime,
            source,
            H2_PAL_ERR_INVALID_ARG,
            now_ms);
    }

    int changed =
        source->nfc_state.stage != scan->stage ||
        source->nfc_state.tag_type != scan->tag_type ||
        source->nfc_state.result != scan->result ||
        !nfc_uid_same(&source->nfc_state, scan);

    if (changed == 0) {
        return H2_PAL_OK;
    }
    source->nfc_state.stage = scan->stage;
    source->nfc_state.tag_type = scan->tag_type;
    source->nfc_state.uid_len = scan->uid_len;
    memcpy(
        source->nfc_state.uid,
        scan->uid,
        sizeof(source->nfc_state.uid));
    source->nfc_state.result = scan->result;
    source->nfc_state.updated_at_ms = now_ms;
    if (scan->stage == H2_PAL_NFC_STAGE_DISCOVERED) {
        source->nfc_state.status = H2_RUNTIME_NFC_STATE_DISCOVERED;
    } else if (scan->stage == H2_PAL_NFC_STAGE_ABSENT) {
        source->nfc_state.status = H2_RUNTIME_NFC_STATE_ABSENT;
    } else {
        source->nfc_state.status = H2_RUNTIME_NFC_STATE_ERROR;
    }
    source->timestamp_ms = now_ms;
    h2_runtime_state_mark_dirty(runtime);
    h2_runtime_nfc_state_t payload = source->nfc_state;
    return append_input_event(
        runtime,
        source,
        H2_RUNTIME_COMPONENT_EVENT_NFC_STATE,
        now_ms,
        &payload,
        sizeof(payload));
}

static h2_runtime_input_source_t *find_nfc_source(
    h2_runtime_t *runtime, h2_pal_periph_id_t periph_id) {
    for (size_t index = 0u;
         index < runtime->private_state->input_source_count;
         ++index) {
        h2_runtime_input_source_t *source =
            &runtime->private_state->input_sources[index];
        if (source->kind == H2_RUNTIME_INPUT_SOURCE_NFC_READER &&
            source->periph_id == periph_id) {
            return source;
        }
    }
    return NULL;
}

static h2_pal_result_t consume_nfc_results(
    h2_runtime_t *runtime, h2_runtime_timestamp_ms_t now_ms) {
    for (;;) {
        h2_runtime_input_nfc_result_t result;
        h2_pal_result_t rc = h2_pal_queue_recv(
            runtime->queue, runtime->private_state->input_nfc_result_queue,
            &result, H2_PAL_QUEUE_NO_WAIT);
        if (rc == H2_PAL_ERR_TIMEOUT || rc == H2_PAL_ERR_WOULD_BLOCK) {
            return H2_PAL_OK;
        }
        if (rc != H2_PAL_OK) {
            return rc;
        }
        if (runtime->private_state->test_control != NULL) {
            /* A test-control session owns NFC input; production scan
             * results must not mutate the test state or emit events. */
            continue;
        }
        h2_runtime_input_source_t *source =
            find_nfc_source(runtime, result.periph_id);
        if (source != NULL) {
            rc = apply_nfc_scan(runtime, source, &result.scan, now_ms);
            if (rc != H2_PAL_OK) {
                return rc;
            }
        }
    }
}

static h2_pal_result_t emit_imu_gesture(
    h2_runtime_t *runtime,
    h2_runtime_input_source_t *source,
    h2_runtime_timestamp_ms_t now_ms,
    const h2_runtime_imu_gesture_event_t *event) {
    source->imu_state.gesture_kind = event->kind;
    source->imu_state.updated_at_ms = now_ms;
    source->imu_state.result = H2_PAL_OK;
    switch (event->kind) {
    case H2_RUNTIME_IMU_GESTURE_SHAKE:
        source->imu_state.gesture.shake = event->gesture.shake;
        break;
    case H2_RUNTIME_IMU_GESTURE_TILT:
        source->imu_state.gesture.tilt = event->gesture.tilt;
        break;
    case H2_RUNTIME_IMU_GESTURE_FLIP:
        source->imu_state.gesture.flip = event->gesture.flip;
        break;
    case H2_RUNTIME_IMU_GESTURE_FREE_FALL:
        source->imu_state.gesture.free_fall =
            event->gesture.free_fall;
        break;
    default:
        break;
    }
    h2_runtime_state_mark_dirty(runtime);
    return append_input_event(
        runtime,
        source,
        H2_RUNTIME_COMPONENT_EVENT_IMU_GESTURE,
        now_ms,
        event,
        sizeof(*event));
}

static h2_pal_result_t poll_imu(
    h2_runtime_t *runtime,
    h2_runtime_input_source_t *source,
    h2_runtime_timestamp_ms_t now_ms) {
    h2_pal_imu_reading_t reading;
    h2_pal_result_t rc =
        h2_pal_imu_read(runtime->imu, source->periph_id, &reading);
    if (rc != H2_PAL_OK) {
        return publish_error(runtime, source, rc, now_ms);
    }

    const int has_accel =
        (reading.flags & H2_PAL_IMU_HAS_ACCEL) != 0u;
    const int has_gyro =
        (reading.flags & H2_PAL_IMU_HAS_GYRO) != 0u;
    const int64_t accel_magnitude =
        has_accel != 0
            ? max3_abs_i32(
                  reading.accel_mg.x,
                  reading.accel_mg.y,
                  reading.accel_mg.z)
            : 0;
    h2_runtime_imu_gesture_event_t event = {0};

    if (has_accel != 0 &&
        accel_magnitude >= H2_RUNTIME_IMU_SHAKE_THRESHOLD_MG) {
        if (source->imu.shaking == 0) {
            source->imu.shaking = 1;
            source->imu.shake_started_ms = now_ms;
        } else if (
            now_ms - source->imu.shake_started_ms >=
            H2_RUNTIME_IMU_SHAKE_MIN_DURATION_MS) {
            source->imu.shaking = 0;
            event.kind = H2_RUNTIME_IMU_GESTURE_SHAKE;
            event.gesture.shake.magnitude_mg =
                clamp_i64_to_i32(accel_magnitude);
            event.gesture.shake.duration_ms =
                (uint32_t)(now_ms - source->imu.shake_started_ms);
            return emit_imu_gesture(
                runtime,
                source,
                now_ms,
                &event);
        }
    } else {
        source->imu.shaking = 0;
    }

    if (has_accel != 0 &&
        accel_magnitude <= H2_RUNTIME_IMU_FREE_FALL_THRESHOLD_MG) {
        if (source->imu.free_falling == 0) {
            source->imu.free_falling = 1;
            source->imu.free_fall_started_ms = now_ms;
        } else if (
            now_ms - source->imu.free_fall_started_ms >=
            H2_RUNTIME_IMU_FREE_FALL_MIN_DURATION_MS) {
            source->imu.free_falling = 0;
            event.kind = H2_RUNTIME_IMU_GESTURE_FREE_FALL;
            event.gesture.free_fall.duration_ms =
                (uint32_t)(
                    now_ms - source->imu.free_fall_started_ms);
            event.gesture.free_fall.magnitude_mg =
                clamp_i64_to_i32(accel_magnitude);
            return emit_imu_gesture(
                runtime,
                source,
                now_ms,
                &event);
        }
    } else {
        source->imu.free_falling = 0;
    }

    if (has_accel != 0 && source->imu.shaking == 0 &&
        (abs_i32_to_i64(reading.accel_mg.x) >=
             H2_RUNTIME_IMU_TILT_THRESHOLD_MG ||
         abs_i32_to_i64(reading.accel_mg.y) >=
             H2_RUNTIME_IMU_TILT_THRESHOLD_MG) &&
        now_ms - source->imu.last_tilt_at_ms >=
            H2_RUNTIME_IMU_TILT_DEBOUNCE_MS) {
        source->imu.last_tilt_at_ms = now_ms;
        event.kind = H2_RUNTIME_IMU_GESTURE_TILT;
        event.gesture.tilt.x_mg = reading.accel_mg.x;
        event.gesture.tilt.y_mg = reading.accel_mg.y;
        event.gesture.tilt.z_mg = reading.accel_mg.z;
        return emit_imu_gesture(runtime, source, now_ms, &event);
    }

    if (has_gyro != 0 &&
        abs_i32_to_i64(reading.gyro_mdps.z) >=
            H2_RUNTIME_IMU_FLIP_GYRO_THRESHOLD_MDPS &&
        now_ms - source->imu.last_flip_at_ms >=
            H2_RUNTIME_IMU_FLIP_DEBOUNCE_MS) {
        source->imu.last_flip_at_ms = now_ms;
        event.kind = H2_RUNTIME_IMU_GESTURE_FLIP;
        event.gesture.flip.gyro_z_mdps = reading.gyro_mdps.z;
        return emit_imu_gesture(runtime, source, now_ms, &event);
    }

    return H2_PAL_OK;
}

static h2_pal_result_t poll_battery(
    h2_runtime_t *runtime,
    h2_runtime_input_source_t *source,
    h2_runtime_timestamp_ms_t now_ms) {
    h2_pal_battery_reading_t reading = {0};
    h2_pal_result_t rc = h2_pal_input_read_battery(
        runtime->input, source->periph_id, &reading);
    if (rc == H2_PAL_OK) {
        source->battery_state.reading = reading;
    }
    source->battery_state.result = rc;
    source->battery_state.updated_at_ms = now_ms;
    source->timestamp_ms = now_ms;
    h2_runtime_state_mark_dirty(runtime);
    return H2_PAL_OK;
}

static h2_pal_result_t poll_temperature(
    h2_runtime_t *runtime,
    h2_runtime_input_source_t *source,
    h2_runtime_timestamp_ms_t now_ms) {
    h2_pal_temperature_reading_t reading = {0};
    h2_pal_result_t rc = h2_pal_input_read_temperature(
        runtime->input, source->periph_id, &reading);
    if (rc == H2_PAL_OK) {
        source->temperature_state.reading = reading;
    }
    source->temperature_state.result = rc;
    source->temperature_state.updated_at_ms = now_ms;
    source->timestamp_ms = now_ms;
    h2_runtime_state_mark_dirty(runtime);
    return H2_PAL_OK;
}

static h2_pal_result_t input_poll_once_unlocked(
    h2_runtime_t *runtime,
    int force_due,
    int sensor_only) {
    if (!h2_runtime_ready(runtime)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    h2_runtime_timestamp_ms_t now_ms = 0u;
    h2_pal_result_t rc = input_now(runtime, &now_ms);
    if (rc != H2_PAL_OK) {
        return rc;
    }

    /*
     * While a test-control session owns the input, its source table is
     * authoritative: it must not be replaced by physical discovery, and only
     * the passive sensors (Battery/Temperature) keep reading their PAL.
     */
    if (runtime->private_state->test_control == NULL) {
        rc = ensure_input_sources(runtime, now_ms);
        if (rc != H2_PAL_OK) {
            return rc;
        }
    }

    /* Pending events left by a deferred snapshot publication are kept and
     * published together with the snapshot below. */

    rc = consume_push_button_edges(runtime);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    rc = consume_nfc_results(runtime, now_ms);
    if (rc != H2_PAL_OK) {
        return rc;
    }

    for (size_t i = 0u; i < runtime->private_state->input_source_count; ++i) {
        h2_runtime_input_source_t *source =
            &runtime->private_state->input_sources[i];
        if (source->skip_poll_once != 0) {
            source->skip_poll_once = 0;
            continue;
        }
        if ((sensor_only != 0 ||
             runtime->private_state->test_control != NULL) &&
            source->kind != H2_RUNTIME_INPUT_SOURCE_BATTERY &&
            source->kind != H2_RUNTIME_INPUT_SOURCE_TEMPERATURE) {
            continue;
        }
        if (force_due == 0 &&
            h2_pal_time_deadline_expired(
                now_ms, source->next_due_ms) == 0) {
            continue;
        }
        source->next_due_ms = h2_pal_time_deadline_ms(
            now_ms, source->poll_interval_ms);
        if (source->kind == H2_RUNTIME_INPUT_SOURCE_SINGLE_BUTTON) {
            rc = source->button_delivery == H2_PAL_BUTTON_DELIVERY_PUSH_EDGE
                     ? update_button_pressed(
                           runtime,
                           source,
                           source->button.is_pressed,
                           now_ms)
                     : poll_single_button(runtime, source, now_ms);
        } else if (
            source->kind == H2_RUNTIME_INPUT_SOURCE_RADIO_BUTTON) {
            int already_polled = 0;
            for (size_t previous = 0u; previous < i; ++previous) {
                const h2_runtime_input_source_t *candidate =
                    &runtime->private_state->input_sources[previous];
                if (candidate->kind ==
                        H2_RUNTIME_INPUT_SOURCE_RADIO_BUTTON &&
                    candidate->group_id == source->group_id) {
                    already_polled = 1;
                    break;
                }
            }
            rc = already_polled != 0
                     ? H2_PAL_OK
                     : poll_radio_button_group(runtime, source, now_ms);
        } else if (
            source->kind == H2_RUNTIME_INPUT_SOURCE_NFC_READER) {
            rc = H2_PAL_OK;
        } else if (source->kind == H2_RUNTIME_INPUT_SOURCE_IMU) {
            rc = poll_imu(runtime, source, now_ms);
        } else if (source->kind == H2_RUNTIME_INPUT_SOURCE_BATTERY) {
            rc = poll_battery(runtime, source, now_ms);
        } else if (source->kind == H2_RUNTIME_INPUT_SOURCE_TEMPERATURE) {
            rc = poll_temperature(runtime, source, now_ms);
        } else {
            rc = H2_PAL_ERR_INVALID_STATE;
        }
        if (rc != H2_PAL_OK) {
            runtime->private_state->input_pending_event_count = 0u;
            return rc;
        }
        if (runtime->private_state->input_pending_event_count != 0u) {
            rc = publish_pending_events(runtime);
            if (rc != H2_PAL_OK) {
                return rc;
            }
        }
    }

    return publish_snapshot_if_due(runtime, now_ms, force_due);
}

/*
 * The writer mutex serialises every mutator of the input source table and
 * test-control state (poll task, explicit polls, push edges, test-session
 * open/close) so a session transition can never interleave with a poll
 * and emit events from the wrong source set. It is NULL while input is
 * stopped; callers then run unlocked because no poll task exists.
 */
static h2_pal_result_t input_writer_lock(h2_runtime_t *runtime) {
    if (runtime->private_state->input_writer_mutex == NULL) {
        return H2_PAL_OK;
    }
    return h2_pal_mutex_lock(
        runtime->sync, runtime->private_state->input_writer_mutex);
}

static h2_pal_result_t input_writer_unlock(h2_runtime_t *runtime) {
    if (runtime->private_state->input_writer_mutex == NULL) {
        return H2_PAL_OK;
    }
    return h2_pal_mutex_unlock(
        runtime->sync, runtime->private_state->input_writer_mutex);
}

static int input_test_control_active(h2_runtime_t *runtime) {
    if (input_writer_lock(runtime) != H2_PAL_OK) {
        return 1;
    }
    const int active = runtime->private_state->test_control != NULL;
    (void)input_writer_unlock(runtime);
    return active;
}

static void input_writer_mutex_destroy(h2_runtime_t *runtime) {
    if (runtime->private_state->input_writer_mutex != NULL) {
        (void)h2_pal_mutex_destroy(
            runtime->sync, runtime->private_state->input_writer_mutex);
        runtime->private_state->input_writer_mutex = NULL;
    }
}

static h2_pal_result_t input_poll_once_locked(
    h2_runtime_t *runtime, int force_due, int sensor_only) {
    h2_pal_result_t rc = input_writer_lock(runtime);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    rc = input_poll_once_unlocked(runtime, force_due, sensor_only);
    const h2_pal_result_t unlock_rc = input_writer_unlock(runtime);
    return rc != H2_PAL_OK ? rc : unlock_rc;
}

h2_pal_result_t h2_runtime_input_poll_once(h2_runtime_t *runtime) {
    if (!h2_runtime_ready(runtime)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    return input_poll_once_locked(runtime, 1, 0);
}

h2_pal_result_t h2_runtime_input_poll_sensors_once(h2_runtime_t *runtime) {
    if (!h2_runtime_ready(runtime)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    return input_poll_once_locked(runtime, 1, 1);
}

static void input_task_entry(void *ctx) {
    h2_runtime_t *runtime = (h2_runtime_t *)ctx;
    while (atomic_load(&runtime->private_state->input_stop_requested) == 0) {
        h2_pal_result_t rc = input_poll_once_locked(runtime, 0, 0);
        if (rc == H2_PAL_OK &&
            atomic_load(&runtime->private_state->input_stop_requested) == 0) {
            rc = h2_pal_time_sleep_ms(
                runtime->time, runtime->private_state->input_tick_ms);
        }
        if (rc != H2_PAL_OK) {
            atomic_store(&runtime->private_state->input_worker_result, rc);
            atomic_store(&runtime->private_state->input_stop_requested, 1);
            atomic_store(
                &runtime->private_state->input_phase,
                H2_RUNTIME_INPUT_PHASE_FAULTED);
            (void)h2_pal_queue_close(
                runtime->queue, runtime->private_state->event_queue);
            return;
        }
    }
}

static int has_nfc_source(const h2_runtime_t *runtime) {
    for (size_t index = 0u;
         index < runtime->private_state->component_mapping_count;
         ++index) {
        if (runtime->private_state->component_mappings[index].component ==
            H2_RUNTIME_COMPONENT_NFC_READER) {
            return 1;
        }
    }
    return 0;
}

static void input_nfc_task_entry(void *ctx) {
    h2_runtime_t *runtime = (h2_runtime_t *)ctx;
    while (atomic_load(&runtime->private_state->input_stop_requested) == 0) {
        for (size_t index = 0u;
             index < runtime->private_state->component_mapping_count &&
             atomic_load(&runtime->private_state->input_stop_requested) == 0;
             ++index) {
            const h2_runtime_component_mapping_t *mapping =
                &runtime->private_state->component_mappings[index];
            if (mapping->component != H2_RUNTIME_COMPONENT_NFC_READER ||
                input_test_control_active(runtime)) {
                continue;
            }
            h2_runtime_input_nfc_result_t result = {
                .periph_id = mapping->periph_id,
            };
            h2_pal_result_t rc = h2_pal_nfc_scan_nfc_reader(
                runtime->nfc, mapping->periph_id, &result.scan);
            if (rc != H2_PAL_OK) {
                memset(&result.scan, 0, sizeof(result.scan));
                result.scan.stage = H2_PAL_NFC_STAGE_ERROR;
                result.scan.result = rc;
            }
            rc = h2_pal_queue_send(
                runtime->queue,
                runtime->private_state->input_nfc_result_queue,
                &result,
                H2_PAL_QUEUE_NO_WAIT);
            if (rc == H2_PAL_ERR_FULL || rc == H2_PAL_ERR_TIMEOUT ||
                rc == H2_PAL_ERR_WOULD_BLOCK) {
                h2_runtime_input_nfc_result_t discarded;
                (void)h2_pal_queue_recv(
                    runtime->queue,
                    runtime->private_state->input_nfc_result_queue,
                    &discarded,
                    H2_PAL_QUEUE_NO_WAIT);
                rc = h2_pal_queue_send(
                    runtime->queue,
                    runtime->private_state->input_nfc_result_queue,
                    &result,
                    H2_PAL_QUEUE_NO_WAIT);
            }
            if (rc != H2_PAL_OK) {
                atomic_store(&runtime->private_state->input_worker_result, rc);
                atomic_store(&runtime->private_state->input_stop_requested, 1);
                atomic_store(&runtime->private_state->input_phase,
                             H2_RUNTIME_INPUT_PHASE_FAULTED);
                (void)h2_pal_queue_close(
                    runtime->queue, runtime->private_state->event_queue);
                return;
            }
        }
        if (atomic_load(&runtime->private_state->input_stop_requested) == 0) {
            h2_pal_result_t rc = h2_pal_time_sleep_ms(
                runtime->time,
                runtime->private_state->input_nfc_poll_interval_ms);
            if (rc != H2_PAL_OK) {
                atomic_store(&runtime->private_state->input_worker_result, rc);
                atomic_store(&runtime->private_state->input_stop_requested, 1);
                atomic_store(&runtime->private_state->input_phase,
                             H2_RUNTIME_INPUT_PHASE_FAULTED);
                (void)h2_pal_queue_close(
                    runtime->queue, runtime->private_state->event_queue);
                return;
            }
        }
    }
}

h2_pal_result_t h2_runtime_input_prepare(h2_runtime_t *runtime) {
    if (!h2_runtime_ready(runtime)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    /*
     * Everything here is derived from data that is fixed for the life of the
     * Runtime: the component mapping and the board periph inventory. It is
     * built once, during h2_runtime_init(), and survives every input
     * start/stop cycle. Discovery failures are configuration errors and are
     * reported by init, not by a later start.
     *
     * A Runtime whose mapping has no input component can never acquire input,
     * so nothing is built and Sync, Periph and Time stay unused. That keeps
     * headless and capability-only images initializable on boards that bind
     * the unsupported providers for those capabilities.
     */
    if (has_mapped_input_component(runtime) == 0) {
        return H2_PAL_OK;
    }
    const h2_pal_mutex_config_t writer_mutex_config = {
        .name = "h2-runtime-input-writer",
        .allocator = runtime->mem,
        .flags = H2_PAL_MUTEX_FLAG_NONE,
    };
    h2_pal_result_t rc = h2_pal_mutex_create(
        runtime->sync,
        &writer_mutex_config,
        &runtime->private_state->input_writer_mutex);
    if (rc != H2_PAL_OK) {
        runtime->private_state->input_writer_mutex = NULL;
        return rc;
    }
    rc = h2_runtime_state_publication_init(runtime);
    if (rc != H2_PAL_OK) {
        input_writer_mutex_destroy(runtime);
        return rc;
    }
    /*
     * Discovery records metadata only, so it needs no clock: every source is
     * stamped by the forced first frame that h2_runtime_input_start() takes.
     * Keeping Time out of init lets a Runtime with an unsupported Time
     * provider still initialize.
     */
    rc = ensure_input_sources(runtime, 0u);
    if (rc != H2_PAL_OK) {
        reset_input_sources(runtime);
        runtime->private_state->input_sources_ready = 0;
        h2_runtime_state_publication_deinit(runtime);
        input_writer_mutex_destroy(runtime);
        return rc;
    }
    return H2_PAL_OK;
}

void h2_runtime_input_release(h2_runtime_t *runtime) {
    if (runtime == NULL || runtime->private_state == NULL) {
        return;
    }
    reset_input_sources(runtime);
    runtime->private_state->input_sources_ready = 0;
    h2_runtime_state_publication_deinit(runtime);
    input_writer_mutex_destroy(runtime);
}

h2_pal_result_t h2_runtime_input_start(
    h2_runtime_t *runtime,
    const h2_runtime_input_poll_config_t *config) {
    if (!h2_runtime_ready(runtime)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    /*
     * A test-control session owns the input source table for as long as it is
     * open. Running the poller underneath it would race the input task
     * against the test sources, so refuse until the session closes.
     */
    if (runtime->private_state->test_control != NULL) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    /*
     * A worker fault closes the Runtime event queue to wake blocked consumers,
     * and the PAL queue contract has no reopen. Restarting the poller would
     * produce a Runtime that samples input it can never deliver, so the fault
     * is terminal: only deinit plus a fresh init recovers. The latched worker
     * result is the fault record and is never cleared by a start.
     */
    if (atomic_load(&runtime->private_state->input_worker_result) !=
        H2_PAL_OK) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    if (atomic_load(&runtime->private_state->input_phase) !=
        H2_RUNTIME_INPUT_PHASE_STOPPED) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    atomic_store(
        &runtime->private_state->input_phase,
        H2_RUNTIME_INPUT_PHASE_STARTING);

    h2_runtime_input_poll_config_t selected = {0};
    if (config != NULL) {
        selected = *config;
    }
    selected.task_options.name = h2_runtime_input_task_name;
    runtime->private_state->input_tick_ms =
        select_interval(selected.tick_ms, H2_RUNTIME_BUTTON_POLL_INTERVAL_MS);
    runtime->private_state->input_button_poll_interval_ms = select_interval(
        selected.button_poll_interval_ms,
        H2_RUNTIME_BUTTON_POLL_INTERVAL_MS);
    runtime->private_state->input_nfc_poll_interval_ms = select_interval(
        selected.nfc_poll_interval_ms,
        H2_RUNTIME_NFC_POLL_INTERVAL_MS);
    runtime->private_state->input_imu_poll_interval_ms = select_interval(
        selected.imu_poll_interval_ms,
        H2_RUNTIME_IMU_POLL_INTERVAL_MS);
    runtime->private_state->input_battery_poll_interval_ms = select_interval(
        selected.battery_poll_interval_ms,
        H2_RUNTIME_BATTERY_POLL_INTERVAL_MS);
    runtime->private_state->input_temperature_poll_interval_ms = select_interval(
        selected.temperature_poll_interval_ms,
        H2_RUNTIME_TEMPERATURE_POLL_INTERVAL_MS);
    atomic_store(&runtime->private_state->input_stop_requested, 0);

    /*
     * Take one immediate frame so the first publication after a start reflects
     * the current hardware rather than whatever the poller last observed
     * before it was stopped. This is a poll, not a state reset: component
     * state stays whatever was last published until this frame replaces it.
     *
     * The poll also rebuilds the source table in the one case where it is not
     * already valid: a closed test-control session leaves it invalidated for
     * lazy rediscovery. On the normal path the table was built during init
     * and this is a no-op.
     */
    h2_pal_result_t rc = input_poll_once_locked(runtime, 1, 0);
    if (rc != H2_PAL_OK) {
        atomic_store(
            &runtime->private_state->input_phase,
            H2_RUNTIME_INPUT_PHASE_STOPPED);
        return rc;
    }
    if (runtime->private_state->input_source_count == 0u) {
        atomic_store(
            &runtime->private_state->input_phase,
            H2_RUNTIME_INPUT_PHASE_STOPPED);
        return H2_PAL_OK;
    }

    atomic_store(
        &runtime->private_state->input_phase,
        H2_RUNTIME_INPUT_PHASE_TASK_RUNNING);
    rc = h2_pal_task_start(
        runtime->task,
        &selected.task_options,
        input_task_entry,
        runtime,
        &runtime->private_state->input_task);
    if (rc != H2_PAL_OK || runtime->private_state->input_task == NULL) {
        runtime->private_state->input_task = NULL;
        atomic_store(
            &runtime->private_state->input_phase,
            H2_RUNTIME_INPUT_PHASE_STOPPED);
        return rc != H2_PAL_OK ? rc : H2_PAL_ERR_TASK;
    }
    if (has_nfc_source(runtime)) {
        h2_pal_task_options_t nfc_options = selected.task_options;
        nfc_options.name = h2_runtime_nfc_task_name;
        rc = h2_pal_task_start(
            runtime->task,
            &nfc_options,
            input_nfc_task_entry,
            runtime,
            &runtime->private_state->input_nfc_task);
        if (rc != H2_PAL_OK || runtime->private_state->input_nfc_task == NULL) {
            runtime->private_state->input_nfc_task = NULL;
            atomic_store(&runtime->private_state->input_stop_requested, 1);
            (void)h2_pal_task_join(
                runtime->task, runtime->private_state->input_task);
            runtime->private_state->input_task = NULL;
            atomic_store(&runtime->private_state->input_stop_requested, 0);
            atomic_store(&runtime->private_state->input_phase,
                         H2_RUNTIME_INPUT_PHASE_STOPPED);
            return rc != H2_PAL_OK ? rc : H2_PAL_ERR_TASK;
        }
    }
    return H2_PAL_OK;
}

h2_pal_result_t h2_runtime_input_stop(h2_runtime_t *runtime) {
    if (!h2_runtime_ready(runtime)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    int phase = atomic_load(&runtime->private_state->input_phase);
    if (phase == H2_RUNTIME_INPUT_PHASE_STOPPED) {
        return H2_PAL_OK;
    }
    if (phase != H2_RUNTIME_INPUT_PHASE_TASK_RUNNING &&
        phase != H2_RUNTIME_INPUT_PHASE_FAULTED) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    atomic_store(
        &runtime->private_state->input_phase,
        H2_RUNTIME_INPUT_PHASE_STOPPING);
    atomic_store(&runtime->private_state->input_stop_requested, 1);
    if (runtime->private_state->input_nfc_task != NULL) {
        h2_pal_result_t rc = h2_pal_task_join(
            runtime->task, runtime->private_state->input_nfc_task);
        if (rc != H2_PAL_OK) {
            atomic_store(&runtime->private_state->input_phase, phase);
            return rc;
        }
        runtime->private_state->input_nfc_task = NULL;
    }
    if (runtime->private_state->input_task != NULL) {
        h2_pal_result_t rc = h2_pal_task_join(
            runtime->task, runtime->private_state->input_task);
        if (rc != H2_PAL_OK) {
            atomic_store(&runtime->private_state->input_phase, phase);
            return rc;
        }
        runtime->private_state->input_task = NULL;
    }
    /*
     * Stopping the poller does not touch component state. The last published
     * snapshot stays readable with its own updated_at_ms, because it is still
     * the most recent observation the Runtime actually made.
     */
    atomic_store(&runtime->private_state->input_stop_requested, 0);
    atomic_store(
        &runtime->private_state->input_phase,
        H2_RUNTIME_INPUT_PHASE_STOPPED);
    return atomic_load(&runtime->private_state->input_worker_result);
}

static h2_pal_result_t button_push_edge_enqueue(
    h2_runtime_t *runtime,
    h2_pal_periph_id_t periph_id,
    h2_runtime_button_edge_t edge) {
    if (runtime->private_state->test_control != NULL) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    h2_runtime_timestamp_ms_t now_ms = 0u;
    h2_pal_result_t rc = input_now(runtime, &now_ms);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    rc = ensure_input_sources(runtime, now_ms);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    if (find_push_button_source(runtime, periph_id) == NULL) {
        return H2_PAL_ERR_NOT_FOUND;
    }
    const h2_runtime_button_push_edge_t pushed = {
        .periph_id = periph_id,
        .edge = edge,
        .timestamp_ms = now_ms,
    };
    return h2_pal_queue_send(
        runtime->queue,
        runtime->private_state->input_push_edge_queue,
        &pushed,
        H2_PAL_QUEUE_NO_WAIT);
}

h2_pal_result_t h2_runtime_button_push_edge(
    h2_runtime_t *runtime,
    h2_pal_periph_id_t periph_id,
    h2_runtime_button_edge_t edge) {
    if (!h2_runtime_ready(runtime) || periph_id == 0u ||
        (edge != H2_RUNTIME_BUTTON_EDGE_DOWN &&
         edge != H2_RUNTIME_BUTTON_EDGE_UP)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    /* Push edges hand off through a queue consumed under the writer mutex
     * by the poller, so the producer never blocks on a slow poll. */
    return button_push_edge_enqueue(runtime, periph_id, edge);
}

static h2_pal_result_t prepare_test_input_sources(
    h2_runtime_t *runtime) {
    reset_input_sources(runtime);
    for (size_t index = 0u;
         index < runtime->private_state->component_mapping_count;
         ++index) {
        const h2_runtime_component_mapping_t *mapping =
            &runtime->private_state->component_mappings[index];
        if (mapping->component != H2_RUNTIME_COMPONENT_BUTTON &&
            mapping->component != H2_RUNTIME_COMPONENT_NFC_READER &&
            mapping->component != H2_RUNTIME_COMPONENT_IMU &&
            mapping->component != H2_RUNTIME_COMPONENT_BATTERY &&
            mapping->component != H2_RUNTIME_COMPONENT_TEMPERATURE_SENSOR) {
            continue;
        }
        if (runtime->private_state->input_source_count >=
            runtime->private_state->input_source_capacity) {
            reset_input_sources(runtime);
            return H2_PAL_ERR_NO_SPACE;
        }
        h2_runtime_input_source_t *source =
            &runtime->private_state->input_sources[
                runtime->private_state->input_source_count++];
        source->component = mapping->component;
        source->component_id = mapping->component_id;
        source->periph_id = mapping->periph_id;
        if (mapping->component == H2_RUNTIME_COMPONENT_BUTTON) {
            source->kind = H2_RUNTIME_INPUT_SOURCE_SINGLE_BUTTON;
            source->button_state.result = H2_PAL_OK;
        } else if (
            mapping->component == H2_RUNTIME_COMPONENT_NFC_READER) {
            source->kind = H2_RUNTIME_INPUT_SOURCE_NFC_READER;
            source->nfc_state.status = H2_RUNTIME_NFC_STATE_NONE;
            source->nfc_state.result = H2_PAL_OK;
        } else if (mapping->component == H2_RUNTIME_COMPONENT_IMU) {
            source->kind = H2_RUNTIME_INPUT_SOURCE_IMU;
            source->imu_state.result = H2_PAL_OK;
        } else if (mapping->component == H2_RUNTIME_COMPONENT_BATTERY) {
            source->kind = H2_RUNTIME_INPUT_SOURCE_BATTERY;
            source->poll_interval_ms =
                runtime->private_state->input_battery_poll_interval_ms;
            source->battery_state.result = H2_PAL_ERR_WOULD_BLOCK;
        } else if (
            mapping->component == H2_RUNTIME_COMPONENT_TEMPERATURE_SENSOR) {
            source->kind = H2_RUNTIME_INPUT_SOURCE_TEMPERATURE;
            source->poll_interval_ms =
                runtime->private_state->input_temperature_poll_interval_ms;
            source->temperature_state.result = H2_PAL_ERR_WOULD_BLOCK;
        } else {
            return H2_PAL_ERR_INVALID_ARG;
        }
    }
    return H2_PAL_OK;
}

h2_pal_result_t h2_runtime_input_test_session_open(
    h2_runtime_t *runtime,
    struct h2_runtime_test_control *control) {
    if (!h2_runtime_ready(runtime) || control == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    h2_pal_result_t rc = input_writer_lock(runtime);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    if (runtime->private_state->test_control != NULL) {
        rc = H2_PAL_ERR_INVALID_STATE;
        goto done;
    }
    runtime->private_state->test_control = control;
    rc = h2_runtime_state_publication_init(runtime);
    if (rc != H2_PAL_OK) {
        goto done;
    }
    /*
     * Physical push edges accepted before the session opened belong to the
     * production source table that is being replaced; discard them so they
     * neither leak into the test session nor get replayed after it closes.
     */
    if (runtime->private_state->input_push_edge_queue != NULL) {
        rc = h2_pal_queue_reset(
            runtime->queue, runtime->private_state->input_push_edge_queue);
        if (rc != H2_PAL_OK) {
            goto done;
        }
    }
    runtime->private_state->input_sources_ready = 0;
    reset_input_sources(runtime);
    rc = prepare_test_input_sources(runtime);
    if (rc != H2_PAL_OK) {
        goto done;
    }
    rc = publish_snapshot(runtime, 0);
    if (rc == H2_PAL_OK) {
        rc = h2_pal_queue_reset(
            runtime->queue, runtime->private_state->event_queue);
    }

done:
    if (rc != H2_PAL_OK &&
        runtime->private_state->test_control == control) {
        runtime->private_state->test_control = NULL;
        reset_input_sources(runtime);
        runtime->private_state->input_sources_ready = 0;
    }
    {
        const h2_pal_result_t unlock_rc = input_writer_unlock(runtime);
        if (rc == H2_PAL_OK) {
            rc = unlock_rc;
        }
    }
    return rc;
}

h2_pal_result_t h2_runtime_input_test_publish(
    h2_runtime_t *runtime) {
    if (!h2_runtime_ready(runtime) ||
        !h2_runtime_state_publication_ready(runtime) ||
        runtime->private_state->test_control == NULL) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    return publish_snapshot(runtime, 0);
}

h2_pal_result_t h2_runtime_input_test_writer_lock(h2_runtime_t *runtime) {
    if (!h2_runtime_ready(runtime)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    return input_writer_lock(runtime);
}

h2_pal_result_t h2_runtime_input_test_writer_unlock(h2_runtime_t *runtime) {
    if (!h2_runtime_ready(runtime)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    return input_writer_unlock(runtime);
}

h2_pal_result_t h2_runtime_input_test_session_close(
    h2_runtime_t *runtime) {
    if (!h2_runtime_ready(runtime) ||
        !h2_runtime_state_publication_ready(runtime)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    h2_pal_result_t rc = input_writer_lock(runtime);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    if (runtime->private_state->test_control == NULL) {
        rc = H2_PAL_ERR_INVALID_STATE;
        goto done;
    }
    reset_input_sources(runtime);
    runtime->private_state->input_sources_ready = 0;
    rc = publish_snapshot(runtime, 1);
    if (rc == H2_PAL_OK) {
        runtime->private_state->test_control = NULL;
    }

done:
    {
        const h2_pal_result_t unlock_rc = input_writer_unlock(runtime);
        if (rc == H2_PAL_OK) {
            rc = unlock_rc;
        }
    }
    return rc;
}
