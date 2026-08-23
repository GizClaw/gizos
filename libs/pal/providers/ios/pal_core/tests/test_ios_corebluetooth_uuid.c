#include "h2_ios_corebluetooth_internal.h"

#include <assert.h>
#include <string.h>

static void check_round_trip(const uint8_t *wire, size_t len) {
    uint8_t platform[16] = { 0 };
    uint8_t round_trip[16] = { 0 };
    const h2_pal_ble_uuid_t uuid = { wire, len };
    assert(h2_ios_corebluetooth_uuid_to_platform(
        &uuid, platform, sizeof(platform)) == H2_PAL_OK);
    assert(h2_ios_corebluetooth_uuid_from_platform(
        platform, len, round_trip, sizeof(round_trip)) == H2_PAL_OK);
    assert(memcmp(wire, round_trip, len) == 0);
}

void h2_ios_test_corebluetooth_uuid(void) {
    static const uint8_t fee0[] = { 0xe0u, 0xfeu };
    static const uint8_t fee1[] = { 0xe1u, 0xfeu };
    static const uint8_t fee2[] = { 0xe2u, 0xfeu };
    static const uint8_t cccd[] = { 0x02u, 0x29u };
    static const uint8_t service128[] = {
        0x65u, 0x63u, 0x69u, 0x76u, 0x72u, 0x65u, 0x73u, 0x2fu,
        0x70u, 0x63u, 0x6bu, 0x69u, 0x65u, 0x6cu, 0x62u, 0x2fu,
    };
    check_round_trip(fee0, sizeof(fee0));
    check_round_trip(fee1, sizeof(fee1));
    check_round_trip(fee2, sizeof(fee2));
    check_round_trip(cccd, sizeof(cccd));
    check_round_trip(service128, sizeof(service128));
    h2_pal_ble_adv_params_t adv = {
        .type = H2_PAL_BLE_ADV_TYPE_LEGACY,
    };
    assert(h2_ios_corebluetooth_adv_params_supported(&adv));
    adv.type = H2_PAL_BLE_ADV_TYPE_EXTENDED;
    assert(!h2_ios_corebluetooth_adv_params_supported(&adv));
    assert(!h2_ios_corebluetooth_adv_params_supported(NULL));
    h2_pal_ble_scan_params_t scan = {
        .type = H2_PAL_BLE_SCAN_TYPE_LEGACY,
    };
    assert(h2_ios_corebluetooth_scan_params_supported(&scan));
    scan.type = H2_PAL_BLE_SCAN_TYPE_EXTENDED;
    assert(!h2_ios_corebluetooth_scan_params_supported(&scan));
    assert(!h2_ios_corebluetooth_scan_params_supported(NULL));
}
