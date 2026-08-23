#ifndef H2_PAL_SYSTEM_EVENT_H
#define H2_PAL_SYSTEM_EVENT_H

#include "h2/pal/core/h2_pal_errors.h"
#include "h2/pal/hal/h2_pal_gpio_irq.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum h2_pal_system_event_type {
    H2_PAL_SYSTEM_EVENT_TYPE_NONE = 0,

    H2_PAL_SYSTEM_EVENT_TYPE_GPIO_IRQ,

    H2_PAL_SYSTEM_EVENT_TYPE_WIFI_STA_CONNECTING,
    H2_PAL_SYSTEM_EVENT_TYPE_WIFI_STA_CONNECTED,
    H2_PAL_SYSTEM_EVENT_TYPE_WIFI_STA_DISCONNECTED,
    H2_PAL_SYSTEM_EVENT_TYPE_WIFI_STA_GOT_IP,
    H2_PAL_SYSTEM_EVENT_TYPE_WIFI_STA_LOST_IP,
    H2_PAL_SYSTEM_EVENT_TYPE_WIFI_AP_STARTED,
    H2_PAL_SYSTEM_EVENT_TYPE_WIFI_AP_STOPPED,
    H2_PAL_SYSTEM_EVENT_TYPE_WIFI_AP_CLIENT_JOINED,
    H2_PAL_SYSTEM_EVENT_TYPE_WIFI_AP_CLIENT_LEFT,
    H2_PAL_SYSTEM_EVENT_TYPE_WIFI_AP_LEASE_GRANTED,
    H2_PAL_SYSTEM_EVENT_TYPE_WIFI_AP_LEASE_RELEASED,

    H2_PAL_SYSTEM_EVENT_TYPE_BLE_HOST_STARTED,
    H2_PAL_SYSTEM_EVENT_TYPE_BLE_HOST_STOPPED,
    H2_PAL_SYSTEM_EVENT_TYPE_BLE_ADVERTISING_STARTED,
    H2_PAL_SYSTEM_EVENT_TYPE_BLE_ADVERTISING_STOPPED,
    H2_PAL_SYSTEM_EVENT_TYPE_BLE_SCAN_STARTED,
    H2_PAL_SYSTEM_EVENT_TYPE_BLE_SCAN_STOPPED,
    H2_PAL_SYSTEM_EVENT_TYPE_BLE_CONNECTED,
    H2_PAL_SYSTEM_EVENT_TYPE_BLE_CONNECTION_UPDATED,
    H2_PAL_SYSTEM_EVENT_TYPE_BLE_DISCONNECTED,
    H2_PAL_SYSTEM_EVENT_TYPE_BLE_MTU_CHANGED,
    H2_PAL_SYSTEM_EVENT_TYPE_BLE_SUBSCRIPTION_CHANGED,
    H2_PAL_SYSTEM_EVENT_TYPE_BLE_GATT_CLIENT_NOTIFICATION,
    H2_PAL_SYSTEM_EVENT_TYPE_BLE_GATT_CLIENT_INDICATION,
    H2_PAL_SYSTEM_EVENT_TYPE_MODEM_READY,
    H2_PAL_SYSTEM_EVENT_TYPE_MODEM_ERROR,
    H2_PAL_SYSTEM_EVENT_TYPE_MODEM_SIM_CHANGED,
    H2_PAL_SYSTEM_EVENT_TYPE_MODEM_REGISTRATION_CHANGED,
    H2_PAL_SYSTEM_EVENT_TYPE_MODEM_PACKET_CHANGED,
    H2_PAL_SYSTEM_EVENT_TYPE_MODEM_SIGNAL_CHANGED,
    H2_PAL_SYSTEM_EVENT_TYPE_MODEM_DATA_OPENED,
    H2_PAL_SYSTEM_EVENT_TYPE_MODEM_DATA_CLOSED,
    H2_PAL_SYSTEM_EVENT_TYPE_MODEM_CALL_INCOMING,
    H2_PAL_SYSTEM_EVENT_TYPE_MODEM_CALL_STATE_CHANGED,
    H2_PAL_SYSTEM_EVENT_TYPE_MODEM_CALL_ENDED,

    H2_PAL_SYSTEM_EVENT_TYPE_NETIF_DEFAULT_CHANGED,

    H2_PAL_SYSTEM_EVENT_TYPE_COUNT,
} h2_pal_system_event_type_t;

