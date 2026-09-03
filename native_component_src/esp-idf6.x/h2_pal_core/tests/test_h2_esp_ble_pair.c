#include "h2_esp_ble_pair_tracker.h"

#include <assert.h>

int main(void) {
    h2_esp_ble_pair_tracker_t tracker;
    h2_esp_ble_pair_tracker_clear(&tracker);

    h2_esp_ble_pair_tracker_begin(&tracker, 7u);
    assert(!h2_esp_ble_pair_tracker_complete(
        &tracker, 8u, H2_PAL_ERR_CLOSED));
    assert(tracker.conn_handle == 7u);
    assert(tracker.result == H2_PAL_ERR_IO);

    assert(h2_esp_ble_pair_tracker_complete(
        &tracker, 7u, H2_PAL_ERR_CLOSED));
    assert(h2_esp_ble_pair_tracker_take(&tracker) == H2_PAL_ERR_CLOSED);
    assert(tracker.conn_handle == H2_PAL_BLE_INVALID_CONN_HANDLE);

    h2_esp_ble_pair_tracker_begin(&tracker, 9u);
    assert(h2_esp_ble_pair_tracker_complete(&tracker, 9u, H2_PAL_OK));
    assert(!h2_esp_ble_pair_tracker_complete(
        &tracker, 9u, H2_PAL_ERR_CLOSED));
    assert(h2_esp_ble_pair_tracker_take(&tracker) == H2_PAL_OK);
    return 0;
}
