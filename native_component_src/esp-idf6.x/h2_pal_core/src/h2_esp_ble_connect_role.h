#ifndef H2_ESP_BLE_CONNECT_ROLE_H
#define H2_ESP_BLE_CONNECT_ROLE_H

#include <stdbool.h>

/**
 * Decide whether one NimBLE BLE_GAP_EVENT_CONNECT belongs to the local
 * central connect() waiter.
 *
 * The Host may advertise and initiate at the same time, so a peer can connect
 * to a local advertising set while connect() is still pending. The pending
 * flag alone would then complete the waiter with the peer's link and mark the
 * incoming link as central. An established link reports its real role in the
 * connection descriptor. A failed event has no descriptor: only the connect()
 * callback, which carries no advertising-set argument, can report it.
 *
 * @param status BLE_GAP_EVENT_CONNECT status; zero means established.
 * @param has_desc Whether ble_gap_conn_find() returned the descriptor.
 * @param desc_central Whether that descriptor's role is BLE_GAP_ROLE_MASTER.
 * @param connect_pending Whether a local connect() is waiting.
 * @param from_adv_set Whether the event arrived through an advertising set.
 */
static inline bool h2_esp_ble_connect_event_is_central(
    int status,
    bool has_desc,
    bool desc_central,
    bool connect_pending,
    bool from_adv_set) {
    if (status == 0 && has_desc) {
        return desc_central;
    }
    return connect_pending && !from_adv_set;
}

#endif
