#include "h2_smoke_ble_advertising.h"

#include <string.h>

#define H2_SMOKE_BLE_EVENT_TIMEOUT_MS 10000u

static void h2_smoke_ble_log(h2_runtime_t *runtime, h2_pal_log_level_t level, const char *message) {
    if (runtime->log != NULL) {
        (void)h2_pal_log_write(runtime->log, level, "ble-broadcaster", message);
    }
}

static h2_pal_result_t h2_smoke_ble_wait(
    h2_runtime_t *runtime,
    h2_runtime_event_kind_t expected) {
    uint8_t payload[H2_RUNTIME_EVENT_PAYLOAD_MAX];
    h2_runtime_event_t event = {
        .payload = payload,
        .payload_capacity = sizeof(payload),
    };
    for (;;) {
        h2_pal_result_t rc = h2_runtime_wait_event(runtime, &event, H2_SMOKE_BLE_EVENT_TIMEOUT_MS);
        if (rc != H2_PAL_OK || event.kind == expected) {
            return rc;
        }
    }
}

static h2_pal_result_t h2_smoke_ble_run_case(
    h2_runtime_t *runtime,
    const h2_pal_ble_adv_data_t *data,
    const h2_pal_ble_adv_params_t *params,
    int explicit_stop,
    const char *pass_message,
    const char *fail_message) {
    h2_pal_result_t rc = h2_pal_ble_set_adv_data(runtime->ble_host, data);
    if (rc == H2_PAL_OK) {
        rc = h2_pal_ble_start_advertising(runtime->ble_host, params);
    }
    if (rc == H2_PAL_OK) {
        rc = h2_smoke_ble_wait(runtime, H2_RUNTIME_SYSTEM_EVENT_BLE_ADVERTISING_STARTED);
    }
    if (rc == H2_PAL_OK && explicit_stop) {
        rc = h2_pal_ble_stop_advertising(runtime->ble_host);
    }
    if (rc == H2_PAL_OK) {
        rc = h2_smoke_ble_wait(runtime, H2_RUNTIME_SYSTEM_EVENT_BLE_ADVERTISING_STOPPED);
    }
    h2_smoke_ble_log(runtime, rc == H2_PAL_OK ? H2_PAL_LOG_INFO : H2_PAL_LOG_ERROR,
        rc == H2_PAL_OK ? pass_message : fail_message);
    return rc;
}

static h2_pal_ble_adv_set_t *s_product_beacon;

static h2_pal_result_t h2_smoke_ble_start_product_beacon(
    h2_runtime_t *runtime,
    const h2_pal_ble_adv_data_t *data,
    const h2_pal_ble_adv_data_t *updated_data,
    const h2_pal_ble_adv_params_t *params) {
    h2_pal_result_t rc = h2_pal_ble_adv_set_create(
        runtime->ble_host, params, &s_product_beacon);
    if (rc == H2_PAL_OK) {
        rc = h2_pal_ble_adv_set_set_data(
            runtime->ble_host, s_product_beacon, data);
    }
    if (rc == H2_PAL_OK) {
        rc = h2_pal_ble_adv_set_start(runtime->ble_host, s_product_beacon);
    }
    if (rc == H2_PAL_OK) {
        rc = h2_pal_ble_adv_set_set_data(
            runtime->ble_host, s_product_beacon, updated_data);
    }
    if (rc == H2_PAL_OK) {
        rc = h2_pal_ble_adv_set_stop(runtime->ble_host, s_product_beacon);
    }
    if (rc == H2_PAL_OK) {
        rc = h2_pal_ble_adv_set_start(runtime->ble_host, s_product_beacon);
    }
    if (rc != H2_PAL_OK && s_product_beacon != NULL) {
        (void)h2_pal_ble_adv_set_destroy(
            runtime->ble_host, s_product_beacon);
        s_product_beacon = NULL;
    }
    if (rc == H2_PAL_OK) {
        rc = h2_pal_ble_adv_set_stop(runtime->ble_host, s_product_beacon);
    }
    if (rc == H2_PAL_OK) {
        rc = h2_pal_ble_adv_set_destroy(runtime->ble_host, s_product_beacon);
        s_product_beacon = NULL;
    }
    if (rc == H2_PAL_OK) {
        rc = h2_pal_ble_adv_set_create(
            runtime->ble_host, params, &s_product_beacon);
    }
    if (rc == H2_PAL_OK) {
        rc = h2_pal_ble_adv_set_set_data(
            runtime->ble_host, s_product_beacon, data);
    }
    if (rc == H2_PAL_OK) {
        rc = h2_pal_ble_adv_set_start(runtime->ble_host, s_product_beacon);
    }
    if (rc != H2_PAL_OK && s_product_beacon != NULL) {
        (void)h2_pal_ble_adv_set_destroy(
            runtime->ble_host, s_product_beacon);
        s_product_beacon = NULL;
    }
    h2_smoke_ble_log(runtime, rc == H2_PAL_OK ? H2_PAL_LOG_INFO : H2_PAL_LOG_ERROR,
        rc == H2_PAL_OK ? "stage=product_beacon state=complete rc=0"
                        : "stage=product_beacon state=error");
    return rc;
}

