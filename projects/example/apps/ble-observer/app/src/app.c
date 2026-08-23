#include "h2_smoke_ble_observer.h"

#include <stdio.h>
#include <string.h>

#define H2_SMOKE_BLE_OBSERVER_SCAN_TIMEOUT_MS 15000u
#define H2_SMOKE_BLE_OBSERVER_EVENT_TIMEOUT_MS 20000u
#define H2_SMOKE_BLE_OBSERVER_RAW_CAPACITY 256u

typedef struct h2_smoke_ble_observer_state {
    int fragment_active;
    unsigned total_reports;
    unsigned extended_reports;
    unsigned matching_shape_reports;
    unsigned matching_payload_reports;
    unsigned truncated_reports;
    unsigned oversized_reports;
    unsigned other_payload_reports;
    h2_pal_ble_addr_t addr;
    uint8_t sid;
    h2_pal_ble_adv_type_t last_adv_type;
    h2_pal_ble_phy_t last_primary_phy;
    h2_pal_ble_phy_t last_secondary_phy;
    uint8_t last_sid;
    h2_pal_ble_adv_data_status_t last_data_status;
    size_t last_raw_len;
    size_t last_manufacturer_len;
    uint8_t raw[H2_SMOKE_BLE_OBSERVER_RAW_CAPACITY];
    size_t raw_len;
} h2_smoke_ble_observer_state_t;

static void h2_smoke_ble_observer_log(
    h2_runtime_t *runtime,
    h2_pal_log_level_t level,
    const char *message) {
    (void)h2_pal_log_write(runtime->log, level, "ble-observer", message);
}

static int h2_smoke_ble_observer_same_advertiser(
    const h2_smoke_ble_observer_state_t *state,
    const h2_pal_ble_scan_result_t *result) {
    return state->fragment_active && state->sid == result->sid &&
           state->addr.type == result->addr.type &&
           memcmp(state->addr.value, result->addr.value, sizeof(state->addr.value)) == 0;
}

static int h2_smoke_ble_observer_append(
    h2_smoke_ble_observer_state_t *state,
    const h2_pal_ble_scan_result_t *result) {
    if (result->raw_data.data == NULL || result->raw_data.len == 0u ||
        result->raw_data.len > sizeof(state->raw) - state->raw_len) {
        return 0;
    }
    memcpy(state->raw + state->raw_len, result->raw_data.data, result->raw_data.len);
    state->raw_len += result->raw_data.len;
    return 1;
}

static int h2_smoke_ble_observer_payload_matches(
    const uint8_t *data,
    size_t len) {
    size_t offset = 0u;
    if (data == NULL || len <= H2_PAL_BLE_LEGACY_ADV_DATA_MAX_LEN) {
        return 0;
    }
    while (offset < len) {
        size_t field_len = data[offset];
        if (field_len == 0u) {
            break;
        }
        if (field_len > len - offset - 1u) {
            return 0;
        }
        if (data[offset + 1u] == 0xffu && field_len == 65u) {
            for (size_t i = 0u; i < 64u; ++i) {
                if (data[offset + 2u + i] != (uint8_t)i) {
                    return 0;
                }
            }
            return 1;
        }
        offset += field_len + 1u;
    }
    return 0;
}

static bool h2_smoke_ble_observer_on_result(
    void *user,
    const h2_pal_ble_scan_result_t *result) {
    h2_smoke_ble_observer_state_t *state = user;
    ++state->total_reports;
    state->last_adv_type = result->adv_type;
    state->last_primary_phy = result->primary_phy;
    state->last_secondary_phy = result->secondary_phy;
    state->last_sid = result->sid;
    state->last_data_status = result->data_status;
    state->last_raw_len = result->raw_data.len;
    state->last_manufacturer_len = result->manufacturer_data.len;
    if (result->adv_type == H2_PAL_BLE_ADV_TYPE_EXTENDED) {
        ++state->extended_reports;
    }
    if (result->adv_type != H2_PAL_BLE_ADV_TYPE_EXTENDED ||
        result->primary_phy != H2_PAL_BLE_PHY_CODED ||
        result->secondary_phy != H2_PAL_BLE_PHY_2M || result->sid != 7u) {
        return false;
    }
    ++state->matching_shape_reports;
    if (result->data_status == H2_PAL_BLE_ADV_DATA_TRUNCATED) {
        ++state->truncated_reports;
        state->fragment_active = 0;
        state->raw_len = 0u;
        return false;
    }
    if (!h2_smoke_ble_observer_same_advertiser(state, result)) {
        state->fragment_active = 1;
        state->addr = result->addr;
        state->sid = result->sid;
        state->raw_len = 0u;
    }
    if (!h2_smoke_ble_observer_append(state, result)) {
        ++state->oversized_reports;
        state->fragment_active = 0;
        state->raw_len = 0u;
        return false;
    }
    if (result->data_status == H2_PAL_BLE_ADV_DATA_INCOMPLETE) {
        return false;
    }
    if (result->data_status == H2_PAL_BLE_ADV_DATA_COMPLETE &&
        h2_smoke_ble_observer_payload_matches(state->raw, state->raw_len)) {
        ++state->matching_payload_reports;
    } else {
        ++state->other_payload_reports;
    }
    state->fragment_active = 0;
    state->raw_len = 0u;
    return false;
}

