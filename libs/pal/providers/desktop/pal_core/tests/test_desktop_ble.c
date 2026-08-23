#include "h2_desktop_platform.h"
#include "h2_desktop_test_system_event.h"

#include <assert.h>
#include <string.h>

typedef struct event_counts {
    unsigned started;
    unsigned stopped;
    h2_pal_ble_adv_set_t *last_started_set;
    h2_pal_ble_adv_set_t *last_stopped_set;
} event_counts_t;

typedef struct scan_capture {
    unsigned count;
    h2_pal_ble_scan_result_t result;
} scan_capture_t;

static const uint8_t k_secure_value[] = { 0x73u, 0x65u, 0x63u,
                                         0x72u, 0x65u, 0x74u };

static h2_pal_result_t read_secure_value(
    void *user, const h2_pal_ble_gatt_access_t *access, uint8_t *out,
    size_t out_size, size_t *out_len) {
    assert(user == (void *)k_secure_value);
    assert(access != NULL && out_len != NULL);
    *out_len = 0u;
    if (access->offset >= sizeof(k_secure_value))
        return H2_PAL_OK;
    size_t len = sizeof(k_secure_value) - access->offset;
    if (len > out_size)
        len = out_size;
    memcpy(out, k_secure_value + access->offset, len);
    *out_len = len;
    return H2_PAL_OK;
}

static bool capture_scan(void *user, const h2_pal_ble_scan_result_t *result) {
    scan_capture_t *capture = user;
    ++capture->count;
    capture->result = *result;
    assert(result->raw_data.data != NULL);
    assert(result->raw_data.len > H2_PAL_BLE_LEGACY_ADV_DATA_MAX_LEN);
    return false;
}

static int count_event(void *user, const h2_pal_system_event_t *event) {
    event_counts_t *counts = user;
    h2_pal_ble_adv_set_t *set = NULL;
    if (event->payload != NULL) {
        assert(event->payload_size == sizeof(h2_pal_ble_adv_set_event_t));
        const h2_pal_ble_adv_set_event_t *adv_event = event->payload;
        assert(adv_event->status == H2_PAL_OK);
        set = adv_event->set;
    } else {
        assert(event->payload_size == 0u);
    }
    if (event->type == H2_PAL_SYSTEM_EVENT_TYPE_BLE_ADVERTISING_STARTED) {
        ++counts->started;
        counts->last_started_set = set;
    } else if (event->type == H2_PAL_SYSTEM_EVENT_TYPE_BLE_ADVERTISING_STOPPED) {
        ++counts->stopped;
        counts->last_stopped_set = set;
    }
    return H2_PAL_OK;
}

