#include "h2_bk3633_platform_core.h"
#include "h2/pal/net/h2_pal_netif.h"
#include "h2/pal/h2_pal_unsupported.h"

#include <stdint.h>

#define PROBE_EVENT_QUEUE_CAPACITY 8u

static int s_handler_calls;
static int s_netif_handler_calls;

static int netif_event_handler(
    void *user,
    const h2_pal_system_event_t *event)
{
    (void)user;
    if (event == NULL ||
        event->type != H2_PAL_SYSTEM_EVENT_TYPE_NETIF_DEFAULT_CHANGED) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    s_netif_handler_calls++;
    return H2_PAL_OK;
}

h2_pal_result_t h2_bk3633_platform_ble_dispatch_pending(void)
{
    return H2_PAL_OK;
}

static int system_event_handler(
    void *user,
    const h2_pal_system_event_t *event)
{
    const uint32_t *expected_payload = (const uint32_t *)user;
    if (event == NULL ||
        event->type != H2_PAL_SYSTEM_EVENT_TYPE_BLE_CONNECTED ||
        event->payload == NULL ||
        event->payload_size != sizeof(*expected_payload) ||
        *(const uint32_t *)event->payload != *expected_payload) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    s_handler_calls++;
    return H2_PAL_OK;
}

int main(void)
{
    const h2_pal_system_event_api_t *api =
        h2_bk3633_platform_system_event_api();
    const uint32_t payload = 0x3633u;
    const uint8_t oversized_payload[
        H2_BK3633_SYSTEM_EVENT_PAYLOAD_MAX + 1u] = {0};
    const h2_pal_system_event_t event = {
        .type = H2_PAL_SYSTEM_EVENT_TYPE_BLE_CONNECTED,
        .payload = &payload,
        .payload_size = sizeof(payload),
    };
    h2_pal_system_event_subscription_t *subscription = NULL;
    h2_pal_system_event_subscription_t *netif_subscription = NULL;

    h2_pal_netif_ref_t default_ref = h2_pal_netif_default_ref();
    h2_pal_netif_status_t netif_status;
    if (h2_pal_netif_get_status(
            h2_pal_unsupported_netif_api(),
            &default_ref,
            &netif_status) != H2_PAL_ERR_UNSUPPORTED) {
        return 10;
    }

    if (h2_pal_system_event_post(api, &event, 0u) !=
        H2_PAL_ERR_INVALID_STATE) {
        return 1;
    }
    if (h2_pal_system_event_init(api) != H2_PAL_OK) {
        return 2;
    }
    if (h2_pal_system_event_subscribe(
            api,
            H2_PAL_SYSTEM_EVENT_TYPE_BLE_CONNECTED,
            system_event_handler,
            (void *)&payload,
            &subscription) != H2_PAL_OK ||
        subscription == NULL) {
        return 3;
    }
    if (h2_pal_system_event_subscribe(
            api,
            H2_PAL_SYSTEM_EVENT_TYPE_NETIF_DEFAULT_CHANGED,
            netif_event_handler,
            NULL,
            &netif_subscription) != H2_PAL_OK ||
        netif_subscription == NULL) {
        return 11;
    }
    if (h2_pal_system_event_post(api, &event, 0u) != H2_PAL_OK ||
        s_handler_calls != 0 ||
        h2_bk3633_platform_system_event_dispatch_pending() != H2_PAL_OK ||
        s_handler_calls != 1) {
        return 4;
    }
    const h2_pal_system_event_t oversized_event = {
        .type = H2_PAL_SYSTEM_EVENT_TYPE_BLE_CONNECTED,
        .payload = oversized_payload,
        .payload_size = sizeof(oversized_payload),
    };
    if (h2_pal_system_event_post(api, &event, 0u) != H2_PAL_OK ||
        h2_pal_system_event_post(api, &oversized_event, 0u) !=
            H2_PAL_ERR_NO_SPACE ||
        h2_pal_system_event_post(api, &event, 0u) !=
            H2_PAL_ERR_NO_SPACE ||
        h2_bk3633_platform_system_event_dispatch_pending() !=
            H2_PAL_ERR_NO_SPACE ||
        s_handler_calls != 2) {
        return 5;
    }
    for (size_t i = 0u; i < PROBE_EVENT_QUEUE_CAPACITY; ++i) {
        if (h2_pal_system_event_post(api, &event, 0u) != H2_PAL_OK) {
            return 6;
        }
    }
    if (h2_pal_system_event_post(api, &event, 0u) != H2_PAL_ERR_FULL ||
        h2_pal_system_event_post(api, &oversized_event, 0u) !=
            H2_PAL_ERR_FULL ||
        h2_bk3633_platform_system_event_dispatch_pending() !=
            H2_PAL_ERR_FULL ||
        s_handler_calls != 2 + PROBE_EVENT_QUEUE_CAPACITY) {
        return 7;
    }
    h2_pal_system_event_unsubscribe(api, netif_subscription);
    h2_pal_system_event_unsubscribe(api, subscription);
    if (h2_pal_system_event_post(api, &event, 0u) != H2_PAL_OK ||
        h2_bk3633_platform_system_event_dispatch_pending() != H2_PAL_OK ||
        s_handler_calls != 2 + PROBE_EVENT_QUEUE_CAPACITY ||
        s_netif_handler_calls != 0) {
        return 8;
    }
    h2_pal_system_event_deinit(api);
    if (h2_pal_system_event_post(api, &event, 0u) !=
        H2_PAL_ERR_INVALID_STATE) {
        return 9;
    }
    return 0;
}
