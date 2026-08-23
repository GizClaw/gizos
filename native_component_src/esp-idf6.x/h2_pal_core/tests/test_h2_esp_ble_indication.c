#include "h2_esp_ble_indication_tracker.h"

#include <assert.h>

int main(void) {
    h2_esp_ble_indication_tracker_t tracker = {0};
    h2_esp_ble_indication_tracker_clear(&tracker);

    assert(h2_esp_ble_indication_tracker_begin(&tracker, 3u, 5u) ==
           H2_PAL_OK);
    assert(h2_esp_ble_indication_tracker_begin(&tracker, 3u, 6u) ==
           H2_PAL_ERR_BUSY);
    assert(!h2_esp_ble_indication_tracker_finish(
        &tracker, 4u, 5u, true, H2_PAL_OK));
    assert(h2_esp_ble_indication_tracker_finish(
        &tracker, 3u, 5u, true, H2_PAL_OK));
    assert(h2_esp_ble_indication_tracker_take(&tracker) == H2_PAL_OK);

    assert(h2_esp_ble_indication_tracker_begin(&tracker, 7u, 9u) ==
           H2_PAL_OK);
    assert(h2_esp_ble_indication_tracker_timeout(&tracker) ==
           H2_PAL_ERR_TIMEOUT);
    assert(h2_esp_ble_indication_tracker_begin(&tracker, 7u, 9u) ==
           H2_PAL_ERR_BUSY);
    assert(!h2_esp_ble_indication_tracker_finish(
        &tracker, 7u, 9u, true, H2_PAL_OK));
    assert(h2_esp_ble_indication_tracker_begin(&tracker, 8u, 10u) ==
           H2_PAL_OK);
    h2_esp_ble_indication_tracker_cancel_submission(&tracker, 8u, 10u);
    assert(tracker.state == H2_ESP_BLE_INDICATION_IDLE);
    return 0;
}
