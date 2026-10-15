#ifndef H2_IOS_COREBLUETOOTH_INTERNAL_H
#define H2_IOS_COREBLUETOOTH_INTERNAL_H

#include "h2/pal/hal/h2_pal_ble.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool h2_ios_corebluetooth_adv_params_supported(
    const h2_pal_ble_adv_params_t *params);

bool h2_ios_corebluetooth_scan_params_supported(
    const h2_pal_ble_scan_params_t *params);

int h2_ios_corebluetooth_uuid_to_platform(
    const h2_pal_ble_uuid_t *uuid,
    uint8_t *out,
    size_t out_size);

int h2_ios_corebluetooth_uuid_from_platform(
    const uint8_t *bytes,
    size_t len,
    uint8_t *out,
    size_t out_size);

h2_pal_result_t h2_ios_corebluetooth_readiness_result(
    int central_known,
    int central_ready,
    int peripheral_known,
    int peripheral_ready);

void h2_ios_corebluetooth_test_set_start_result(
    int enabled,
    h2_pal_result_t result);

void h2_ios_corebluetooth_test_post_connected_on_backend_queue(void);

/* Pass NULL to uninstall the fake and reset the backend after the test. */
void h2_ios_corebluetooth_test_set_pending_connect(
    const h2_pal_ble_addr_t *address);
bool h2_ios_corebluetooth_test_connect_pending(void);
bool h2_ios_corebluetooth_test_connect_cleanup(unsigned cancel_count);

enum {
    H2_IOS_COREBLUETOOTH_TEST_CONNECTED,
    H2_IOS_COREBLUETOOTH_TEST_FAILED,
    H2_IOS_COREBLUETOOTH_TEST_DISCONNECTED,
};
/* Peripheral index 0 is the timed-out fake; index 1 is the other connection. */
void h2_ios_corebluetooth_test_deliver_central_event(
    int event, int peripheral_index);
void h2_ios_corebluetooth_test_set_other_connected(void);
bool h2_ios_corebluetooth_test_other_connected(void);

#endif
