#ifndef H2_BLE_WIFI_CONFIG_INTERNAL_H
#define H2_BLE_WIFI_CONFIG_INTERNAL_H

#include "h2_ble_wifi_config.h"

#include <stdbool.h>

/** Longest UUID the BLE PAL accepts, in bytes. */
#define H2_BLE_WIFI_CONFIG_UUID_MAX_LEN 16u
/**
 * System events the service subscribes to: the BLE link, plus the Wi-Fi
 * station transitions it forwards to the peer as provisioning progress.
 */
#define H2_BLE_WIFI_CONFIG_SUBSCRIPTION_COUNT 7u
/**
 * Progress frames waiting for the worker task. A station transition is
 * advisory, so overflow drops the oldest rather than stalling the runtime
 * callback that produced it.
 */
#define H2_BLE_WIFI_CONFIG_PROGRESS_QUEUE_LEN 4u
/**
 * Pending notifications waiting for the worker task. Overflow drops the
 * oldest entry and is counted, because a lost notification must never stall
 * the BLE Host callback that produced it.
 */
#define H2_BLE_WIFI_CONFIG_EVENT_QUEUE_LEN 8u
/** Smallest worker stack, in bytes. */
#define H2_BLE_WIFI_CONFIG_MIN_STACK_SIZE (4u * 1024u)

/**
 * Identity of one adopted connection.
 *
 * Controllers reuse connection handles, so a handle alone cannot tell the
 * peer that started a scan from a later peer that reused its handle. The
 * generation counter, bumped on every adoption, makes the pair unique for
 * the life of the service.
 */
typedef struct h2_ble_wifi_config_peer {
    uint16_t conn_handle;
    uint32_t generation;
} h2_ble_wifi_config_peer_t;

/** Connection transitions deferred while a notification is in flight. */
#define H2_BLE_WIFI_CONFIG_TRANSITION_QUEUE_LEN 8u

typedef struct h2_ble_wifi_config_transition {
    h2_pal_system_event_type_t type;
    union {
        h2_pal_ble_connection_t connection;
        h2_pal_ble_mtu_info_t mtu;
        h2_pal_ble_subscription_state_t subscription;
        h2_pal_ble_disconnected_info_t disconnected;
    } payload;
} h2_ble_wifi_config_transition_t;

/**
 * One queued station transition.
 *
 * The peer is captured when the transition is queued, not when the worker
 * drains it: a connect attempt outlives the write that started it, so a frame
 * drained after a reconnect would otherwise reach the replacement peer.
 */
typedef struct h2_ble_wifi_config_progress_entry {
    uint8_t state;
    h2_ble_wifi_config_peer_t peer;
} h2_ble_wifi_config_progress_entry_t;

typedef struct h2_ble_wifi_config_pending_event {
    h2_ble_wifi_config_event_t event;
    uint16_t conn_handle;
    int status;
} h2_ble_wifi_config_pending_event_t;

struct h2_ble_wifi_config {
    h2_ble_wifi_config_api_t api;
    h2_ble_wifi_config_config_t config;
    uint8_t service_uuid[H2_BLE_WIFI_CONFIG_UUID_MAX_LEN];
    uint8_t command_uuid[H2_BLE_WIFI_CONFIG_UUID_MAX_LEN];
    uint8_t scan_uuid[H2_BLE_WIFI_CONFIG_UUID_MAX_LEN];
    uint8_t provision_uuid[H2_BLE_WIFI_CONFIG_UUID_MAX_LEN];

    h2_pal_mutex_t *mutex;
    h2_pal_cond_t *cond;
    h2_pal_task_t *worker;
    h2_pal_system_event_subscription_t
        *subscriptions[H2_BLE_WIFI_CONFIG_SUBSCRIPTION_COUNT];

    h2_pal_ble_gatt_characteristic_t
        characteristics[H2_BLE_WIFI_CONFIG_CHARACTERISTIC_COUNT];
    h2_pal_ble_gatt_service_t service;
    uint16_t service_handle;
    uint16_t command_value_handle;
    uint16_t scan_value_handle;
    uint16_t scan_cccd_handle;
    uint16_t provision_value_handle;
    uint16_t provision_cccd_handle;

    /* Connection state, owned by the mutex. */
    uint16_t conn_handle;
    uint32_t conn_generation;
    uint16_t att_mtu;
    bool scan_subscribed;
    bool provision_subscribed;

    /* Advertising window, owned by the mutex. */
    h2_pal_ble_adv_params_t adv_params;
    bool adv_started;
    bool adv_paused;

    /* Worker requests, owned by the mutex. */
    bool scan_requested;
    bool scan_running;
    bool scan_stop_requested;
    bool credentials_pending;
    bool credentials_running;
    h2_ble_wifi_config_credentials_t credentials;
    bool reject_pending;
    /*
     * Whether the running attempt still accepts station transitions. It is
     * closed before the queue is drained, so nothing can be queued after the
     * drain and then be stranded behind the final frame.
     */
    bool progress_open;
    /* Station transitions to forward as progress frames; see the worker. */
    h2_ble_wifi_config_progress_entry_t
        progress_queue[H2_BLE_WIFI_CONFIG_PROGRESS_QUEUE_LEN];
    size_t progress_head;
    size_t progress_count;
    h2_ble_wifi_config_reason_t reject_reason;
    /** Connection whose write was rejected; the result goes only to it. */
    h2_ble_wifi_config_peer_t reject_peer;

    h2_ble_wifi_config_pending_event_t
        events[H2_BLE_WIFI_CONFIG_EVENT_QUEUE_LEN];
    size_t event_head;
    size_t event_count;

    /*
     * A notification is in flight. The BLE Host call runs without the mutex,
     * so connection transitions that arrive meanwhile are queued instead of
     * applied: the send has already validated its peer, and applying a
     * reconnect underneath it would let the frame reach the new peer.
     */
    bool sending;
    h2_ble_wifi_config_transition_t
        transitions[H2_BLE_WIFI_CONFIG_TRANSITION_QUEUE_LEN];
    size_t transition_head;
    size_t transition_count;
    /** A transition was dropped; the connection is treated as lost. */
    bool transition_overflow;

    bool gatt_registered;
    bool closing;
    h2_ble_wifi_config_stats_t stats;
};

#endif
