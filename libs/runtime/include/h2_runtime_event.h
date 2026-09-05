#ifndef H2_RUNTIME_EVENT_H
#define H2_RUNTIME_EVENT_H

/* Scope: Runtime event envelope and delivery API. */

#include "h2/pal/core/h2_pal_errors.h"
#include "h2_runtime_component.h"
#include "h2_runtime_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define H2_RUNTIME_EVENT_PAYLOAD_MAX 640u

typedef enum h2_runtime_event_kind {
    H2_RUNTIME_EVENT_NONE = 0,

    H2_RUNTIME_SYSTEM_EVENT_GPIO_IRQ_TRIGGERED,
    H2_RUNTIME_SYSTEM_EVENT_WIFI_STA_CONNECTING,
    H2_RUNTIME_SYSTEM_EVENT_WIFI_STA_CONNECTED,
    H2_RUNTIME_SYSTEM_EVENT_WIFI_STA_DISCONNECTED,
    H2_RUNTIME_SYSTEM_EVENT_WIFI_STA_GOT_IP,
    H2_RUNTIME_SYSTEM_EVENT_WIFI_STA_LOST_IP,
    H2_RUNTIME_SYSTEM_EVENT_WIFI_AP_STARTED,
    H2_RUNTIME_SYSTEM_EVENT_WIFI_AP_STOPPED,
    H2_RUNTIME_SYSTEM_EVENT_WIFI_AP_CLIENT_JOINED,
    H2_RUNTIME_SYSTEM_EVENT_WIFI_AP_CLIENT_LEFT,
    H2_RUNTIME_SYSTEM_EVENT_WIFI_AP_LEASE_GRANTED,
    H2_RUNTIME_SYSTEM_EVENT_WIFI_AP_LEASE_RELEASED,
    H2_RUNTIME_SYSTEM_EVENT_BLE_HOST_STARTED,
    H2_RUNTIME_SYSTEM_EVENT_BLE_HOST_STOPPED,
    H2_RUNTIME_SYSTEM_EVENT_BLE_ADVERTISING_STARTED,
    H2_RUNTIME_SYSTEM_EVENT_BLE_ADVERTISING_STOPPED,
    H2_RUNTIME_SYSTEM_EVENT_BLE_SCAN_STARTED,
    H2_RUNTIME_SYSTEM_EVENT_BLE_SCAN_STOPPED,
    H2_RUNTIME_SYSTEM_EVENT_BLE_CONNECTED,
    H2_RUNTIME_SYSTEM_EVENT_BLE_CONNECTION_UPDATED,
    H2_RUNTIME_SYSTEM_EVENT_BLE_DISCONNECTED,
    H2_RUNTIME_SYSTEM_EVENT_BLE_MTU_CHANGED,
    H2_RUNTIME_SYSTEM_EVENT_BLE_SUBSCRIPTION_CHANGED,
    H2_RUNTIME_SYSTEM_EVENT_BLE_GATT_CLIENT_NOTIFICATION,
    H2_RUNTIME_SYSTEM_EVENT_BLE_GATT_CLIENT_INDICATION,
    H2_RUNTIME_SYSTEM_EVENT_MODEM_READY,
    H2_RUNTIME_SYSTEM_EVENT_MODEM_ERROR,
    H2_RUNTIME_SYSTEM_EVENT_MODEM_SIM_CHANGED,
    H2_RUNTIME_SYSTEM_EVENT_MODEM_REGISTRATION_CHANGED,
    H2_RUNTIME_SYSTEM_EVENT_MODEM_PACKET_CHANGED,
    H2_RUNTIME_SYSTEM_EVENT_MODEM_SIGNAL_CHANGED,
    H2_RUNTIME_SYSTEM_EVENT_MODEM_DATA_OPENED,
    H2_RUNTIME_SYSTEM_EVENT_MODEM_DATA_CLOSED,
    H2_RUNTIME_SYSTEM_EVENT_MODEM_CALL_INCOMING,
    H2_RUNTIME_SYSTEM_EVENT_MODEM_CALL_STATE_CHANGED,
    H2_RUNTIME_SYSTEM_EVENT_MODEM_CALL_ENDED,
    H2_RUNTIME_SYSTEM_EVENT_NETIF_DEFAULT_CHANGED,

    H2_RUNTIME_COMPONENT_EVENT_BUTTON_ACTION,
    H2_RUNTIME_COMPONENT_EVENT_NFC_STATE,
    H2_RUNTIME_COMPONENT_EVENT_IMU_GESTURE,
    H2_RUNTIME_COMPONENT_EVENT_ERROR,
    H2_RUNTIME_COMPONENT_EVENT_BUTTON_DOWN,
    H2_RUNTIME_COMPONENT_EVENT_BUTTON_UP,

    /*
     * App or library owned event posted with h2_runtime_post_custom_event().
     * The payload is a h2_runtime_custom_event_payload_t; see
     * h2_runtime_custom_event.h.
     */
    H2_RUNTIME_EVENT_CUSTOM,
} h2_runtime_event_kind_t;

typedef struct h2_runtime_event {
    h2_runtime_event_kind_t kind;
    h2_runtime_component_t component;
    h2_runtime_component_id_t component_id;
    h2_runtime_sequence_t sequence;
    h2_runtime_timestamp_ms_t timestamp_ms;

    void *payload;
    size_t payload_capacity;
    size_t payload_size;
} h2_runtime_event_t;

/*
 * poll/wait requires payload to point at a caller-owned buffer with at least
 * H2_RUNTIME_EVENT_PAYLOAD_MAX bytes. The runtime validates the buffer before
 * dequeuing so a too-small buffer cannot consume an event.
 */

h2_pal_result_t h2_runtime_poll_event(
    h2_runtime_t *runtime,
    h2_runtime_event_t *out_event);

/*
 * Blocks until something needs the main loop's attention, without dequeuing
 * anything. Every Runtime producer signals it after enqueuing an event, and
 * libraries with their own dispatch queues signal it with
 * h2_runtime_notify(). Signals are coalesced: any number of them between two
 * waits produce exactly one H2_PAL_OK, and a signal that arrives while the
 * loop is running is kept for the next wait. Returns H2_PAL_ERR_TIMEOUT when
 * nothing was signalled within timeout_ms (H2_PAL_QUEUE_WAIT_FOREVER waits
 * indefinitely), H2_PAL_ERR_CLOSED once deinit has closed the Runtime.
 *
 * The intended loop: wait_notify, then drain the event queue with
 * h2_runtime_poll_event() until it is empty, then drain the library dispatch
 * queues, then wait again.
 */
h2_pal_result_t h2_runtime_wait_notify(
    h2_runtime_t *runtime,
    uint32_t timeout_ms);

/*
 * Convenience for loops that consume one event per iteration:
 * h2_runtime_wait_notify() followed by one h2_runtime_poll_event(). A wake
 * that finds no queued event (an h2_runtime_notify() from a library, or an
 * earlier poll already drained the queue) returns H2_PAL_ERR_TIMEOUT before
 * timeout_ms elapsed, so treat TIMEOUT as "nothing dequeued", never as a
 * measure of elapsed time.
 */
h2_pal_result_t h2_runtime_wait_event(
    h2_runtime_t *runtime,
    h2_runtime_event_t *out_event,
    uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif
