#include "h2_esp_ble_pair_tracker.h"

#include <assert.h>

int main(void) {
    h2_esp_ble_pair_tracker_t tracker = {0};
    h2_esp_ble_pair_tracker_clear(&tracker);

    assert(h2_esp_ble_pair_tracker_begin(&tracker, 7u) == H2_PAL_OK);
    assert(h2_esp_ble_pair_tracker_begin(&tracker, 8u) == H2_PAL_ERR_BUSY);
    assert(!h2_esp_ble_pair_tracker_complete(
        &tracker, 8u, H2_PAL_ERR_CLOSED));
    assert(tracker.conn_handle == 7u);
    assert(tracker.result == H2_PAL_ERR_IO);

    assert(h2_esp_ble_pair_tracker_complete(
        &tracker, 7u, H2_PAL_ERR_CLOSED));
    assert(h2_esp_ble_pair_tracker_take(&tracker) == H2_PAL_ERR_CLOSED);
    assert(tracker.conn_handle == H2_PAL_BLE_INVALID_CONN_HANDLE);

    assert(h2_esp_ble_pair_tracker_begin(&tracker, 9u) == H2_PAL_OK);
    assert(h2_esp_ble_pair_tracker_complete(&tracker, 9u, H2_PAL_OK));
    assert(!h2_esp_ble_pair_tracker_complete(
        &tracker, 9u, H2_PAL_ERR_CLOSED));
    assert(h2_esp_ble_pair_tracker_take(&tracker) == H2_PAL_OK);

    assert(h2_esp_ble_pair_tracker_begin(&tracker, 10u) == H2_PAL_OK);
    assert(h2_esp_ble_pair_tracker_timeout(&tracker) == H2_PAL_ERR_TIMEOUT);
    assert(tracker.state == H2_ESP_BLE_PAIR_DRAINING);
    /* A retry cannot begin while the original SMP callback is still live. */
    assert(h2_esp_ble_pair_tracker_begin(&tracker, 10u) == H2_PAL_ERR_BUSY);
    /* A late completion is ignored; only disconnect releases the handle. */
    assert(!h2_esp_ble_pair_tracker_complete(
        &tracker, 10u, H2_PAL_OK));
    assert(tracker.state == H2_ESP_BLE_PAIR_DRAINING);
    assert(h2_esp_ble_pair_tracker_begin(&tracker, 10u) == H2_PAL_ERR_BUSY);
    assert(!h2_esp_ble_pair_tracker_disconnect(&tracker, 10u));
    assert(tracker.state == H2_ESP_BLE_PAIR_IDLE);

    assert(h2_esp_ble_pair_tracker_begin(&tracker, 10u) == H2_PAL_OK);
    assert(h2_esp_ble_pair_tracker_complete(&tracker, 10u, H2_PAL_OK));
    assert(h2_esp_ble_pair_tracker_timeout(&tracker) == H2_PAL_OK);

    assert(h2_esp_ble_pair_tracker_begin(&tracker, 12u) == H2_PAL_OK);
    h2_esp_ble_pair_tracker_cancel_submission(&tracker, 12u);
    assert(tracker.state == H2_ESP_BLE_PAIR_IDLE);

    assert(h2_esp_ble_pair_tracker_begin(&tracker, 13u) == H2_PAL_OK);
    assert(h2_esp_ble_pair_tracker_disconnect(&tracker, 13u));
    assert(h2_esp_ble_pair_tracker_take(&tracker) == H2_PAL_ERR_CLOSED);
    return 0;
}
