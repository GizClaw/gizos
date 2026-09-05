#include "h2_bk3633_platform_core.h"
#include "h2_bk3633_ble_sdk_fake.h"
#include "h2_bk3633_mem_test_support.h"

#include <assert.h>
#include <string.h>

static unsigned s_report_calls;
static h2_pal_ble_scan_result_t s_report;
static uint8_t s_report_data[32];

static bool capture_report(
    void *user, const h2_pal_ble_scan_result_t *result) {
    (void)user;
    ++s_report_calls;
    s_report = *result;
    assert(result->raw_data.len <= sizeof(s_report_data));
    memcpy(s_report_data, result->raw_data.data, result->raw_data.len);
    s_report.raw_data.data = s_report_data;
    return false;
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

static const h2_bk3633_ble_sdk_fake_message_t *first_message_with_id(
    uint16_t id) {
    for (size_t i = 0u; i < h2_bk3633_ble_sdk_fake_message_count(); ++i) {
        const h2_bk3633_ble_sdk_fake_message_t *message =
            h2_bk3633_ble_sdk_fake_message(i);
        if (message->id == id) return message;
    }
    return NULL;
}

int main(void) {
    h2_bk3633_mem_test_support_init();
    h2_bk3633_ble_sdk_fake_reset();
    h2_bk3633_platform_ble_test_reset();
    assert(h2_bk3633_platform_ble_host_bootstrap_begin() == H2_PAL_OK);
    h2_bk3633_platform_ble_host_bootstrap_complete(H2_PAL_OK);
    const h2_pal_ble_host_api_t *api = h2_bk3633_platform_ble_api();

    const h2_pal_ble_adv_params_t advertising = {
        .mode = H2_PAL_BLE_ADV_MODE_CONNECTABLE,
        .interval_min_ms = 100u,
        .interval_max_ms = 120u,
        .type = H2_PAL_BLE_ADV_TYPE_LEGACY,
        .primary_phy = H2_PAL_BLE_PHY_1M,
        .secondary_phy = H2_PAL_BLE_PHY_1M,
    };
    h2_pal_ble_adv_set_t *set = NULL;
    assert(h2_pal_ble_adv_set_create(api, &advertising, &set) == H2_PAL_OK);
    assert(h2_pal_ble_adv_set_start(api, set) == H2_PAL_OK);

    const h2_pal_ble_scan_params_t scan = {
        .mode = H2_PAL_BLE_SCAN_MODE_PASSIVE,
        .type = H2_PAL_BLE_SCAN_TYPE_EXTENDED,
        .phy_mask = H2_PAL_BLE_SCAN_PHY_ALL,
        .interval_units_625us = 4u,
        .window_units_625us = 4u,
    };
    assert(h2_pal_ble_start_scan(api, &scan, capture_report, NULL) ==
           H2_PAL_OK);
    const struct gapm_activity_created_ind scan_created = {
        .actv_idx = 5u,
        .actv_type = GAPM_ACTV_TYPE_SCAN,
    };
    const struct gapm_activity_created_ind adv_created = {
        .actv_idx = 8u,
        .actv_type = GAPM_ACTV_TYPE_ADV,
    };
    (void)h2_bk3633_platform_ble_dispatch(
        GAPM_ACTIVITY_CREATED_IND, &scan_created, TASK_APP, TASK_GAPM);
    (void)h2_bk3633_platform_ble_dispatch(
        GAPM_ACTIVITY_CREATED_IND, &adv_created, TASK_APP, TASK_GAPM);
    const h2_bk3633_ble_sdk_fake_message_t *start =
        first_message_with_id(GAPM_ACTIVITY_START_CMD);
    assert(start != NULL);
    const struct gapm_activity_start_cmd *start_command = start->payload;
    assert(start_command->actv_idx == 5u);
    assert((start_command->u_param.scan_param.prop &
            (GAPM_SCAN_PROP_ACTIVE_1M_BIT |
             GAPM_SCAN_PROP_ACTIVE_CODED_BIT)) == 0u);
    assert(start_command->u_param.scan_param.scan_param_1m.scan_intv == 4u);
    assert(start_command->u_param.scan_param.scan_param_1m.scan_wd == 4u);
    assert(start_command->u_param.scan_param.scan_param_coded.scan_intv == 4u);
    assert(start_command->u_param.scan_param.scan_param_coded.scan_wd == 4u);

    const struct gapm_cmp_evt start_complete = {
        .operation = GAPM_START_ACTIVITY,
        .status = GAP_ERR_NO_ERROR,
    };
    (void)h2_bk3633_platform_ble_dispatch(
        GAPM_CMP_EVT, &start_complete, TASK_APP, TASK_GAPM);

    uint8_t report_storage[sizeof(struct gapm_ext_adv_report_ind) + 5u] = {0};
    struct gapm_ext_adv_report_ind *report =
        (struct gapm_ext_adv_report_ind *)report_storage;
    report->trans_addr.addr_type = 1u;
    report->trans_addr.addr.addr[0] = 0xaau;
    report->rssi = -42;
    report->info = GAPM_REPORT_INFO_CONN_ADV_BIT |
                   GAPM_REPORT_INFO_COMPLETE_BIT;
    report->phy_prim = GAP_PHY_LE_1MBPS;
    report->phy_second = GAPM_PHY_TYPE_LE_2M;
    report->adv_sid = 3u;
    report->tx_pwr = 7;
    report->length = 5u;
    report->data[0] = 4u;
    report->data[1] = 0x09u;
    report->data[2] = 'a';
    report->data[3] = 'b';
    report->data[4] = 'c';
    (void)h2_bk3633_platform_ble_dispatch(
        GAPM_EXT_ADV_REPORT_IND, report, TASK_APP, TASK_GAPM);
    assert(s_report_calls == 0u);
    memset(report->data, 0xff, report->length);
    assert(h2_bk3633_platform_ble_dispatch_pending() == H2_PAL_OK);
    assert(s_report_calls == 1u);
    assert(s_report.addr.type == H2_PAL_BLE_ADDR_TYPE_RANDOM);
    assert(s_report.addr.value[0] == 0xaau);
    assert(s_report.rssi == -42);
    assert(s_report.primary_phy == H2_PAL_BLE_PHY_1M);
    assert(s_report.secondary_phy == H2_PAL_BLE_PHY_2M);
    assert(s_report.sid == 3u && s_report.tx_power == 7);
    assert(s_report.data_status == H2_PAL_BLE_ADV_DATA_COMPLETE);
    assert(s_report.local_name_len == 3u);
    assert(s_report_data[2] == 'a' && s_report_data[4] == 'c');

    for (size_t i = 0u; i < 5u; ++i) {
        report->rssi = (int8_t)(-50 + (int)i);
        (void)h2_bk3633_platform_ble_dispatch(
            GAPM_EXT_ADV_REPORT_IND, report, TASK_APP, TASK_GAPM);
    }
    assert(s_report_calls == 1u);
    assert(h2_bk3633_platform_ble_dispatch_pending() == H2_PAL_OK);
    assert(s_report_calls == 5u);
    assert(s_report.rssi == -46);

    (void)h2_bk3633_platform_ble_dispatch(
        GAPM_EXT_ADV_REPORT_IND, report, TASK_APP, TASK_GAPM);
    assert(h2_pal_ble_stop_scan(api) == H2_PAL_OK);
    assert(h2_bk3633_platform_ble_dispatch_pending() == H2_PAL_OK);
    assert(s_report_calls == 5u);
    const h2_bk3633_ble_sdk_fake_message_t *stop =
        last_message_with_id(GAPM_ACTIVITY_STOP_CMD);
    assert(stop != NULL);
    assert(((const struct gapm_activity_stop_cmd *)stop->payload)->actv_idx ==
           5u);
    return 0;
}
