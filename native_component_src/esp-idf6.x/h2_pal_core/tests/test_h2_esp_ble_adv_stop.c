#include "h2_esp_ble_adv_stop.h"

#include <assert.h>

/* NimBLE's BLE_HS_EALREADY; the host test cannot include the SDK header. */
#define TEST_ALREADY_RC 2

int main(void) {
    /*
     * A successful explicit stop ends advertising synchronously and emits no
     * BLE_GAP_EVENT_ADV_COMPLETE, so the caller completes inline. Waiting for
     * that event is what made every explicit stop time out.
     */
    assert(h2_esp_ble_adv_stop_classify(0, TEST_ALREADY_RC) ==
           H2_ESP_BLE_ADV_STOP_COMPLETED);

    /* Not advertising is the same observable end state. */
    assert(h2_esp_ble_adv_stop_classify(TEST_ALREADY_RC, TEST_ALREADY_RC) ==
           H2_ESP_BLE_ADV_STOP_COMPLETED);

    /* Any other SDK failure is reported, not silently treated as stopped. */
    assert(h2_esp_ble_adv_stop_classify(1, TEST_ALREADY_RC) ==
           H2_ESP_BLE_ADV_STOP_FAILED);
    assert(h2_esp_ble_adv_stop_classify(-1, TEST_ALREADY_RC) ==
           H2_ESP_BLE_ADV_STOP_FAILED);
    assert(h2_esp_ble_adv_stop_classify(0x0e, TEST_ALREADY_RC) ==
           H2_ESP_BLE_ADV_STOP_FAILED);
    return 0;
}
