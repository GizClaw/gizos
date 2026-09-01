#include "h2_bk3633_platform_core.h"
#include "h2_bk3633_ble_sdk_fake.h"
#include "h2_bk3633_mem_test_support.h"

#include <assert.h>
#include <string.h>

typedef struct event_capture {
    h2_pal_ble_adv_set_event_t events[12];
    size_t count;
} event_capture_t;

static h2_pal_result_t capture_event(
    void *user, const h2_pal_system_event_t *event) {
    event_capture_t *capture = user;
    assert(event->payload_size == sizeof(h2_pal_ble_adv_set_event_t));
    assert(capture->count < 12u);
    memcpy(&capture->events[capture->count++], event->payload,
           sizeof(h2_pal_ble_adv_set_event_t));
    return H2_PAL_OK;
}

static const h2_bk3633_ble_sdk_fake_message_t *message_with_id(
    uint16_t id, size_t occurrence) {
    for (size_t i = 0u; i < h2_bk3633_ble_sdk_fake_message_count(); ++i) {
        const h2_bk3633_ble_sdk_fake_message_t *message =
            h2_bk3633_ble_sdk_fake_message(i);
        if (message->id == id && occurrence-- == 0u) {
            return message;
        }
    }
    return NULL;
}

static const h2_bk3633_ble_sdk_fake_message_t *last_message_with_id(
    uint16_t id) {
    for (size_t i = h2_bk3633_ble_sdk_fake_message_count(); i > 0u; --i) {
        const h2_bk3633_ble_sdk_fake_message_t *message =
            h2_bk3633_ble_sdk_fake_message(i - 1u);
        if (message->id == id) return message;
    }
    return NULL;
}

static size_t message_count_with_id(uint16_t id) {
    size_t count = 0u;
    for (size_t i = 0u; i < h2_bk3633_ble_sdk_fake_message_count(); ++i) {
        if (h2_bk3633_ble_sdk_fake_message(i)->id == id) ++count;
    }
    return count;
}