int h2_smoke_ble_observer_run(h2_runtime_t *runtime) {
    if (runtime == NULL || runtime->ble_host == NULL || runtime->log == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    h2_pal_result_t rc = h2_pal_ble_start(runtime->ble_host);
    if (rc != H2_PAL_OK) {
        h2_smoke_ble_observer_log(
            runtime, H2_PAL_LOG_ERROR, "stage=host state=error");
        return rc;
    }

    h2_smoke_ble_observer_state_t state = { 0 };
    h2_pal_ble_scan_params_t params = {
        .mode = H2_PAL_BLE_SCAN_MODE_PASSIVE,
        .interval_ms = 100u,
        .window_ms = 100u,
        .timeout_ms = H2_SMOKE_BLE_OBSERVER_SCAN_TIMEOUT_MS,
        .type = H2_PAL_BLE_SCAN_TYPE_EXTENDED,
        .phy_mask = H2_PAL_BLE_SCAN_PHY_ALL,
    };
    int scan_started = 0;
    int scan_stopped = 0;
    rc = h2_pal_ble_start_scan(
        runtime->ble_host, &params, h2_smoke_ble_observer_on_result, &state);
    if (rc == H2_PAL_OK) {
        scan_started = 1;
        uint8_t payload[H2_RUNTIME_EVENT_PAYLOAD_MAX];
        h2_runtime_event_t event = {
            .payload = payload,
            .payload_capacity = sizeof(payload),
        };
        do {
            rc = h2_runtime_wait_event(
                runtime, &event, H2_SMOKE_BLE_OBSERVER_EVENT_TIMEOUT_MS);
        } while (rc == H2_PAL_OK &&
                 event.kind != H2_RUNTIME_SYSTEM_EVENT_BLE_SCAN_STOPPED);
        scan_stopped = rc == H2_PAL_OK;
    }
    if (scan_started && !scan_stopped) {
        (void)h2_pal_ble_stop_scan(runtime->ble_host);
    }
    char summary[256];
    (void)snprintf(
        summary,
        sizeof(summary),
        "stage=reports total=%u extended=%u shape=%u payload=%u truncated=%u "
        "oversized=%u other_payload=%u last_type=%u primary=%u secondary=%u "
        "sid=%u status=%u raw_len=%u mfg_len=%u raw_accum=%u",
        state.total_reports,
        state.extended_reports,
        state.matching_shape_reports,
        state.matching_payload_reports,
        state.truncated_reports,
        state.oversized_reports,
        state.other_payload_reports,
        (unsigned)state.last_adv_type,
        (unsigned)state.last_primary_phy,
        (unsigned)state.last_secondary_phy,
        (unsigned)state.last_sid,
        (unsigned)state.last_data_status,
        (unsigned)state.last_raw_len,
        (unsigned)state.last_manufacturer_len,
        (unsigned)state.raw_len);
    h2_smoke_ble_observer_log(runtime, H2_PAL_LOG_INFO, summary);
    (void)snprintf(
        summary,
        sizeof(summary),
        "stage=extended_scan state=%s rc=%d",
        rc == H2_PAL_OK ? "complete" : "error",
        rc);
    h2_smoke_ble_observer_log(
        runtime,
        rc == H2_PAL_OK ? H2_PAL_LOG_INFO : H2_PAL_LOG_ERROR,
        summary);
    return rc;
}
