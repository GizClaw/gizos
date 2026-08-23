#include "h2_bk_ble_gatts_tx_tracker.h"

#include <assert.h>

int main(void) {
    h2_bk_ble_gatts_tx_tracker_t tracker = {0};
    h2_bk_ble_gatts_tx_tracker_clear(&tracker);

    assert(h2_bk_ble_gatts_tx_tracker_begin(
               &tracker, H2_BK_BLE_GATTS_TX_NOTIFY, 2u, 4u) == H2_PAL_OK);
    assert(h2_bk_ble_gatts_tx_tracker_finish(
               &tracker, 2u, H2_PAL_OK) == H2_BK_BLE_GATTS_TX_NOTIFY);

    assert(h2_bk_ble_gatts_tx_tracker_begin(
               &tracker, H2_BK_BLE_GATTS_TX_INDICATE_WAITING, 3u, 5u) ==
           H2_PAL_OK);
    assert(h2_bk_ble_gatts_tx_tracker_begin(
               &tracker, H2_BK_BLE_GATTS_TX_INDICATE_WAITING, 3u, 6u) ==
           H2_PAL_ERR_BUSY);
    assert(h2_bk_ble_gatts_tx_tracker_finish(
               &tracker, 3u, H2_PAL_ERR_IO) ==
           H2_BK_BLE_GATTS_TX_INDICATE_WAITING);
    assert(h2_bk_ble_gatts_tx_tracker_take(&tracker) == H2_PAL_ERR_IO);

    assert(h2_bk_ble_gatts_tx_tracker_begin(
               &tracker, H2_BK_BLE_GATTS_TX_INDICATE_WAITING, 7u, 9u) ==
           H2_PAL_OK);
    assert(h2_bk_ble_gatts_tx_tracker_timeout(&tracker) ==
           H2_PAL_ERR_TIMEOUT);
    assert(h2_bk_ble_gatts_tx_tracker_begin(
               &tracker, H2_BK_BLE_GATTS_TX_INDICATE_WAITING, 8u, 10u) ==
           H2_PAL_ERR_BUSY);
    assert(h2_bk_ble_gatts_tx_tracker_finish(
               &tracker, 7u, H2_PAL_OK) == H2_BK_BLE_GATTS_TX_IDLE);
    assert(h2_bk_ble_gatts_tx_tracker_begin(
               &tracker, H2_BK_BLE_GATTS_TX_INDICATE_WAITING, 8u, 10u) ==
           H2_PAL_OK);
    h2_bk_ble_gatts_tx_tracker_cancel_submission(&tracker, 8u, 10u);
    assert(tracker.kind == H2_BK_BLE_GATTS_TX_IDLE);
    return 0;
}