int h2_smoke_ble_advertising_run(h2_runtime_t *runtime) {
    if (runtime == NULL || runtime->ble_host == NULL || runtime->log == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }

    h2_pal_result_t rc = h2_pal_ble_start(runtime->ble_host);
    if (rc != H2_PAL_OK) {
        h2_smoke_ble_log(runtime, H2_PAL_LOG_ERROR,
            "stage=host state=error");
        return rc;
    }

    static const uint8_t legacy_manufacturer[] = { 0x48u, 0x32u, 0x00u, 0x01u };
    h2_pal_ble_adv_data_t legacy_data = {
        .local_name = "h2-legacy",
        .manufacturer_data = {
            .data = legacy_manufacturer,
            .len = sizeof(legacy_manufacturer),
        },
    };
    h2_pal_ble_adv_params_t legacy_params = {
        .mode = H2_PAL_BLE_ADV_MODE_CONNECTABLE,
        .interval_min_ms = 100u,
        .interval_max_ms = 120u,
    };
    rc = h2_smoke_ble_run_case(
        runtime,
        &legacy_data,
        &legacy_params,
        1,
        "stage=legacy state=complete rc=0",
        "stage=legacy state=error");
    if (rc != H2_PAL_OK) {
        (void)h2_pal_ble_stop(runtime->ble_host);
        return rc;
    }

    uint8_t extended_manufacturer[64];
    for (size_t i = 0u; i < sizeof(extended_manufacturer); ++i) {
        extended_manufacturer[i] = (uint8_t)i;
    }
    static const uint8_t service_uuid16[] = { 0x32u, 0x48u };
    h2_pal_ble_uuid_t service_uuid = {
        .data = service_uuid16,
        .len = sizeof(service_uuid16),
    };
    h2_pal_ble_adv_data_t extended_data = {
        .local_name = "h2-extended-advertising",
        .service_uuids = &service_uuid,
        .service_uuid_count = 1u,
        .manufacturer_data = {
            .data = extended_manufacturer,
            .len = sizeof(extended_manufacturer),
        },
    };
    h2_pal_ble_adv_params_t extended_params = {
        .mode = H2_PAL_BLE_ADV_MODE_NON_CONNECTABLE,
        .interval_min_ms = 100u,
        .interval_max_ms = 120u,
        .type = H2_PAL_BLE_ADV_TYPE_EXTENDED,
        .primary_phy = H2_PAL_BLE_PHY_CODED,
        .secondary_phy = H2_PAL_BLE_PHY_2M,
        .sid = 7u,
    };
    rc = h2_smoke_ble_run_case(
        runtime,
        &extended_data,
        &extended_params,
        1,
        "stage=extended state=complete rc=0",
        "stage=extended state=error");
    if (rc == H2_PAL_OK) {
        extended_params.duration_ms = 500u;
        rc = h2_smoke_ble_run_case(
            runtime,
            &extended_data,
            &extended_params,
            0,
            "stage=duration state=complete rc=0",
            "stage=duration state=error");
    }
    if (rc == H2_PAL_OK) {
        extended_params.duration_ms = 0u;
        extended_params.max_adv_events = 3u;
        rc = h2_smoke_ble_run_case(
            runtime,
            &extended_data,
            &extended_params,
            0,
            "stage=max_events state=complete rc=0",
            "stage=max_events state=error");
    }
    if (rc == H2_PAL_OK) {
        extended_params.max_adv_events = 0u;
        extended_manufacturer[0] = 0xa5u;
        uint8_t updated_manufacturer[sizeof(extended_manufacturer)];
        memcpy(updated_manufacturer, extended_manufacturer, sizeof(updated_manufacturer));
        updated_manufacturer[0] = 0x5au;
        h2_pal_ble_adv_data_t updated_data = extended_data;
        updated_data.manufacturer_data.data = updated_manufacturer;
        rc = h2_smoke_ble_start_product_beacon(
            runtime, &extended_data, &updated_data, &extended_params);
    }
    if (rc != H2_PAL_OK) {
        (void)h2_pal_ble_stop(runtime->ble_host);
    }
    h2_smoke_ble_log(runtime, rc == H2_PAL_OK ? H2_PAL_LOG_INFO : H2_PAL_LOG_ERROR,
        rc == H2_PAL_OK ? "stage=diagnostic state=complete rc=0"
                        : "stage=diagnostic state=error");
    return rc;
}
