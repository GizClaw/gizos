#include "h2_esp_platform_core.h"

#include "esp_event.h"
#include "freertos/FreeRTOS.h"

#include <stdlib.h>
#include <string.h>

#define H2_ESP_SYSTEM_EVENT_MAX_SUBSCRIPTIONS 64u

ESP_EVENT_DEFINE_BASE(H2_ESP_SYSTEM_EVENT_BASE);

typedef struct h2_esp_system_event_envelope {
    uint32_t source_id;
    uint64_t timestamp_ms;
    size_t payload_size;
    uint8_t payload[];
} h2_esp_system_event_envelope_t;

struct h2_pal_system_event_subscription {
    int active;
    h2_pal_system_event_type_t type;
    h2_pal_system_event_handler_t handler;
    void *handler_user;
    esp_event_handler_instance_t instance;
};

static h2_pal_system_event_subscription_t
    s_h2_esp_system_event_subscriptions[H2_ESP_SYSTEM_EVENT_MAX_SUBSCRIPTIONS];
static int s_h2_esp_system_event_initialized;

static h2_pal_result_t h2_esp_system_event_map_error(esp_err_t err) {
    switch (err) {
    case ESP_OK:
        return H2_PAL_OK;
    case ESP_ERR_INVALID_ARG:
        return H2_PAL_ERR_INVALID_ARG;
    case ESP_ERR_NO_MEM:
        return H2_PAL_ERR_NO_MEMORY;
    case ESP_ERR_TIMEOUT:
        return H2_PAL_ERR_TIMEOUT;
    default:
        return H2_PAL_ERR_IO;
    }
}

static void h2_esp_system_event_dispatch(
    void *arg,
    esp_event_base_t event_base,
    int32_t event_id,
    void *event_data) {
    (void)event_base;

    h2_pal_system_event_subscription_t *sub = (h2_pal_system_event_subscription_t *)arg;
    const h2_esp_system_event_envelope_t *envelope = (const h2_esp_system_event_envelope_t *)event_data;
    if (sub == NULL || sub->active == 0 || sub->handler == NULL || envelope == NULL) {
        return;
    }

    h2_pal_system_event_t event;
    memset(&event, 0, sizeof(event));
    event.type = (h2_pal_system_event_type_t)event_id;
    event.source_id = envelope->source_id;
    event.timestamp_ms = envelope->timestamp_ms;
    event.payload_size = envelope->payload_size;
    event.payload = envelope->payload_size > 0u ? envelope->payload : NULL;
    (void)sub->handler(sub->handler_user, &event);
}

static int h2_esp_system_event_init(void *user) {
    (void)user;
    if (s_h2_esp_system_event_initialized != 0) {
        return H2_PAL_OK;
    }

    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return h2_esp_system_event_map_error(err);
    }
    s_h2_esp_system_event_initialized = 1;
    int rc = h2_esp_platform_netif_monitor_init();
    if (rc != H2_PAL_OK) {
        s_h2_esp_system_event_initialized = 0;
    }
    return rc;
}

static void h2_esp_system_event_deinit(void *user) {
    (void)user;

    h2_esp_platform_netif_monitor_deinit();
    for (size_t i = 0u; i < H2_ESP_SYSTEM_EVENT_MAX_SUBSCRIPTIONS; ++i) {
        h2_pal_system_event_subscription_t *sub = &s_h2_esp_system_event_subscriptions[i];
        if (sub->active != 0) {
            (void)esp_event_handler_instance_unregister(
                H2_ESP_SYSTEM_EVENT_BASE,
                (int32_t)sub->type,
                sub->instance);
            memset(sub, 0, sizeof(*sub));
        }
    }

    /*
     * The ESP default event loop is shared with ESP-IDF WiFi/IP and may have
     * been created outside this implementation, so deinit only removes H2
     * subscriptions and intentionally leaves the default loop alive.
     */
    s_h2_esp_system_event_initialized = 0;
}