int main(void) {
    const h2_pal_system_event_api_t *events =
        h2_desktop_test_system_event_api();
    const h2_pal_system_event_vtable_t incomplete_vtable = {0};
    const h2_pal_system_event_api_t incomplete_events = {
        .user = NULL,
        .vtable = &incomplete_vtable,
    };
    assert(h2_desktop_platform_ble(NULL) == NULL);
    assert(h2_desktop_platform_ble(&incomplete_events) == NULL);
    h2_pal_ble_t *ble = h2_desktop_platform_ble(events);
    assert(ble != NULL);
    assert(h2_desktop_platform_ble(events) == ble);
    event_counts_t counts = { 0 };
    h2_pal_system_event_subscription_t *started_sub = NULL;
    h2_pal_system_event_subscription_t *stopped_sub = NULL;
    assert(h2_desktop_test_system_event_init() == H2_PAL_OK);
    assert(h2_pal_system_event_subscribe(events, H2_PAL_SYSTEM_EVENT_TYPE_BLE_ADVERTISING_STARTED,
               count_event, &counts, &started_sub) == H2_PAL_OK);
    assert(h2_pal_system_event_subscribe(events, H2_PAL_SYSTEM_EVENT_TYPE_BLE_ADVERTISING_STOPPED,
               count_event, &counts, &stopped_sub) == H2_PAL_OK);
    assert(h2_pal_ble_start(ble) == H2_PAL_OK);

    const uint8_t tracked_value = 0x5au;
    assert(h2_pal_ble_indicate(
               ble, 1u, 1u, &tracked_value, sizeof(tracked_value), 100u) ==
           H2_PAL_ERR_UNSUPPORTED);
    assert(h2_pal_ble_indicate(
               ble, 1u, 1u, &tracked_value, sizeof(tracked_value),
               0u) == H2_PAL_ERR_UNSUPPORTED);

    uint8_t manufacturer[14];
    memset(manufacturer, 0x5a, sizeof(manufacturer));
    h2_pal_ble_adv_data_t data = {
        .local_name = "1234567890",
        .manufacturer_data = { .data = manufacturer, .len = sizeof(manufacturer) },
    };
    assert(h2_pal_ble_set_adv_data(ble, &data) == H2_PAL_OK);
    memset(manufacturer, 0xff, sizeof(manufacturer));
    uint8_t staged[H2_PAL_BLE_EXT_ADV_DATA_MAX_LEN];
    size_t staged_len = 0u;
    assert(h2_desktop_platform_copy_ble_staged_adv_data(staged, sizeof(staged), &staged_len) == H2_PAL_OK);
    assert(staged_len == H2_PAL_BLE_LEGACY_ADV_DATA_MAX_LEN);
    for (size_t i = 5u; i < 5u + sizeof(manufacturer); ++i) {
        assert(staged[i] == 0x5au);
    }

    h2_pal_ble_adv_params_t legacy = {
        .mode = H2_PAL_BLE_ADV_MODE_NON_CONNECTABLE,
        .interval_min_ms = 100u,
        .interval_max_ms = 120u,
    };
    assert(h2_pal_ble_start_advertising(ble, &legacy) == H2_PAL_OK);
    assert(counts.started == 1u && counts.stopped == 0u);
    assert(h2_pal_ble_stop_advertising(ble) == H2_PAL_OK);
    assert(counts.stopped == 1u);

    uint8_t uuid16_a[] = { 0x0du, 0x18u };
    uint8_t uuid16_b[] = { 0x0fu, 0x18u };
    uint8_t uuid32[] = { 0x44u, 0x33u, 0x22u, 0x11u };
    h2_pal_ble_uuid_t service_uuids[] = {
        { .data = uuid16_a, .len = sizeof(uuid16_a) },
        { .data = uuid32, .len = sizeof(uuid32) },
        { .data = uuid16_b, .len = sizeof(uuid16_b) },
    };
    h2_pal_ble_adv_data_t uuid_data = {
        .service_uuids = service_uuids,
        .service_uuid_count = sizeof(service_uuids) / sizeof(service_uuids[0]),
    };
    const uint8_t expected_uuid_data[] = {
        0x02u, 0x01u, 0x06u,
        0x05u, 0x03u, 0x0du, 0x18u, 0x0fu, 0x18u,
        0x05u, 0x05u, 0x44u, 0x33u, 0x22u, 0x11u,
    };
    assert(h2_pal_ble_set_adv_data(ble, &uuid_data) == H2_PAL_OK);
    assert(h2_desktop_platform_copy_ble_staged_adv_data(
               staged, sizeof(staged), &staged_len) == H2_PAL_OK);
    assert(staged_len == sizeof(expected_uuid_data));
    assert(memcmp(staged, expected_uuid_data, sizeof(expected_uuid_data)) == 0);

    uint8_t uuid128[] = {
        0x1du, 0x72u, 0xa1u, 0x6bu, 0x3au, 0xafu, 0x0bu, 0xaau,
        0xe2u, 0x53u, 0xd8u, 0x3eu, 0x70u, 0xb5u, 0xa4u, 0x71u,
    };
    uint8_t service_payload[] = { 0x01u, 0x02u, 0x03u, 0x04u };
    h2_pal_ble_adv_data_t service_data128 = {
        .service_data_uuid = { .data = uuid128, .len = sizeof(uuid128) },
        .service_data = { .data = service_payload, .len = sizeof(service_payload) },
    };
    const uint8_t expected_service_data128[] = {
        0x02u, 0x01u, 0x06u,
        0x15u, 0x21u,
        0x1du, 0x72u, 0xa1u, 0x6bu, 0x3au, 0xafu, 0x0bu, 0xaau,
        0xe2u, 0x53u, 0xd8u, 0x3eu, 0x70u, 0xb5u, 0xa4u, 0x71u,
        0x01u, 0x02u, 0x03u, 0x04u,
    };
    assert(h2_pal_ble_set_adv_data(ble, &service_data128) == H2_PAL_OK);
    assert(h2_desktop_platform_copy_ble_staged_adv_data(
               staged, sizeof(staged), &staged_len) == H2_PAL_OK);
    assert(staged_len == sizeof(expected_service_data128));
    assert(memcmp(staged, expected_service_data128,
               sizeof(expected_service_data128)) == 0);

    uint8_t invalid_payload[255] = { 0 };
    h2_pal_ble_adv_data_t invalid_data = {
        .manufacturer_data = { .data = invalid_payload, .len = sizeof(invalid_payload) },
    };
    assert(h2_pal_ble_set_adv_data(ble, &invalid_data) == H2_PAL_ERR_INVALID_ARG);
    assert(h2_pal_ble_start_advertising(ble, &legacy) == H2_PAL_OK);
    assert(h2_pal_ble_stop_advertising(ble) == H2_PAL_OK);
    assert(counts.started == 2u && counts.stopped == 2u);

    uint8_t extended_manufacturer[48];
    uint8_t service_data[] = { 0x0du, 0x18u, 0xa5u, 0x5au };
    memset(extended_manufacturer, 0x33, sizeof(extended_manufacturer));
    data.local_name = "h2-extended-advertising";
    data.service_uuids = service_uuids;
    data.service_uuid_count = sizeof(service_uuids) / sizeof(service_uuids[0]);
    data.manufacturer_data.data = extended_manufacturer;
    data.manufacturer_data.len = sizeof(extended_manufacturer);
    data.service_data.data = service_data;
    data.service_data.len = sizeof(service_data);
    assert(h2_pal_ble_set_adv_data(ble, &data) == H2_PAL_OK);
    assert(h2_pal_ble_start_advertising(ble, &legacy) == H2_PAL_ERR_INVALID_ARG);

    h2_pal_ble_adv_params_t extended = {
        .mode = H2_PAL_BLE_ADV_MODE_NON_CONNECTABLE,
        .interval_min_ms = 100u,
        .interval_max_ms = 120u,
        .type = H2_PAL_BLE_ADV_TYPE_EXTENDED,
        .primary_phy = H2_PAL_BLE_PHY_CODED,
        .secondary_phy = H2_PAL_BLE_PHY_2M,
        .sid = 7u,
    };
    assert(h2_pal_ble_start_advertising(ble, &extended) == H2_PAL_OK);
    assert(counts.started == 3u && counts.stopped == 2u);
    assert(h2_pal_ble_stop_advertising(ble) == H2_PAL_OK);
    assert(counts.stopped == 3u);

    scan_capture_t capture = { 0 };
    h2_pal_ble_scan_params_t scan = {
        .mode = H2_PAL_BLE_SCAN_MODE_PASSIVE,
        .interval_ms = 100u,
        .window_ms = 50u,
        .type = H2_PAL_BLE_SCAN_TYPE_EXTENDED,
        .phy_mask = H2_PAL_BLE_SCAN_PHY_ALL,
    };
    assert(h2_pal_ble_start_scan(ble, &scan, capture_scan, &capture) == H2_PAL_OK);
    assert(capture.count == 1u);
    assert(capture.result.adv_type == H2_PAL_BLE_ADV_TYPE_EXTENDED);
    assert(capture.result.primary_phy == H2_PAL_BLE_PHY_1M);
    assert(capture.result.secondary_phy == H2_PAL_BLE_PHY_2M);
    assert(capture.result.sid == 7u);
    assert(capture.result.data_status == H2_PAL_BLE_ADV_DATA_COMPLETE);
    assert(capture.result.tx_power == 4);
    assert(capture.result.local_name_len == strlen(data.local_name));
    assert(memcmp(capture.result.local_name, data.local_name, strlen(data.local_name)) == 0);
    assert(capture.result.manufacturer_data.len == sizeof(extended_manufacturer));
    assert(memcmp(capture.result.manufacturer_data.data,
               extended_manufacturer, sizeof(extended_manufacturer)) == 0);
    assert(capture.result.service_data.len == sizeof(service_data));
    assert(memcmp(capture.result.service_data.data, service_data, sizeof(service_data)) == 0);
    assert(capture.result.service_uuid_count == 3u);
    assert(capture.result.service_uuids[0].len == sizeof(uuid16_a));
    assert(memcmp(capture.result.service_uuids[0].data, uuid16_a, sizeof(uuid16_a)) == 0);
    assert(capture.result.service_uuids[1].len == sizeof(uuid16_b));
    assert(memcmp(capture.result.service_uuids[1].data, uuid16_b, sizeof(uuid16_b)) == 0);
    assert(capture.result.service_uuids[2].len == sizeof(uuid32));
    assert(memcmp(capture.result.service_uuids[2].data, uuid32, sizeof(uuid32)) == 0);

    scan.phy_mask = H2_PAL_BLE_SCAN_PHY_CODED;
    assert(h2_pal_ble_start_scan(ble, &scan, capture_scan, &capture) == H2_PAL_OK);
    assert(capture.count == 2u);
    assert(capture.result.primary_phy == H2_PAL_BLE_PHY_CODED);

    extended.duration_ms = 100u;
    assert(h2_pal_ble_start_advertising(ble, &extended) == H2_PAL_OK);
    assert(counts.started == 4u && counts.stopped == 4u);

    extended.duration_ms = 0u;
    extended.max_adv_events = 3u;
    assert(h2_pal_ble_start_advertising(ble, &extended) == H2_PAL_OK);
    assert(counts.started == 5u && counts.stopped == 5u);

    extended.max_adv_events = 0u;
    assert(h2_desktop_platform_configure_ble_advertising_start_result(H2_PAL_ERR_IO) == H2_PAL_OK);
    assert(h2_pal_ble_start_advertising(ble, &extended) == H2_PAL_ERR_IO);
    assert(counts.started == 5u && counts.stopped == 5u);
    assert(h2_desktop_platform_configure_ble_advertising_start_result(H2_PAL_OK) == H2_PAL_OK);

    h2_pal_ble_adv_params_t h2loader_params = extended;
    h2loader_params.mode = H2_PAL_BLE_ADV_MODE_CONNECTABLE;
    h2loader_params.sid = 1u;
    h2_pal_ble_adv_params_t beacon_params = extended;
    beacon_params.mode = H2_PAL_BLE_ADV_MODE_NON_CONNECTABLE;
    beacon_params.sid = 2u;
    h2_pal_ble_adv_set_t *h2loader_set = NULL;
    h2_pal_ble_adv_set_t *beacon_set = NULL;
    assert(h2_pal_ble_adv_set_create(ble, &h2loader_params, &h2loader_set) == H2_PAL_OK);
    assert(h2_pal_ble_adv_set_create(ble, &beacon_params, &beacon_set) == H2_PAL_OK);
    assert(h2loader_set != NULL && beacon_set != NULL && h2loader_set != beacon_set);

    uint8_t h2loader_payload[] = { 0x0du, 0x18u, 0x01u };
    uint8_t beacon_payload[] = { 0x0du, 0x18u, 0x02u };
    h2_pal_ble_adv_data_t h2loader_data = {
        .local_name = "H2Loader-devkit",
        .service_data = { .data = h2loader_payload, .len = sizeof(h2loader_payload) },
    };
    h2_pal_ble_adv_data_t beacon_data = {
        .local_name = "invite",
        .service_data = { .data = beacon_payload, .len = sizeof(beacon_payload) },
    };
    assert(h2_pal_ble_adv_set_set_data(ble, h2loader_set, &h2loader_data) == H2_PAL_OK);
    assert(h2_pal_ble_adv_set_set_data(ble, beacon_set, &beacon_data) == H2_PAL_OK);
    memset(h2loader_payload, 0xff, sizeof(h2loader_payload));
    memset(beacon_payload, 0xff, sizeof(beacon_payload));
    uint8_t h2loader_encoded[64];
    uint8_t beacon_encoded[64];
    size_t h2loader_encoded_len = 0u;
    size_t beacon_encoded_len = 0u;
    assert(h2_desktop_platform_copy_ble_adv_set_data(
               h2loader_set, h2loader_encoded, sizeof(h2loader_encoded),
               &h2loader_encoded_len) == H2_PAL_OK);
    assert(h2_desktop_platform_copy_ble_adv_set_data(
               beacon_set, beacon_encoded, sizeof(beacon_encoded),
               &beacon_encoded_len) == H2_PAL_OK);
    assert(h2loader_encoded_len != beacon_encoded_len ||
           memcmp(h2loader_encoded, beacon_encoded, h2loader_encoded_len) != 0);
    assert(memchr(h2loader_encoded, 0xff, h2loader_encoded_len) == NULL);
    assert(memchr(beacon_encoded, 0xff, beacon_encoded_len) == NULL);
    h2_pal_ble_adv_data_t scan_response_data = {
        .local_name = "scan-response",
    };
    assert(h2_pal_ble_adv_set_set_scan_response_data(
               ble, h2loader_set, &scan_response_data) ==
           H2_PAL_ERR_UNSUPPORTED);
    uint8_t unchanged_h2loader_encoded[64];
    size_t unchanged_h2loader_encoded_len = 0u;
    assert(h2_desktop_platform_copy_ble_adv_set_data(
               h2loader_set, unchanged_h2loader_encoded,
               sizeof(unchanged_h2loader_encoded),
               &unchanged_h2loader_encoded_len) == H2_PAL_OK);
    assert(unchanged_h2loader_encoded_len == h2loader_encoded_len);
    assert(memcmp(unchanged_h2loader_encoded, h2loader_encoded,
                  h2loader_encoded_len) == 0);

    assert(h2_pal_ble_adv_set_start(ble, h2loader_set) == H2_PAL_OK);
    assert(counts.last_started_set == h2loader_set);
    assert(h2_desktop_platform_configure_ble_advertising_start_result(
               H2_PAL_ERR_IO) == H2_PAL_OK);
    assert(h2_pal_ble_adv_set_start(ble, beacon_set) == H2_PAL_ERR_IO);
    assert(h2_pal_ble_adv_set_start(ble, h2loader_set) ==
           H2_PAL_ERR_INVALID_STATE);
    assert(counts.started == 6u && counts.stopped == 5u);
    assert(h2_desktop_platform_configure_ble_advertising_start_result(
               H2_PAL_OK) == H2_PAL_OK);
    assert(h2_pal_ble_adv_set_start(ble, beacon_set) == H2_PAL_OK);
    assert(counts.last_started_set == beacon_set);
    assert(counts.started == 7u && counts.stopped == 5u);
    static const uint8_t updated_beacon_payload[] = { 0x0du, 0x18u, 0x03u };
    beacon_data.service_data.data = updated_beacon_payload;
    assert(h2_pal_ble_adv_set_set_data(ble, beacon_set, &beacon_data) == H2_PAL_OK);
    assert(h2_desktop_platform_copy_ble_adv_set_data(
               beacon_set, beacon_encoded, sizeof(beacon_encoded),
               &beacon_encoded_len) == H2_PAL_OK);
    assert(memchr(beacon_encoded, 0x03, beacon_encoded_len) != NULL);
    assert(h2_desktop_platform_copy_ble_adv_set_data(
               h2loader_set, h2loader_encoded, sizeof(h2loader_encoded),
               &h2loader_encoded_len) == H2_PAL_OK);
    assert(memchr(h2loader_encoded, 0x03, h2loader_encoded_len) == NULL);
    assert(counts.started == 7u && counts.stopped == 5u);
    assert(h2_pal_ble_adv_set_stop(ble, beacon_set) == H2_PAL_OK);
    assert(counts.last_stopped_set == beacon_set);
    assert(counts.stopped == 6u);
    assert(h2_pal_ble_adv_set_destroy(ble, beacon_set) == H2_PAL_OK);
    assert(h2_pal_ble_adv_set_start(ble, h2loader_set) == H2_PAL_ERR_INVALID_STATE);
    assert(h2_pal_ble_adv_set_destroy(ble, h2loader_set) == H2_PAL_OK);
    assert(counts.last_stopped_set == h2loader_set);
    assert(counts.stopped == 7u);

    h2_pal_ble_adv_set_t *capacity_sets[4] = { 0 };
    for (size_t i = 0u; i < 4u; ++i) {
        assert(h2_pal_ble_adv_set_create(ble, &beacon_params, &capacity_sets[i]) == H2_PAL_OK);
    }
    h2_pal_ble_adv_set_t *overflow_set = NULL;
    assert(h2_pal_ble_adv_set_create(ble, &beacon_params, &overflow_set) == H2_PAL_ERR_NO_SPACE);
    assert(overflow_set == NULL);
    for (size_t i = 0u; i < 4u; ++i) {
        assert(h2_pal_ble_adv_set_destroy(ble, capacity_sets[i]) == H2_PAL_OK);
    }

    static const uint8_t secure_service_uuid[] = {
        0x10u, 0x11u, 0x12u, 0x13u, 0x14u, 0x15u, 0x16u, 0x17u,
        0x18u, 0x19u, 0x1au, 0x1bu, 0x1cu, 0x1du, 0x1eu, 0x1fu,
    };
    static const uint8_t secure_characteristic_uuid[] = {
        0x20u, 0x21u, 0x22u, 0x23u, 0x24u, 0x25u, 0x26u, 0x27u,
        0x28u, 0x29u, 0x2au, 0x2bu, 0x2cu, 0x2du, 0x2eu, 0x2fu,
    };
    uint16_t secure_value_handle = H2_PAL_BLE_INVALID_ATTR_HANDLE;
    const h2_pal_ble_gatt_characteristic_t secure_characteristic = {
        .uuid = { .data = secure_characteristic_uuid,
                  .len = sizeof(secure_characteristic_uuid) },
        .properties = H2_PAL_BLE_GATT_PROPERTY_READ,
        .permissions = H2_PAL_BLE_GATT_PERMISSION_READ_AUTHENTICATED,
        .max_value_len = sizeof(k_secure_value),
        .read = read_secure_value,
        .user = (void *)k_secure_value,
        .out_value_handle = &secure_value_handle,
    };
    const h2_pal_ble_gatt_service_t secure_service = {
        .uuid = { .data = secure_service_uuid,
                  .len = sizeof(secure_service_uuid) },
        .primary = true,
        .characteristics = &secure_characteristic,
        .characteristic_count = 1u,
    };
    assert(h2_pal_ble_register_gatt_services(ble, &secure_service, 1u) ==
           H2_PAL_OK);
    assert(secure_value_handle != H2_PAL_BLE_INVALID_ATTR_HANDLE);

    h2_pal_ble_addr_t peer = { 0 };
    h2_pal_ble_connect_params_t connect_params = { .timeout_ms = 100u };
    uint16_t conn_handle = H2_PAL_BLE_INVALID_CONN_HANDLE;
    assert(h2_pal_ble_connect(ble, &peer, &connect_params, &conn_handle) ==
           H2_PAL_OK);
    uint8_t secure_read[sizeof(k_secure_value)] = { 0 };
    size_t secure_read_len = 0u;
    assert(h2_pal_ble_gatt_read(ble, conn_handle, secure_value_handle, 0u,
               secure_read, sizeof(secure_read), &secure_read_len, 100u) ==
           H2_PAL_ERR_INVALID_STATE);
    const h2_pal_ble_pairing_config_t invalid_pairing = {
        .enabled = true,
        .passkey = 9999u,
        .io = H2_PAL_BLE_PAIRING_IO_KEYBOARD_ONLY,
    };
    assert(h2_pal_ble_configure_pairing(ble, &invalid_pairing) ==
           H2_PAL_ERR_INVALID_ARG);
    const h2_pal_ble_pairing_config_t pairing = {
        .enabled = true,
        .passkey = 142424u,
        .io = H2_PAL_BLE_PAIRING_IO_KEYBOARD_ONLY,
    };
    assert(h2_pal_ble_configure_pairing(ble, &pairing) == H2_PAL_OK);
    assert(h2_pal_ble_pair(ble, conn_handle, 100u) == H2_PAL_OK);
    assert(h2_pal_ble_gatt_read(ble, conn_handle, secure_value_handle, 0u,
               secure_read, sizeof(secure_read), &secure_read_len, 100u) ==
           H2_PAL_OK);
    assert(secure_read_len == sizeof(k_secure_value));
    assert(memcmp(secure_read, k_secure_value, sizeof(k_secure_value)) == 0);
    assert(h2_pal_ble_disconnect(ble, conn_handle) == H2_PAL_OK);
    secure_read_len = 0u;
    assert(h2_pal_ble_gatt_read(ble, conn_handle, secure_value_handle, 0u,
               secure_read, sizeof(secure_read), &secure_read_len, 100u) ==
           H2_PAL_ERR_INVALID_STATE);
    const h2_pal_ble_pairing_config_t pairing_disabled = { 0 };
    assert(h2_pal_ble_configure_pairing(ble, &pairing_disabled) == H2_PAL_OK);
    assert(h2_pal_ble_unregister_gatt_services(ble) == H2_PAL_OK);

    assert(h2_pal_ble_stop(ble) == H2_PAL_OK);
    assert(h2_desktop_platform_configure_ble_extended_advertising(0) == H2_PAL_OK);
    assert(h2_pal_ble_start(ble) == H2_PAL_OK);
    assert(h2_pal_ble_set_adv_data(ble, &data) == H2_PAL_OK);
    assert(h2_pal_ble_start_advertising(ble, &extended) == H2_PAL_ERR_UNSUPPORTED);
    assert(h2_pal_ble_stop(ble) == H2_PAL_OK);
    assert(h2_desktop_platform_configure_ble_extended_advertising(1) == H2_PAL_OK);
    assert(h2_desktop_platform_configure_ble_extended_scanning(0) == H2_PAL_OK);
    assert(h2_pal_ble_start(ble) == H2_PAL_OK);
    assert(h2_pal_ble_start_scan(ble, &scan, capture_scan, &capture) == H2_PAL_ERR_UNSUPPORTED);
    assert(h2_pal_ble_stop(ble) == H2_PAL_OK);
    assert(h2_desktop_platform_configure_ble_extended_scanning(1) == H2_PAL_OK);

    h2_pal_system_event_unsubscribe(events, stopped_sub);
    h2_pal_system_event_unsubscribe(events, started_sub);
    h2_desktop_test_system_event_deinit();
    return 0;
}