int main(void) {
    h2_bk3633_mem_test_support_init();
    h2_bk3633_ble_sdk_fake_reset();
    h2_bk3633_platform_ble_test_reset();
    assert(h2_pal_system_event_init(
               h2_bk3633_platform_system_event_api()) == H2_PAL_OK);
    assert(h2_bk3633_platform_ble_host_bootstrap_begin() == H2_PAL_OK);
    h2_bk3633_platform_ble_host_bootstrap_complete(H2_PAL_OK);

    event_capture_t capture = {0};
    h2_pal_system_event_subscription_t *started_subscription = NULL;
    h2_pal_system_event_subscription_t *stopped_subscription = NULL;
    assert(h2_pal_system_event_subscribe(
               h2_bk3633_platform_system_event_api(),
               H2_PAL_SYSTEM_EVENT_TYPE_BLE_ADVERTISING_STARTED,
               capture_event, &capture, &started_subscription) == H2_PAL_OK);
    assert(h2_pal_system_event_subscribe(
               h2_bk3633_platform_system_event_api(),
               H2_PAL_SYSTEM_EVENT_TYPE_BLE_ADVERTISING_STOPPED,
               capture_event, &capture, &stopped_subscription) == H2_PAL_OK);

    const h2_pal_ble_host_api_t *api = h2_bk3633_platform_ble_api();
    const h2_pal_ble_adv_params_t legacy_params = {
        .mode = H2_PAL_BLE_ADV_MODE_CONNECTABLE,
        .interval_min_ms = 100u,
        .interval_max_ms = 120u,
        .type = H2_PAL_BLE_ADV_TYPE_LEGACY,
        .primary_phy = H2_PAL_BLE_PHY_1M,
        .secondary_phy = H2_PAL_BLE_PHY_1M,
    };
    const h2_pal_ble_adv_params_t extended_params = {
        .mode = H2_PAL_BLE_ADV_MODE_NON_CONNECTABLE,
        .interval_min_ms = 160u,
        .interval_max_ms = 200u,
        .type = H2_PAL_BLE_ADV_TYPE_EXTENDED,
        .primary_phy = H2_PAL_BLE_PHY_1M,
        .secondary_phy = H2_PAL_BLE_PHY_2M,
        .sid = 1u,
    };
    h2_pal_ble_adv_set_t *legacy = NULL;
    h2_pal_ble_adv_set_t *extended = NULL;
    assert(h2_pal_ble_adv_set_create(api, &legacy_params, &legacy) ==
           H2_PAL_OK);
    assert(h2_pal_ble_adv_set_create(api, &extended_params, &extended) ==
           H2_PAL_OK);
    assert(legacy != NULL && extended != NULL && legacy != extended);
    h2_pal_ble_adv_set_t *extra = NULL;
    assert(h2_pal_ble_adv_set_create(api, &extended_params, &extra) ==
           H2_PAL_ERR_NO_SPACE);

    uint8_t vendor_uuid[] = {0xe0u, 0xfeu};
    uint8_t battery_uuid[] = {0x0fu, 0x18u};
    uint8_t service_data[] = {0xaau, 0xbbu, 0xccu};
    const h2_pal_ble_uuid_t uuids[] = {
        {
            .data = vendor_uuid,
            .len = sizeof(vendor_uuid),
        },
        {
            .data = battery_uuid,
            .len = sizeof(battery_uuid),
        },
    };
    const h2_pal_ble_adv_data_t data = {
        .local_name = "p",
        .service_uuids = uuids,
        .service_uuid_count = 2u,
        .service_data_uuid = uuids[0],
        .service_data = {
            .data = service_data,
            .len = sizeof(service_data),
        },
    };
    char oversized_scan_response_name[31];
    const uint8_t oversized_manufacturer_data = 0xaau;
    memset(oversized_scan_response_name, 'x',
           sizeof(oversized_scan_response_name) - 1u);
    oversized_scan_response_name[sizeof(oversized_scan_response_name) - 1u] =
        '\0';
    const h2_pal_ble_adv_data_t oversized_scan_response = {
        .local_name = oversized_scan_response_name,
        .manufacturer_data = {
            .data = &oversized_manufacturer_data,
            .len = sizeof(oversized_manufacturer_data),
        },
    };
    assert(h2_pal_ble_adv_set_set_data(api, legacy, &data) == H2_PAL_OK);
    assert(h2_pal_ble_adv_set_set_data(api, extended, &data) == H2_PAL_OK);
    char scan_response_name[] = "scan";
    const h2_pal_ble_adv_data_t scan_response = {
        .local_name = scan_response_name,
    };
    assert(h2_pal_ble_adv_set_set_scan_response_data(
               api, legacy, &scan_response) == H2_PAL_OK);
    assert(h2_pal_ble_adv_set_set_scan_response_data(
               api, legacy, &oversized_scan_response) ==
           H2_PAL_ERR_NO_SPACE);
    assert(h2_pal_ble_adv_set_set_scan_response_data(
               api, extended, &scan_response) == H2_PAL_ERR_UNSUPPORTED);
    assert(h2_pal_ble_adv_set_start(api, legacy) == H2_PAL_OK);
    assert(h2_pal_ble_adv_set_start(api, extended) == H2_PAL_OK);
    vendor_uuid[0] = 0u;
    vendor_uuid[1] = 0u;
    memset(scan_response_name, 'x', sizeof(scan_response_name) - 1u);
    battery_uuid[0] = 0u;
    battery_uuid[1] = 0u;
    memset(service_data, 0, sizeof(service_data));

    const struct gapm_activity_created_ind legacy_created = {
        .actv_idx = 7u,
        .actv_type = GAPM_ACTV_TYPE_ADV,
    };
    const struct gapm_activity_created_ind extended_created = {
        .actv_idx = 9u,
        .actv_type = GAPM_ACTV_TYPE_ADV,
    };
    (void)h2_bk3633_platform_ble_dispatch(
        GAPM_ACTIVITY_CREATED_IND, &legacy_created, TASK_APP, TASK_GAPM);
    (void)h2_bk3633_platform_ble_dispatch(
        GAPM_ACTIVITY_CREATED_IND, &extended_created, TASK_APP, TASK_GAPM);
    const h2_bk3633_ble_sdk_fake_message_t *legacy_data_message =
        message_with_id(GAPM_SET_ADV_DATA_CMD, 0u);
    const h2_bk3633_ble_sdk_fake_message_t *extended_data_message =
        message_with_id(GAPM_SET_ADV_DATA_CMD, 1u);
    assert(legacy_data_message != NULL && extended_data_message != NULL);
    const struct gapm_set_adv_data_cmd *legacy_data_command =
        legacy_data_message->payload;
    const struct gapm_set_adv_data_cmd *extended_data_command =
        extended_data_message->payload;
    assert(legacy_data_command->actv_idx == 7u);
    assert(extended_data_command->actv_idx == 9u);
    static const uint8_t expected_data[] = {
        2u, 0x01u, 0x06u,
        5u, 0x03u, 0xe0u, 0xfeu, 0x0fu, 0x18u,
        6u, 0x16u, 0xe0u, 0xfeu, 0xaau, 0xbbu, 0xccu,
        2u, 0x09u, 'p',
    };
    assert(legacy_data_command->length == sizeof(expected_data));
    assert(extended_data_command->length == sizeof(expected_data));
    assert(memcmp(legacy_data_command->data, expected_data,
                  sizeof(expected_data)) == 0);
    assert(memcmp(extended_data_command->data, expected_data,
                  sizeof(expected_data)) == 0);
    const struct gapm_cmp_evt data_complete = {
        .operation = GAPM_SET_ADV_DATA,
        .status = GAP_ERR_NO_ERROR,
    };
    (void)h2_bk3633_platform_ble_dispatch(
        GAPM_CMP_EVT, &data_complete, TASK_APP, TASK_GAPM);

    const h2_bk3633_ble_sdk_fake_message_t *scan_response_message =
        message_with_id(GAPM_SET_ADV_DATA_CMD, 2u);
    assert(scan_response_message != NULL);
    const struct gapm_set_adv_data_cmd *scan_response_command =
        scan_response_message->payload;
    static const uint8_t expected_scan_response[] = {
        5u, 0x09u, 's', 'c', 'a', 'n',
    };
    assert(scan_response_command->operation == GAPM_SET_SCAN_RSP_DATA);
    assert(scan_response_command->actv_idx == 7u);
    assert(scan_response_command->length == sizeof(expected_scan_response));
    assert(memcmp(scan_response_command->data, expected_scan_response,
                  sizeof(expected_scan_response)) == 0);

    (void)h2_bk3633_platform_ble_dispatch(
        GAPM_CMP_EVT, &data_complete, TASK_APP, TASK_GAPM);

    const h2_bk3633_ble_sdk_fake_message_t *extended_start =
        message_with_id(GAPM_ACTIVITY_START_CMD, 0u);
    assert(extended_start != NULL);
    assert(message_with_id(GAPM_ACTIVITY_START_CMD, 1u) == NULL);
    const struct gapm_cmp_evt scan_response_complete = {
        .operation = GAPM_SET_SCAN_RSP_DATA,
        .status = GAP_ERR_NO_ERROR,
    };
    (void)h2_bk3633_platform_ble_dispatch(
        GAPM_CMP_EVT, &scan_response_complete, TASK_APP, TASK_GAPM);
    const h2_bk3633_ble_sdk_fake_message_t *legacy_start =
        message_with_id(GAPM_ACTIVITY_START_CMD, 1u);
    assert(legacy_start != NULL && extended_start != NULL);
    assert(((const struct gapm_activity_start_cmd *)legacy_start->payload)
               ->actv_idx == 7u);
    assert(((const struct gapm_activity_start_cmd *)extended_start->payload)
               ->actv_idx == 9u);

    const struct gapm_cmp_evt start_complete = {
        .operation = GAPM_START_ACTIVITY,
        .status = GAP_ERR_NO_ERROR,
    };
    const h2_pal_system_event_t filler = {
        .type = H2_PAL_SYSTEM_EVENT_TYPE_BLE_HOST_STARTED,
    };
    for (size_t i = 0u; i < 8u; ++i) {
        assert(h2_pal_system_event_post(
                   h2_bk3633_platform_system_event_api(),
                   &filler, 0u) == H2_PAL_OK);
    }
    (void)h2_bk3633_platform_ble_dispatch(
        GAPM_CMP_EVT, &start_complete, TASK_APP, TASK_GAPM);
    (void)h2_bk3633_platform_ble_dispatch(
        GAPM_CMP_EVT, &start_complete, TASK_APP, TASK_GAPM);
    assert(h2_bk3633_platform_system_event_dispatch_pending() ==
           H2_PAL_ERR_FULL);
    assert(capture.count == 2u);
    assert(capture.events[0].set == extended);
    assert(capture.events[1].set == legacy);

    const h2_pal_ble_adv_data_t empty_scan_response = {0};
    assert(h2_pal_ble_adv_set_set_scan_response_data(
               api, legacy, &empty_scan_response) == H2_PAL_OK);
    const h2_bk3633_ble_sdk_fake_message_t *clear_scan_response_message =
        message_with_id(GAPM_SET_ADV_DATA_CMD, 3u);
    assert(clear_scan_response_message != NULL);
    const struct gapm_set_adv_data_cmd *clear_scan_response_command =
        clear_scan_response_message->payload;
    assert(clear_scan_response_command->operation == GAPM_SET_SCAN_RSP_DATA);
    assert(clear_scan_response_command->length == 0u);
    (void)h2_bk3633_platform_ble_dispatch(
        GAPM_CMP_EVT, &scan_response_complete, TASK_APP, TASK_GAPM);
    assert(message_with_id(GAPM_ACTIVITY_START_CMD, 2u) == NULL);

    const struct gapc_connection_req_ind connection = {
        .conhdl = 1u,
        .role = 1u,
    };
    (void)h2_bk3633_platform_ble_dispatch(
        GAPC_CONNECTION_REQ_IND, &connection, TASK_APP,
        KE_BUILD_ID(TASK_GAPC, 0u));
    assert(h2_bk3633_platform_system_event_dispatch_next(NULL) == H2_PAL_OK);
    assert(h2_pal_ble_adv_set_start(api, legacy) == H2_PAL_OK);
    const h2_bk3633_ble_sdk_fake_message_t *legacy_restart =
        message_with_id(GAPM_ACTIVITY_START_CMD, 2u);
    assert(legacy_restart != NULL);
    assert(((const struct gapm_activity_start_cmd *)legacy_restart->payload)
               ->actv_idx == 7u);
    (void)h2_bk3633_platform_ble_dispatch(
        GAPM_CMP_EVT, &start_complete, TASK_APP, TASK_GAPM);
    assert(h2_bk3633_platform_system_event_dispatch_next(NULL) == H2_PAL_OK);
    assert(capture.count == 3u);
    assert(capture.events[2].set == legacy);

    assert(h2_pal_ble_adv_set_stop(api, extended) == H2_PAL_OK);
    assert(h2_pal_ble_adv_set_destroy(api, extended) == H2_PAL_OK);
    const struct gapm_cmp_evt stop_complete = {
        .operation = GAPM_STOP_ACTIVITY,
        .status = GAP_ERR_NO_ERROR,
    };
    (void)h2_bk3633_platform_ble_dispatch(
        GAPM_CMP_EVT, &stop_complete, TASK_APP, TASK_GAPM);
    const struct gapm_activity_stopped_ind stopped = {
        .actv_idx = 9u,
        .actv_type = GAPM_ACTV_TYPE_ADV,
    };
    (void)h2_bk3633_platform_ble_dispatch(
        GAPM_ACTIVITY_STOPPED_IND, &stopped, TASK_APP, TASK_GAPM);
    const h2_bk3633_ble_sdk_fake_message_t *delete_message =
        message_with_id(GAPM_ACTIVITY_DELETE_CMD, 0u);
    assert(delete_message != NULL);
    assert(((const struct gapm_activity_delete_cmd *)delete_message->payload)
               ->actv_idx == 9u);
    const struct gapm_cmp_evt delete_complete = {
        .operation = GAPM_DELETE_ACTIVITY,
        .status = GAP_ERR_NO_ERROR,
    };
    (void)h2_bk3633_platform_ble_dispatch(
        GAPM_CMP_EVT, &delete_complete, TASK_APP, TASK_GAPM);
    h2_pal_ble_adv_params_t nonconnectable_legacy_params = legacy_params;
    nonconnectable_legacy_params.mode =
        H2_PAL_BLE_ADV_MODE_NON_CONNECTABLE;
    assert(h2_pal_ble_adv_set_create(
               api, &nonconnectable_legacy_params, &extra) ==
           H2_PAL_OK);
    assert(h2_pal_ble_adv_set_set_scan_response_data(
               api, extra, &scan_response) == H2_PAL_ERR_UNSUPPORTED);
    assert(h2_bk3633_platform_system_event_dispatch_next(NULL) == H2_PAL_OK);
    assert(capture.count == 4u);
    assert(capture.events[3].set == extended);

    assert(h2_pal_ble_adv_set_destroy(api, extra) == H2_PAL_OK);
    h2_pal_ble_adv_set_t *failed_before_start = NULL;
    assert(h2_pal_ble_adv_set_create(
               api, &legacy_params, &failed_before_start) == H2_PAL_OK);
    assert(h2_pal_ble_adv_set_set_scan_response_data(
               api, failed_before_start, &scan_response) == H2_PAL_OK);
    assert(h2_pal_ble_adv_set_start(api, failed_before_start) == H2_PAL_OK);
    const struct gapm_activity_created_ind failed_set_created = {
        .actv_idx = 11u,
        .actv_type = GAPM_ACTV_TYPE_ADV,
    };
    (void)h2_bk3633_platform_ble_dispatch(
        GAPM_ACTIVITY_CREATED_IND, &failed_set_created, TASK_APP, TASK_GAPM);
    const h2_bk3633_ble_sdk_fake_message_t *failed_scan_response_message =
        message_with_id(GAPM_SET_ADV_DATA_CMD, 4u);
    assert(failed_scan_response_message != NULL);
    assert(((const struct gapm_set_adv_data_cmd *)
                failed_scan_response_message->payload)->operation ==
           GAPM_SET_SCAN_RSP_DATA);
    const struct gapm_cmp_evt scan_response_failed = {
        .operation = GAPM_SET_SCAN_RSP_DATA,
        .status = 1u,
    };
    (void)h2_bk3633_platform_ble_dispatch(
        GAPM_CMP_EVT, &scan_response_failed, TASK_APP, TASK_GAPM);
    assert(message_with_id(GAPM_ACTIVITY_START_CMD, 3u) == NULL);
    assert(h2_bk3633_platform_system_event_dispatch_next(NULL) == H2_PAL_OK);
    assert(capture.count == 5u);
    assert(capture.events[4].set == failed_before_start);
    assert(capture.events[4].status == H2_PAL_ERR_IO);

    static const uint8_t exact_initial[] = {
        3u, 0xffu, 1u, 2u,
        3u, 0xffu, 3u, 4u,
        3u, 0xffu, 5u, 6u,
    };
    size_t message_count = h2_bk3633_ble_sdk_fake_message_count();
    assert(h2_pal_ble_adv_set_set_encoded_data(
               api, failed_before_start, exact_initial,
               sizeof(exact_initial)) == H2_PAL_OK);
    assert(h2_bk3633_ble_sdk_fake_message_count() == message_count);
    assert(h2_pal_ble_adv_set_start(api, failed_before_start) == H2_PAL_OK);
    const struct gapm_set_adv_data_cmd *exact_initial_command =
        last_message_with_id(GAPM_SET_ADV_DATA_CMD)->payload;
    assert(exact_initial_command->operation == GAPM_SET_ADV_DATA);
    assert(exact_initial_command->actv_idx == 11u);
    assert(exact_initial_command->length == sizeof(exact_initial));
    assert(memcmp(exact_initial_command->data, exact_initial,
                  sizeof(exact_initial)) == 0);
    (void)h2_bk3633_platform_ble_dispatch(
        GAPM_CMP_EVT, &data_complete, TASK_APP, TASK_GAPM);
    (void)h2_bk3633_platform_ble_dispatch(
        GAPM_CMP_EVT, &scan_response_complete, TASK_APP, TASK_GAPM);
    (void)h2_bk3633_platform_ble_dispatch(
        GAPM_CMP_EVT, &start_complete, TASK_APP, TASK_GAPM);
    assert(h2_bk3633_platform_system_event_dispatch_next(NULL) == H2_PAL_OK);
    assert(capture.events[capture.count - 1u].set == failed_before_start);
    assert(capture.events[capture.count - 1u].status == H2_PAL_OK);

    static const uint8_t destroy_update[] = { 2u, 0xffu, 0x10u };
    size_t destroy_stop_count = message_count_with_id(
        GAPM_ACTIVITY_STOP_CMD);
    size_t destroy_delete_count = message_count_with_id(
        GAPM_ACTIVITY_DELETE_CMD);
    assert(h2_pal_ble_adv_set_set_encoded_data(
               api, legacy, destroy_update,
               sizeof(destroy_update)) == H2_PAL_OK);
    assert(h2_pal_ble_adv_set_destroy(api, legacy) == H2_PAL_OK);
    assert(message_count_with_id(GAPM_ACTIVITY_STOP_CMD) ==
           destroy_stop_count);
    (void)h2_bk3633_platform_ble_dispatch(
        GAPM_CMP_EVT, &data_complete, TASK_APP, TASK_GAPM);
    assert(message_count_with_id(GAPM_ACTIVITY_STOP_CMD) ==
           destroy_stop_count + 1u);
    (void)h2_bk3633_platform_ble_dispatch(
        GAPM_CMP_EVT, &stop_complete, TASK_APP, TASK_GAPM);
    const struct gapm_activity_stopped_ind destroyed_stopped = {
        .actv_idx = 7u,
        .actv_type = GAPM_ACTV_TYPE_ADV,
    };
    (void)h2_bk3633_platform_ble_dispatch(
        GAPM_ACTIVITY_STOPPED_IND, &destroyed_stopped, TASK_APP, TASK_GAPM);
    assert(message_count_with_id(GAPM_ACTIVITY_DELETE_CMD) ==
           destroy_delete_count + 1u);
    (void)h2_bk3633_platform_ble_dispatch(
        GAPM_CMP_EVT, &delete_complete, TASK_APP, TASK_GAPM);
    assert(h2_bk3633_platform_system_event_dispatch_next(NULL) == H2_PAL_OK);
    assert(capture.events[capture.count - 1u].set == legacy);

    static const uint8_t update_one[] = { 2u, 0xffu, 0x11u };
    static const uint8_t update_two[] = { 2u, 0xffu, 0x22u };
    static const uint8_t update_three[] = { 2u, 0xffu, 0x33u };
    message_count = h2_bk3633_ble_sdk_fake_message_count();
    h2_bk3633_ble_sdk_fake_fail_next_allocations(1u);
    assert(h2_pal_ble_adv_set_set_encoded_data(
               api, failed_before_start, update_two,
               sizeof(update_two)) == H2_PAL_ERR_NO_MEMORY);
    assert(h2_bk3633_ble_sdk_fake_message_count() == message_count);
    assert(h2_pal_ble_adv_set_set_encoded_data(
               api, failed_before_start, update_one,
               sizeof(update_one)) == H2_PAL_OK);
    assert(h2_pal_ble_adv_set_set_encoded_data(
               api, failed_before_start, update_two,
               sizeof(update_two)) == H2_PAL_OK);
    assert(h2_pal_ble_adv_set_set_encoded_data(
               api, failed_before_start, update_three,
               sizeof(update_three)) == H2_PAL_OK);
    assert(h2_bk3633_ble_sdk_fake_message_count() == message_count + 1u);
    (void)h2_bk3633_platform_ble_dispatch(
        GAPM_CMP_EVT, &data_complete, TASK_APP, TASK_GAPM);
    const struct gapm_set_adv_data_cmd *latest_command =
        last_message_with_id(GAPM_SET_ADV_DATA_CMD)->payload;
    assert(latest_command->operation == GAPM_SET_ADV_DATA);
    assert(latest_command->length == sizeof(update_three));
    assert(memcmp(latest_command->data, update_three,
                  sizeof(update_three)) == 0);
    (void)h2_bk3633_platform_ble_dispatch(
        GAPM_CMP_EVT, &data_complete, TASK_APP, TASK_GAPM);

    size_t stop_message_count = message_count_with_id(
        GAPM_ACTIVITY_STOP_CMD);
    assert(h2_pal_ble_adv_set_set_encoded_data(
               api, failed_before_start, update_one,
               sizeof(update_one)) == H2_PAL_OK);
    assert(h2_pal_ble_adv_set_stop(api, failed_before_start) == H2_PAL_OK);
    assert(message_count_with_id(GAPM_ACTIVITY_STOP_CMD) ==
           stop_message_count);
    (void)h2_bk3633_platform_ble_dispatch(
        GAPM_CMP_EVT, &data_complete, TASK_APP, TASK_GAPM);
    assert(message_count_with_id(GAPM_ACTIVITY_STOP_CMD) ==
           stop_message_count + 1u);
    (void)h2_bk3633_platform_ble_dispatch(
        GAPM_CMP_EVT, &stop_complete, TASK_APP, TASK_GAPM);
    const struct gapm_activity_stopped_ind normal_stopped = {
        .actv_idx = 11u,
        .actv_type = GAPM_ACTV_TYPE_ADV,
    };
    (void)h2_bk3633_platform_ble_dispatch(
        GAPM_ACTIVITY_STOPPED_IND, &normal_stopped, TASK_APP, TASK_GAPM);
    assert(h2_bk3633_platform_system_event_dispatch_next(NULL) == H2_PAL_OK);
    assert(capture.events[capture.count - 1u].set == failed_before_start);
    assert(capture.events[capture.count - 1u].status == H2_PAL_OK);
    assert(h2_pal_ble_adv_set_start(api, failed_before_start) == H2_PAL_OK);
    (void)h2_bk3633_platform_ble_dispatch(
        GAPM_CMP_EVT, &start_complete, TASK_APP, TASK_GAPM);
    assert(h2_bk3633_platform_system_event_dispatch_next(NULL) == H2_PAL_OK);

    static const uint8_t failed_update[] = { 2u, 0xffu, 0x44u };
    assert(h2_pal_ble_adv_set_set_encoded_data(
               api, failed_before_start, failed_update,
               sizeof(failed_update)) == H2_PAL_OK);
    const struct gapm_cmp_evt data_failed = {
        .operation = GAPM_SET_ADV_DATA,
        .status = GAP_ERR_REJECTED,
    };
    (void)h2_bk3633_platform_ble_dispatch(
        GAPM_CMP_EVT, &data_failed, TASK_APP, TASK_GAPM);
    const struct gapm_activity_stop_cmd *failure_stop =
        last_message_with_id(GAPM_ACTIVITY_STOP_CMD)->payload;
    assert(failure_stop->actv_idx == 11u);
    (void)h2_bk3633_platform_ble_dispatch(
        GAPM_CMP_EVT, &stop_complete, TASK_APP, TASK_GAPM);
    const struct gapm_activity_stopped_ind failed_stopped = {
        .actv_idx = 11u,
        .actv_type = GAPM_ACTV_TYPE_ADV,
    };
    (void)h2_bk3633_platform_ble_dispatch(
        GAPM_ACTIVITY_STOPPED_IND, &failed_stopped, TASK_APP, TASK_GAPM);
    assert(h2_bk3633_platform_system_event_dispatch_next(NULL) == H2_PAL_OK);
    assert(capture.events[capture.count - 1u].set == failed_before_start);
    assert(capture.events[capture.count - 1u].status == H2_PAL_ERR_IO);

    assert(h2_pal_ble_adv_set_start(api, failed_before_start) == H2_PAL_OK);
    const struct gapm_set_adv_data_cmd *restart_data =
        last_message_with_id(GAPM_SET_ADV_DATA_CMD)->payload;
    assert(restart_data->length == sizeof(failed_update));
    assert(memcmp(restart_data->data, failed_update,
                  sizeof(failed_update)) == 0);
    (void)h2_bk3633_platform_ble_dispatch(
        GAPM_CMP_EVT, &data_complete, TASK_APP, TASK_GAPM);
    (void)h2_bk3633_platform_ble_dispatch(
        GAPM_CMP_EVT, &start_complete, TASK_APP, TASK_GAPM);
    assert(h2_bk3633_platform_system_event_dispatch_next(NULL) == H2_PAL_OK);

    static const uint8_t terminal_update[] = { 2u, 0xffu, 0x55u };
    assert(h2_pal_ble_adv_set_set_encoded_data(
               api, failed_before_start, terminal_update,
               sizeof(terminal_update)) == H2_PAL_OK);
    h2_bk3633_ble_sdk_fake_fail_next_allocations(1u);
    (void)h2_bk3633_platform_ble_dispatch(
        GAPM_CMP_EVT, &data_failed, TASK_APP, TASK_GAPM);
    assert(h2_bk3633_platform_system_event_dispatch_next(NULL) == H2_PAL_OK);
    assert(capture.events[capture.count - 1u].set == failed_before_start);
    assert(capture.events[capture.count - 1u].status ==
           H2_PAL_ERR_NO_MEMORY);
    assert(h2_pal_ble_adv_set_start(api, failed_before_start) ==
           H2_PAL_ERR_INVALID_STATE);
    assert(h2_pal_ble_adv_set_set_encoded_data(
               api, failed_before_start, update_one,
               sizeof(update_one)) == H2_PAL_ERR_INVALID_STATE);
    assert(h2_pal_ble_adv_set_set_scan_response_data(
               api, failed_before_start, &scan_response) ==
           H2_PAL_ERR_INVALID_STATE);
    size_t terminal_event_count = capture.count;
    (void)h2_bk3633_platform_ble_dispatch(
        GAPM_ACTIVITY_STOPPED_IND, &failed_stopped, TASK_APP, TASK_GAPM);
    assert(h2_bk3633_platform_system_event_dispatch_next(NULL) == H2_PAL_OK);
    assert(capture.count == terminal_event_count);
    return 0;
}