typedef struct h2_pal_system_event {
    h2_pal_system_event_type_t type;
    uint32_t source_id;
    uint64_t timestamp_ms;
    const void *payload;
    size_t payload_size;
} h2_pal_system_event_t;

typedef struct h2_pal_system_event_subscription h2_pal_system_event_subscription_t;

typedef int (*h2_pal_system_event_handler_t)(
    void *user,
    const h2_pal_system_event_t *event);

/*
 * System events are low-level platform capability events. init prepares or
 * attaches to the target-owned event runtime. post is intended for platform
 * backend code to emit those events; application/reducer events belong above
 * this layer. deinit releases only resources owned by this API instance and
 * must not tear down shared platform/default loops owned by the board runtime.
 */
typedef struct h2_pal_system_event_vtable {
    int (*init)(void *user);
    void (*deinit)(void *user);
    int (*post)(void *user, const h2_pal_system_event_t *event, uint32_t timeout_ms);
    int (*subscribe)(
        void *user,
        h2_pal_system_event_type_t type,
        h2_pal_system_event_handler_t handler,
        void *handler_user,
        h2_pal_system_event_subscription_t **out_subscription);
    void (*unsubscribe)(void *user, h2_pal_system_event_subscription_t *subscription);
} h2_pal_system_event_vtable_t;

typedef struct h2_pal_system_event_api {
    void *user;
    const h2_pal_system_event_vtable_t *vtable;
} h2_pal_system_event_api_t;

static inline int h2_pal_system_event_init(const h2_pal_system_event_api_t *api) {
    if (api == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api->vtable == NULL || api->vtable->init == NULL) {
        return H2_PAL_OK;
    }
    return api->vtable->init(api->user);
}

static inline void h2_pal_system_event_deinit(const h2_pal_system_event_api_t *api) {
    if (api == NULL || api->vtable == NULL || api->vtable->deinit == NULL) {
        return;
    }
    api->vtable->deinit(api->user);
}

static inline int h2_pal_system_event_validate(const h2_pal_system_event_t *event) {
    if (event == NULL ||
        event->type <= H2_PAL_SYSTEM_EVENT_TYPE_NONE ||
        event->type >= H2_PAL_SYSTEM_EVENT_TYPE_COUNT ||
        (event->payload_size > 0u && event->payload == NULL)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    return H2_PAL_OK;
}

static inline int h2_pal_system_event_post(
    const h2_pal_system_event_api_t *api,
    const h2_pal_system_event_t *event,
    uint32_t timeout_ms) {
    if (api == NULL || api->vtable == NULL || api->vtable->post == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    int rc = h2_pal_system_event_validate(event);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    return api->vtable->post(api->user, event, timeout_ms);
}

static inline int h2_pal_system_event_subscribe(
    const h2_pal_system_event_api_t *api,
    h2_pal_system_event_type_t type,
    h2_pal_system_event_handler_t handler,
    void *handler_user,
    h2_pal_system_event_subscription_t **out_subscription) {
    if (api == NULL || api->vtable == NULL || api->vtable->subscribe == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    if (type <= H2_PAL_SYSTEM_EVENT_TYPE_NONE ||
        type >= H2_PAL_SYSTEM_EVENT_TYPE_COUNT ||
        handler == NULL ||
        out_subscription == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_subscription = NULL;
    return api->vtable->subscribe(api->user, type, handler, handler_user, out_subscription);
}

static inline void h2_pal_system_event_unsubscribe(
    const h2_pal_system_event_api_t *api,
    h2_pal_system_event_subscription_t *subscription) {
    if (api == NULL || api->vtable == NULL || api->vtable->unsubscribe == NULL ||
        subscription == NULL) {
        return;
    }
    api->vtable->unsubscribe(api->user, subscription);
}

#ifdef __cplusplus
}
#endif

#endif
