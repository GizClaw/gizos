#include "h2_runtime_test.h"

#include "h2_runtime_internal.h"

#include <string.h>

struct h2_runtime_test_control {
    h2_runtime_t *runtime;
    int active;
};

typedef struct h2_runtime_test_event_schema {
    h2_runtime_component_t component;
    size_t payload_size;
} h2_runtime_test_event_schema_t;

static h2_pal_result_t event_schema(
    h2_runtime_event_kind_t kind,
    h2_runtime_test_event_schema_t *out_schema) {
    if (out_schema == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    switch (kind) {
    case H2_RUNTIME_SYSTEM_EVENT_NETIF_DEFAULT_CHANGED:
        *out_schema = (h2_runtime_test_event_schema_t){
            H2_RUNTIME_COMPONENT_SYSTEM_NETIF,
            sizeof(h2_runtime_system_event_netif_default_changed_t),
        };
        return H2_PAL_OK;
    case H2_RUNTIME_SYSTEM_EVENT_GPIO_IRQ_TRIGGERED:
        *out_schema = (h2_runtime_test_event_schema_t){
            H2_RUNTIME_COMPONENT_SYSTEM_GPIO_IRQ,
            sizeof(h2_runtime_system_event_gpio_irq_t),
        };
        return H2_PAL_OK;
    case H2_RUNTIME_SYSTEM_EVENT_WIFI_STA_CONNECTING:
    case H2_RUNTIME_SYSTEM_EVENT_WIFI_STA_CONNECTED:
    case H2_RUNTIME_SYSTEM_EVENT_WIFI_STA_DISCONNECTED:
    case H2_RUNTIME_SYSTEM_EVENT_WIFI_STA_GOT_IP:
    case H2_RUNTIME_SYSTEM_EVENT_WIFI_STA_LOST_IP:
        *out_schema = (h2_runtime_test_event_schema_t){
            H2_RUNTIME_COMPONENT_SYSTEM_WIFI,
            sizeof(h2_runtime_system_event_wifi_sta_t),
        };
        return H2_PAL_OK;
    case H2_RUNTIME_SYSTEM_EVENT_WIFI_AP_STARTED:
    case H2_RUNTIME_SYSTEM_EVENT_WIFI_AP_STOPPED:
        *out_schema = (h2_runtime_test_event_schema_t){
            H2_RUNTIME_COMPONENT_SYSTEM_WIFI,
            sizeof(h2_runtime_system_event_wifi_ap_t),
        };
        return H2_PAL_OK;
    case H2_RUNTIME_SYSTEM_EVENT_WIFI_AP_CLIENT_JOINED:
    case H2_RUNTIME_SYSTEM_EVENT_WIFI_AP_CLIENT_LEFT:
    case H2_RUNTIME_SYSTEM_EVENT_WIFI_AP_LEASE_GRANTED:
    case H2_RUNTIME_SYSTEM_EVENT_WIFI_AP_LEASE_RELEASED:
        *out_schema = (h2_runtime_test_event_schema_t){
            H2_RUNTIME_COMPONENT_SYSTEM_WIFI,
            sizeof(h2_runtime_system_event_wifi_ap_client_t),
        };
        return H2_PAL_OK;
    case H2_RUNTIME_SYSTEM_EVENT_BLE_HOST_STARTED:
    case H2_RUNTIME_SYSTEM_EVENT_BLE_HOST_STOPPED:
    case H2_RUNTIME_SYSTEM_EVENT_BLE_ADVERTISING_STARTED:
    case H2_RUNTIME_SYSTEM_EVENT_BLE_ADVERTISING_STOPPED:
    case H2_RUNTIME_SYSTEM_EVENT_BLE_SCAN_STARTED:
    case H2_RUNTIME_SYSTEM_EVENT_BLE_SCAN_STOPPED:
        *out_schema = (h2_runtime_test_event_schema_t){
            H2_RUNTIME_COMPONENT_SYSTEM_BLE,
            0u,
        };
        return H2_PAL_OK;
    case H2_RUNTIME_SYSTEM_EVENT_BLE_CONNECTED:
        *out_schema = (h2_runtime_test_event_schema_t){
            H2_RUNTIME_COMPONENT_SYSTEM_BLE,
            sizeof(h2_runtime_system_event_ble_connection_t),
        };
        return H2_PAL_OK;
    case H2_RUNTIME_SYSTEM_EVENT_BLE_CONNECTION_UPDATED:
        *out_schema = (h2_runtime_test_event_schema_t){
            H2_RUNTIME_COMPONENT_SYSTEM_BLE,
            sizeof(h2_runtime_system_event_ble_connection_params_t),
        };
        return H2_PAL_OK;
    case H2_RUNTIME_SYSTEM_EVENT_BLE_DISCONNECTED:
        *out_schema = (h2_runtime_test_event_schema_t){
            H2_RUNTIME_COMPONENT_SYSTEM_BLE,
            sizeof(h2_runtime_system_event_ble_disconnected_t),
        };
        return H2_PAL_OK;
    case H2_RUNTIME_SYSTEM_EVENT_BLE_MTU_CHANGED:
        *out_schema = (h2_runtime_test_event_schema_t){
            H2_RUNTIME_COMPONENT_SYSTEM_BLE,
            sizeof(h2_runtime_system_event_ble_mtu_t),
        };
        return H2_PAL_OK;
    case H2_RUNTIME_SYSTEM_EVENT_BLE_SUBSCRIPTION_CHANGED:
        *out_schema = (h2_runtime_test_event_schema_t){
            H2_RUNTIME_COMPONENT_SYSTEM_BLE,
            sizeof(h2_runtime_system_event_ble_subscription_t),
        };
        return H2_PAL_OK;
    case H2_RUNTIME_SYSTEM_EVENT_BLE_GATT_CLIENT_NOTIFICATION:
    case H2_RUNTIME_SYSTEM_EVENT_BLE_GATT_CLIENT_INDICATION:
        *out_schema = (h2_runtime_test_event_schema_t){
            H2_RUNTIME_COMPONENT_SYSTEM_BLE,
            sizeof(h2_runtime_system_event_ble_gatt_client_value_t),
        };
        return H2_PAL_OK;
    case H2_RUNTIME_SYSTEM_EVENT_MODEM_READY:
        *out_schema = (h2_runtime_test_event_schema_t){
            H2_RUNTIME_COMPONENT_SYSTEM_MODEM,
            0u,
        };
        return H2_PAL_OK;
    case H2_RUNTIME_SYSTEM_EVENT_MODEM_ERROR:
        *out_schema = (h2_runtime_test_event_schema_t){
            H2_RUNTIME_COMPONENT_SYSTEM_MODEM,
            sizeof(h2_runtime_system_event_modem_error_t),
        };
        return H2_PAL_OK;
    case H2_RUNTIME_SYSTEM_EVENT_MODEM_SIM_CHANGED:
        *out_schema = (h2_runtime_test_event_schema_t){
            H2_RUNTIME_COMPONENT_SYSTEM_MODEM,
            sizeof(h2_runtime_system_event_modem_sim_t),
        };
        return H2_PAL_OK;
    case H2_RUNTIME_SYSTEM_EVENT_MODEM_REGISTRATION_CHANGED:
        *out_schema = (h2_runtime_test_event_schema_t){
            H2_RUNTIME_COMPONENT_SYSTEM_MODEM,
            sizeof(h2_runtime_system_event_modem_registration_t),
        };
        return H2_PAL_OK;
    case H2_RUNTIME_SYSTEM_EVENT_MODEM_PACKET_CHANGED:
        *out_schema = (h2_runtime_test_event_schema_t){
            H2_RUNTIME_COMPONENT_SYSTEM_MODEM,
            sizeof(h2_runtime_system_event_modem_packet_t),
        };
        return H2_PAL_OK;
    case H2_RUNTIME_SYSTEM_EVENT_MODEM_SIGNAL_CHANGED:
        *out_schema = (h2_runtime_test_event_schema_t){
            H2_RUNTIME_COMPONENT_SYSTEM_MODEM,
            sizeof(h2_runtime_system_event_modem_signal_t),
        };
        return H2_PAL_OK;
    case H2_RUNTIME_SYSTEM_EVENT_MODEM_DATA_OPENED:
    case H2_RUNTIME_SYSTEM_EVENT_MODEM_DATA_CLOSED:
        *out_schema = (h2_runtime_test_event_schema_t){
            H2_RUNTIME_COMPONENT_SYSTEM_MODEM,
            sizeof(h2_runtime_system_event_modem_data_t),
        };
        return H2_PAL_OK;
    case H2_RUNTIME_SYSTEM_EVENT_MODEM_CALL_INCOMING:
    case H2_RUNTIME_SYSTEM_EVENT_MODEM_CALL_STATE_CHANGED:
    case H2_RUNTIME_SYSTEM_EVENT_MODEM_CALL_ENDED:
        *out_schema = (h2_runtime_test_event_schema_t){
            H2_RUNTIME_COMPONENT_SYSTEM_MODEM,
            sizeof(h2_runtime_system_event_modem_call_t),
        };
        return H2_PAL_OK;
    case H2_RUNTIME_COMPONENT_EVENT_BUTTON_DOWN:
        *out_schema = (h2_runtime_test_event_schema_t){
            H2_RUNTIME_COMPONENT_BUTTON,
            sizeof(h2_runtime_button_down_event_t),
        };
        return H2_PAL_OK;
    case H2_RUNTIME_COMPONENT_EVENT_BUTTON_UP:
        *out_schema = (h2_runtime_test_event_schema_t){
            H2_RUNTIME_COMPONENT_BUTTON,
            sizeof(h2_runtime_button_up_event_t),
        };
        return H2_PAL_OK;
    case H2_RUNTIME_COMPONENT_EVENT_BUTTON_ACTION:
        *out_schema = (h2_runtime_test_event_schema_t){
            H2_RUNTIME_COMPONENT_BUTTON,
            sizeof(h2_runtime_button_action_event_t),
        };
        return H2_PAL_OK;
    case H2_RUNTIME_COMPONENT_EVENT_NFC_STATE:
        *out_schema = (h2_runtime_test_event_schema_t){
            H2_RUNTIME_COMPONENT_NFC_READER,
            sizeof(h2_runtime_nfc_state_t),
        };
        return H2_PAL_OK;
    case H2_RUNTIME_COMPONENT_EVENT_IMU_GESTURE:
        *out_schema = (h2_runtime_test_event_schema_t){
            H2_RUNTIME_COMPONENT_IMU,
            sizeof(h2_runtime_imu_gesture_event_t),
        };
        return H2_PAL_OK;
    case H2_RUNTIME_COMPONENT_EVENT_ERROR:
        *out_schema = (h2_runtime_test_event_schema_t){
            H2_RUNTIME_COMPONENT_NONE,
            sizeof(h2_pal_result_t),
        };
        return H2_PAL_OK;
    case H2_RUNTIME_EVENT_NONE:
    default:
        return H2_PAL_ERR_INVALID_ARG;
    }
}

static h2_runtime_t *active_runtime(h2_runtime_test_control_t *control) {
    if (control == NULL || control->active == 0 ||
        !h2_runtime_ready(control->runtime) ||
        control->runtime->private_state->test_control != control) {
        return NULL;
    }
    return control->runtime;
}

static h2_pal_result_t set_button_state_and_emit(
    h2_runtime_test_control_t *control,
    h2_runtime_component_id_t component_id,
    const h2_runtime_button_state_t *state,
    h2_runtime_event_kind_t kind,
    h2_runtime_timestamp_ms_t timestamp_ms,
    const void *payload,
    size_t payload_size);

static h2_pal_result_t validate_event(
    h2_runtime_event_kind_t kind,
    h2_runtime_component_t component,
    h2_runtime_component_id_t component_id,
    const void *payload,
    size_t payload_size) {
    h2_runtime_test_event_schema_t schema;
    h2_pal_result_t rc = event_schema(kind, &schema);
    if (rc != H2_PAL_OK || payload_size != schema.payload_size ||
        (payload_size != 0u && payload == NULL) ||
        (schema.component != H2_RUNTIME_COMPONENT_NONE &&
         component != schema.component) ||
        (schema.component == H2_RUNTIME_COMPONENT_NONE &&
         component == H2_RUNTIME_COMPONENT_NONE) ||
        ((component == H2_RUNTIME_COMPONENT_BUTTON ||
          component == H2_RUNTIME_COMPONENT_NFC_READER ||
          component == H2_RUNTIME_COMPONENT_IMU) &&
         component_id == H2_RUNTIME_COMPONENT_ID_NONE)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (kind == H2_RUNTIME_COMPONENT_EVENT_BUTTON_ACTION) {
        const h2_runtime_button_action_event_t *event = payload;
        if (event->click_count == 0u ||
            event->released_at_ms < event->pressed_at_ms) {
            return H2_PAL_ERR_FORMAT;
        }
    } else if (kind == H2_RUNTIME_COMPONENT_EVENT_BUTTON_UP) {
        const h2_runtime_button_up_event_t *event = payload;
        if (event->released_at_ms < event->pressed_at_ms) {
            return H2_PAL_ERR_FORMAT;
        }
    } else if (kind == H2_RUNTIME_COMPONENT_EVENT_NFC_STATE) {
        const h2_runtime_nfc_state_t *state = payload;
        if (state->status < H2_RUNTIME_NFC_STATE_NONE ||
            state->status > H2_RUNTIME_NFC_STATE_ERROR) {
            return H2_PAL_ERR_FORMAT;
        }
    }
    return H2_PAL_OK;
}

h2_pal_result_t h2_runtime_test_control_open(
    h2_runtime_t *runtime,
    h2_runtime_test_control_t **out_control) {
    if (out_control == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_control = NULL;
    if (!h2_runtime_ready(runtime)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    h2_runtime_test_control_t *control =
        h2_pal_mem_alloc(runtime->mem, sizeof(*control));
    if (control == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    *control = (h2_runtime_test_control_t){
        .runtime = runtime,
        .active = 1,
    };
    h2_pal_result_t rc =
        h2_runtime_input_test_session_open(runtime, control);
    if (rc != H2_PAL_OK) {
        h2_pal_mem_free(runtime->mem, control);
        return rc;
    }
    *out_control = control;
    return H2_PAL_OK;
}

h2_pal_result_t h2_runtime_test_emit_event(
    h2_runtime_test_control_t *control,
    h2_runtime_event_kind_t kind,
    h2_runtime_component_t component,
    h2_runtime_component_id_t component_id,
    h2_runtime_timestamp_ms_t timestamp_ms,
    const void *payload,
    size_t payload_size) {
    h2_runtime_t *runtime = active_runtime(control);
    if (runtime == NULL) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    h2_pal_result_t rc = validate_event(
        kind, component, component_id, payload, payload_size);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    const h2_runtime_sequence_t sequence = h2_runtime_next_sequence(runtime);
    if (sequence == 0u) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    return h2_runtime_emit_event(
        runtime,
        kind,
        component,
        component_id,
        sequence,
        timestamp_ms,
        payload,
        payload_size);
}

h2_pal_result_t h2_runtime_test_poll_sensors(h2_runtime_t *runtime) {
    return h2_runtime_input_poll_sensors_once(runtime);
}

static h2_runtime_input_source_t *find_source_by_id(
    h2_runtime_t *runtime,
    h2_runtime_component_id_t component_id) {
    for (size_t index = 0u;
         index < runtime->private_state->input_source_count;
         ++index) {
        h2_runtime_input_source_t *source =
            &runtime->private_state->input_sources[index];
        if (source->component_id == component_id) {
            return source;
        }
    }
    return NULL;
}

static const h2_runtime_component_mapping_t *find_mapping_by_id(
    const h2_runtime_t *runtime,
    h2_runtime_component_id_t component_id) {
    for (size_t index = 0u;
         index < runtime->private_state->component_mapping_count;
         ++index) {
        const h2_runtime_component_mapping_t *mapping =
            &runtime->private_state->component_mappings[index];
        if (mapping->component_id == component_id) {
            return mapping;
        }
    }
    return NULL;
}

static h2_runtime_input_source_t *find_or_create_input_source(
    h2_runtime_t *runtime,
    h2_runtime_component_id_t component_id) {
    h2_runtime_input_source_t *source =
        find_source_by_id(runtime, component_id);
    if (source != NULL) {
        return source;
    }
    if (runtime->private_state->input_source_count >=
        runtime->private_state->input_source_capacity) {
        return NULL;
    }
    const h2_runtime_component_mapping_t *mapping =
        find_mapping_by_id(runtime, component_id);
    if (mapping == NULL ||
        (mapping->component != H2_RUNTIME_COMPONENT_BUTTON &&
         mapping->component != H2_RUNTIME_COMPONENT_NFC_READER &&
         mapping->component != H2_RUNTIME_COMPONENT_IMU)) {
        return NULL;
    }
    source = &runtime->private_state->input_sources[
        runtime->private_state->input_source_count++];
    memset(source, 0, sizeof(*source));
    source->kind =
        mapping->component == H2_RUNTIME_COMPONENT_BUTTON
            ? H2_RUNTIME_INPUT_SOURCE_SINGLE_BUTTON
            : (mapping->component == H2_RUNTIME_COMPONENT_NFC_READER
                   ? H2_RUNTIME_INPUT_SOURCE_NFC_READER
                   : H2_RUNTIME_INPUT_SOURCE_IMU);
    source->component = mapping->component;
    source->component_id = component_id;
    source->periph_id = mapping->periph_id;
    return source;
}

h2_pal_result_t h2_runtime_test_set_component_state(
    h2_runtime_test_control_t *control,
    h2_runtime_component_id_t component_id,
    const void *state,
    size_t state_size) {
    h2_runtime_t *runtime = active_runtime(control);
    if (runtime == NULL) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    if (component_id == H2_RUNTIME_COMPONENT_ID_NONE || state == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    const size_t prior_count =
        runtime->private_state->input_source_count;
    h2_runtime_input_source_t *source =
        find_or_create_input_source(runtime, component_id);
    if (source == NULL) {
        return H2_PAL_ERR_NOT_FOUND;
    }
    const h2_runtime_input_source_t prior_source = *source;
    if (source->component == H2_RUNTIME_COMPONENT_BUTTON &&
        state_size == sizeof(source->button_state)) {
        const h2_runtime_button_state_t *button_state = state;
        if ((!button_state->pressed && button_state->pressed_at_ms != 0u) ||
            (button_state->pressed &&
             button_state->pressed_at_ms > button_state->updated_at_ms)) {
            *source = prior_source;
            runtime->private_state->input_source_count = prior_count;
            return H2_PAL_ERR_FORMAT;
        }
        source->button_state = *button_state;
        source->timestamp_ms = button_state->updated_at_ms;
    } else if (source->component == H2_RUNTIME_COMPONENT_NFC_READER &&
               state_size == sizeof(source->nfc_state)) {
        const h2_runtime_nfc_state_t *nfc_state = state;
        if (nfc_state->status < H2_RUNTIME_NFC_STATE_NONE ||
            nfc_state->status > H2_RUNTIME_NFC_STATE_ERROR) {
            *source = prior_source;
            runtime->private_state->input_source_count = prior_count;
            return H2_PAL_ERR_FORMAT;
        }
        source->nfc_state = *nfc_state;
        source->timestamp_ms = nfc_state->updated_at_ms;
    } else if (source->component == H2_RUNTIME_COMPONENT_IMU &&
               state_size == sizeof(source->imu_state)) {
        source->imu_state = *(const h2_runtime_imu_state_t *)state;
        source->timestamp_ms =
            ((const h2_runtime_imu_state_t *)state)->updated_at_ms;
    } else {
        *source = prior_source;
        runtime->private_state->input_source_count = prior_count;
        return H2_PAL_ERR_FORMAT;
    }
    h2_pal_result_t rc = h2_runtime_input_test_publish(runtime);
    if (rc != H2_PAL_OK) {
        *source = prior_source;
        runtime->private_state->input_source_count = prior_count;
    }
    return rc;
}

static h2_pal_result_t set_button_state_and_emit(
    h2_runtime_test_control_t *control,
    h2_runtime_component_id_t component_id,
    const h2_runtime_button_state_t *state,
    h2_runtime_event_kind_t kind,
    h2_runtime_timestamp_ms_t timestamp_ms,
    const void *payload,
    size_t payload_size) {
    h2_runtime_t *runtime = active_runtime(control);
    if (runtime == NULL) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    h2_pal_result_t rc = validate_event(
        kind,
        H2_RUNTIME_COMPONENT_BUTTON,
        component_id,
        payload,
        payload_size);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    const size_t prior_count =
        runtime->private_state->input_source_count;
    h2_runtime_input_source_t *source =
        find_or_create_input_source(runtime, component_id);
    if (source == NULL ||
        source->component != H2_RUNTIME_COMPONENT_BUTTON) {
        return H2_PAL_ERR_NOT_FOUND;
    }
    const h2_runtime_input_source_t prior_source = *source;
    const h2_runtime_sequence_t prior_ceiling =
        runtime->private_state->input_event_sequence_ceiling;
    const h2_runtime_sequence_t sequence = h2_runtime_next_sequence(runtime);
    if (sequence == 0u) {
        *source = prior_source;
        runtime->private_state->input_source_count = prior_count;
        return H2_PAL_ERR_INVALID_STATE;
    }
    source->button_state = *state;
    source->sequence = sequence;
    source->timestamp_ms = timestamp_ms;
    if (sequence > runtime->private_state->input_event_sequence_ceiling) {
        runtime->private_state->input_event_sequence_ceiling = sequence;
    }
    rc = h2_runtime_input_test_publish(runtime);
    if (rc != H2_PAL_OK) {
        *source = prior_source;
        runtime->private_state->input_source_count = prior_count;
        runtime->private_state->input_event_sequence_ceiling =
            prior_ceiling;
        return rc;
    }
    rc = h2_runtime_emit_event(
        runtime,
        kind,
        H2_RUNTIME_COMPONENT_BUTTON,
        component_id,
        sequence,
        timestamp_ms,
        payload,
        payload_size);
    if (rc == H2_PAL_OK) {
        return H2_PAL_OK;
    }
    *source = prior_source;
    runtime->private_state->input_source_count = prior_count;
    runtime->private_state->input_event_sequence_ceiling =
        prior_ceiling;
    h2_pal_result_t rollback_rc =
        h2_runtime_input_test_publish(runtime);
    return rollback_rc == H2_PAL_OK ? rc : rollback_rc;
}

/* Current published click_count of a test-owned button (0 when unknown). */
static uint16_t current_click_count(
    h2_runtime_test_control_t *control,
    h2_runtime_component_id_t component_id) {
    h2_runtime_t *runtime = active_runtime(control);
    h2_runtime_button_state_t state;
    if (runtime == NULL ||
        h2_runtime_component_state_button(runtime, component_id, &state) !=
            H2_PAL_OK) {
        return 0u;
    }
    return state.click_count;
}

/*
 * click_count for a test-injected press: continue the published sequence
 * when the button was released no longer than the click gap before this
 * press (mirroring the production recognizer), otherwise start at 1.
 */
static uint16_t next_press_click_count(
    h2_runtime_test_control_t *control,
    h2_runtime_component_id_t component_id,
    h2_runtime_timestamp_ms_t pressed_at_ms) {
    h2_runtime_t *runtime = active_runtime(control);
    h2_runtime_button_state_t state;
    if (runtime == NULL ||
        h2_runtime_component_state_button(runtime, component_id, &state) !=
            H2_PAL_OK ||
        state.result != H2_PAL_OK || state.click_count == 0u) {
        return 1u;
    }
    if (state.pressed) {
        return state.click_count;
    }
    const h2_runtime_timestamp_ms_t released_at_ms = state.updated_at_ms;
    if (pressed_at_ms < released_at_ms ||
        pressed_at_ms - released_at_ms > H2_RUNTIME_BUTTON_CLICK_GAP_MS ||
        state.click_count == UINT16_MAX) {
        return 1u;
    }
    return (uint16_t)(state.click_count + 1u);
}

h2_pal_result_t h2_runtime_test_button_down(
    h2_runtime_test_control_t *control,
    h2_runtime_component_id_t component_id,
    h2_runtime_timestamp_ms_t pressed_at_ms) {
    const h2_runtime_button_down_event_t event = {
        .pressed_at_ms = pressed_at_ms,
    };
    const h2_runtime_button_state_t state = {
        .pressed = true,
        .pressed_at_ms = pressed_at_ms,
        .click_count =
            next_press_click_count(control, component_id, pressed_at_ms),
        .updated_at_ms = pressed_at_ms,
        .result = H2_PAL_OK,
    };
    return set_button_state_and_emit(
        control,
        component_id,
        &state,
        H2_RUNTIME_COMPONENT_EVENT_BUTTON_DOWN,
        pressed_at_ms,
        &event,
        sizeof(event));
}

h2_pal_result_t h2_runtime_test_button_up(
    h2_runtime_test_control_t *control,
    h2_runtime_component_id_t component_id,
    h2_runtime_timestamp_ms_t pressed_at_ms,
    h2_runtime_timestamp_ms_t released_at_ms) {
    if (released_at_ms < pressed_at_ms) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    const h2_runtime_button_up_event_t event = {
        .pressed_at_ms = pressed_at_ms,
        .released_at_ms = released_at_ms,
    };
    const h2_runtime_button_state_t state = {
        .pressed = false,
        .click_count = current_click_count(control, component_id),
        .updated_at_ms = released_at_ms,
        .result = H2_PAL_OK,
    };
    return set_button_state_and_emit(
        control,
        component_id,
        &state,
        H2_RUNTIME_COMPONENT_EVENT_BUTTON_UP,
        released_at_ms,
        &event,
        sizeof(event));
}

h2_pal_result_t h2_runtime_test_button_action(
    h2_runtime_test_control_t *control,
    h2_runtime_component_id_t component_id,
    h2_runtime_timestamp_ms_t pressed_at_ms,
    h2_runtime_timestamp_ms_t released_at_ms,
    uint16_t click_count) {
    if (released_at_ms < pressed_at_ms || click_count == 0u) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    const h2_runtime_button_action_event_t event = {
        .pressed_at_ms = pressed_at_ms,
        .released_at_ms = released_at_ms,
        .click_count = click_count,
    };
    const h2_runtime_button_state_t state = {
        .pressed = false,
        .click_count = click_count,
        .updated_at_ms = released_at_ms,
        .result = H2_PAL_OK,
    };
    return set_button_state_and_emit(
        control,
        component_id,
        &state,
        H2_RUNTIME_COMPONENT_EVENT_BUTTON_ACTION,
        released_at_ms,
        &event,
        sizeof(event));
}

void h2_runtime_test_control_close(h2_runtime_test_control_t *control) {
    h2_runtime_t *runtime = active_runtime(control);
    if (runtime == NULL) {
        return;
    }
    if (h2_runtime_input_test_session_close(runtime) != H2_PAL_OK) {
        return;
    }
    control->active = 0;
    control->runtime = NULL;
    h2_pal_mem_free(runtime->mem, control);
}