static int h2_esp_system_event_post(
    void *user,
    const h2_pal_system_event_t *event,
    uint32_t timeout_ms) {
    (void)user;

    int rc = h2_pal_system_event_validate(event);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    if (s_h2_esp_system_event_initialized == 0) {
        return H2_PAL_ERR_INVALID_STATE;
    }

    size_t envelope_size = sizeof(h2_esp_system_event_envelope_t) + event->payload_size;
    h2_esp_system_event_envelope_t *envelope = (h2_esp_system_event_envelope_t *)malloc(envelope_size);
    if (envelope == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    memset(envelope, 0, sizeof(*envelope));
    envelope->source_id = event->source_id;
    envelope->timestamp_ms = event->timestamp_ms;
    envelope->payload_size = event->payload_size;
    if (event->payload_size > 0u) {
        memcpy(envelope->payload, event->payload, event->payload_size);
    }

    TickType_t ticks = timeout_ms == 0u ? 0u : pdMS_TO_TICKS(timeout_ms);
    rc = h2_esp_system_event_map_error(esp_event_post(
        H2_ESP_SYSTEM_EVENT_BASE,
        (int32_t)event->type,
        envelope,
        envelope_size,
        ticks));
    free(envelope);
    return rc;
}

static int h2_esp_system_event_subscribe(
    void *user,
    h2_pal_system_event_type_t type,
    h2_pal_system_event_handler_t handler,
    void *handler_user,
    h2_pal_system_event_subscription_t **out_subscription) {
    (void)user;
    if (type <= H2_PAL_SYSTEM_EVENT_TYPE_NONE ||
        type >= H2_PAL_SYSTEM_EVENT_TYPE_COUNT ||
        handler == NULL ||
        out_subscription == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (s_h2_esp_system_event_initialized == 0) {
        return H2_PAL_ERR_INVALID_STATE;
    }

    *out_subscription = NULL;
    for (size_t i = 0u; i < H2_ESP_SYSTEM_EVENT_MAX_SUBSCRIPTIONS; ++i) {
        h2_pal_system_event_subscription_t *sub = &s_h2_esp_system_event_subscriptions[i];
        if (sub->active == 0) {
            memset(sub, 0, sizeof(*sub));
            sub->type = type;
            sub->handler = handler;
            sub->handler_user = handler_user;
            int rc = h2_esp_system_event_map_error(esp_event_handler_instance_register(
                H2_ESP_SYSTEM_EVENT_BASE,
                (int32_t)type,
                h2_esp_system_event_dispatch,
                sub,
                &sub->instance));
            if (rc != H2_PAL_OK) {
                memset(sub, 0, sizeof(*sub));
                return rc;
            }
            sub->active = 1;
            *out_subscription = sub;
            return H2_PAL_OK;
        }
    }
    return H2_PAL_ERR_FULL;
}

static void h2_esp_system_event_unsubscribe(
    void *user,
    h2_pal_system_event_subscription_t *subscription) {
    (void)user;
    if (subscription == NULL || subscription->active == 0) {
        return;
    }
    (void)esp_event_handler_instance_unregister(
        H2_ESP_SYSTEM_EVENT_BASE,
        (int32_t)subscription->type,
        subscription->instance);
    memset(subscription, 0, sizeof(*subscription));
}

static const h2_pal_system_event_vtable_t s_h2_esp_system_event_api_vtable = {
    .init = h2_esp_system_event_init,
    .deinit = h2_esp_system_event_deinit,
    .post = h2_esp_system_event_post,
    .subscribe = h2_esp_system_event_subscribe,
    .unsubscribe = h2_esp_system_event_unsubscribe,
};
static const h2_pal_system_event_api_t s_h2_esp_system_event_api = {
    .user = NULL,
    .vtable = &s_h2_esp_system_event_api_vtable,
};

const h2_pal_system_event_api_t *h2_esp_platform_system_event_api(void) {
    return &s_h2_esp_system_event_api;
}
