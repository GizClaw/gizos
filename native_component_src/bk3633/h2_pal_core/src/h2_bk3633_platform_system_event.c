#include "h2_bk3633_platform_core.h"

#include <stddef.h>
#include <string.h>

#ifndef H2_BK3633_SYSTEM_EVENT_EXTRA_SUBSCRIPTION_CAPACITY
#define H2_BK3633_SYSTEM_EVENT_EXTRA_SUBSCRIPTION_CAPACITY 8u
#endif
#define H2_BK3633_SYSTEM_EVENT_MAX_SUBSCRIPTIONS \
    (H2_PAL_SYSTEM_EVENT_TYPE_COUNT + \
     H2_BK3633_SYSTEM_EVENT_EXTRA_SUBSCRIPTION_CAPACITY)
#define H2_BK3633_SYSTEM_EVENT_QUEUE_CAPACITY 8u

struct h2_pal_system_event_subscription {
    h2_pal_system_event_type_t type;
    h2_pal_system_event_handler_t handler;
    void *handler_user;
};

typedef struct h2_bk3633_system_event_slot {
    h2_pal_system_event_type_t type;
    uint32_t source_id;
    uint64_t timestamp_ms;
    size_t payload_size;
    _Alignas(max_align_t)
        uint8_t payload[H2_BK3633_SYSTEM_EVENT_PAYLOAD_MAX];
} h2_bk3633_system_event_slot_t;

_Static_assert(
    offsetof(h2_bk3633_system_event_slot_t, payload) %
            _Alignof(max_align_t) ==
        0u,
    "BK3633 system-event payload must preserve maximum alignment");

static h2_pal_system_event_subscription_t
    s_system_event_subscriptions[H2_BK3633_SYSTEM_EVENT_MAX_SUBSCRIPTIONS];
static h2_bk3633_system_event_slot_t
    s_system_event_queue[H2_BK3633_SYSTEM_EVENT_QUEUE_CAPACITY];
static size_t s_system_event_head;
static size_t s_system_event_tail;
static size_t s_system_event_count;
static h2_pal_result_t s_system_event_post_error;
static int s_system_event_initialized;

static int system_event_init(void *user)
{
    (void)user;
    memset(s_system_event_subscriptions, 0,
           sizeof(s_system_event_subscriptions));
    memset(s_system_event_queue, 0, sizeof(s_system_event_queue));
    s_system_event_head = 0u;
    s_system_event_tail = 0u;
    s_system_event_count = 0u;
    s_system_event_post_error = H2_PAL_OK;
    s_system_event_initialized = 1;
    return H2_PAL_OK;
}

static void system_event_deinit(void *user)
{
    (void)user;
    memset(s_system_event_subscriptions, 0,
           sizeof(s_system_event_subscriptions));
    memset(s_system_event_queue, 0, sizeof(s_system_event_queue));
    s_system_event_head = 0u;
    s_system_event_tail = 0u;
    s_system_event_count = 0u;
    s_system_event_post_error = H2_PAL_OK;
    s_system_event_initialized = 0;
}

static int system_event_post(
    void *user,
    const h2_pal_system_event_t *event,
    uint32_t timeout_ms)
{
    (void)user;
    (void)timeout_ms;

    int result = h2_pal_system_event_validate(event);
    if (result != H2_PAL_OK) {
        return result;
    }
    if (s_system_event_initialized == 0) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    if (s_system_event_post_error != H2_PAL_OK) {
        return s_system_event_post_error;
    }
    if (event->payload_size > H2_BK3633_SYSTEM_EVENT_PAYLOAD_MAX) {
        s_system_event_post_error = H2_PAL_ERR_NO_SPACE;
        return H2_PAL_ERR_NO_SPACE;
    }
    if (s_system_event_count == H2_BK3633_SYSTEM_EVENT_QUEUE_CAPACITY) {
        s_system_event_post_error = H2_PAL_ERR_FULL;
        return H2_PAL_ERR_FULL;
    }

    h2_bk3633_system_event_slot_t *slot =
        &s_system_event_queue[s_system_event_head];
    slot->type = event->type;
    slot->source_id = event->source_id;
    slot->timestamp_ms = event->timestamp_ms;
    slot->payload_size = event->payload_size;
    if (event->payload_size != 0u) {
        memcpy(slot->payload, event->payload, event->payload_size);
    }
    s_system_event_head =
        (s_system_event_head + 1u) % H2_BK3633_SYSTEM_EVENT_QUEUE_CAPACITY;
    ++s_system_event_count;
    return H2_PAL_OK;
}

static int system_event_subscribe(
    void *user,
    h2_pal_system_event_type_t type,
    h2_pal_system_event_handler_t handler,
    void *handler_user,
    h2_pal_system_event_subscription_t **out_subscription)
{
    (void)user;
    if (type <= H2_PAL_SYSTEM_EVENT_TYPE_NONE ||
        type >= H2_PAL_SYSTEM_EVENT_TYPE_COUNT ||
        handler == NULL ||
        out_subscription == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_subscription = NULL;
    if (s_system_event_initialized == 0) {
        return H2_PAL_ERR_INVALID_STATE;
    }

    for (size_t i = 0u;
         i < H2_BK3633_SYSTEM_EVENT_MAX_SUBSCRIPTIONS;
         ++i) {
        h2_pal_system_event_subscription_t *subscription =
            &s_system_event_subscriptions[i];
        if (subscription->handler == NULL) {
            memset(subscription, 0, sizeof(*subscription));
            subscription->type = type;
            subscription->handler = handler;
            subscription->handler_user = handler_user;
            *out_subscription = subscription;
            return H2_PAL_OK;
        }
    }
    return H2_PAL_ERR_FULL;
}

static void system_event_unsubscribe(
    void *user,
    h2_pal_system_event_subscription_t *subscription)
{
    (void)user;
    if (subscription == NULL) {
        return;
    }
    const uintptr_t begin =
        (uintptr_t)&s_system_event_subscriptions[0];
    const uintptr_t end =
        (uintptr_t)&s_system_event_subscriptions[
            H2_BK3633_SYSTEM_EVENT_MAX_SUBSCRIPTIONS];
    const uintptr_t address = (uintptr_t)subscription;
    if (address >= begin && address < end &&
        (address - begin) % sizeof(*subscription) == 0u) {
        memset(subscription, 0, sizeof(*subscription));
    }
}

const h2_pal_system_event_api_t *h2_bk3633_platform_system_event_api(void)
{
    static const h2_pal_system_event_vtable_t vtable = {
        .init = system_event_init,
        .deinit = system_event_deinit,
        .post = system_event_post,
        .subscribe = system_event_subscribe,
        .unsubscribe = system_event_unsubscribe,
    };
    static const h2_pal_system_event_api_t api = {
        .user = NULL,
        .vtable = &vtable,
    };
    return &api;
}

static h2_pal_result_t system_event_dispatch_one(int *out_dispatched)
{
    *out_dispatched = 0;
    if (s_system_event_count == 0u) {
        return H2_PAL_OK;
    }

    h2_bk3633_system_event_slot_t *slot =
        &s_system_event_queue[s_system_event_tail];
    const h2_pal_system_event_t event = {
        .type = slot->type,
        .source_id = slot->source_id,
        .timestamp_ms = slot->timestamp_ms,
        .payload = slot->payload_size != 0u ? slot->payload : NULL,
        .payload_size = slot->payload_size,
    };
    h2_pal_result_t slot_result = H2_PAL_OK;

    for (size_t i = 0u;
         i < H2_BK3633_SYSTEM_EVENT_MAX_SUBSCRIPTIONS;
         ++i) {
        h2_pal_system_event_subscription_t *subscription =
            &s_system_event_subscriptions[i];
        if (subscription->handler != NULL &&
            subscription->type == event.type) {
            h2_pal_result_t result = subscription->handler(
                subscription->handler_user, &event);
            if (result != H2_PAL_OK) {
                slot_result = result;
                break;
            }
        }
    }

    memset(slot, 0, sizeof(*slot));
    s_system_event_tail =
        (s_system_event_tail + 1u) %
        H2_BK3633_SYSTEM_EVENT_QUEUE_CAPACITY;
    --s_system_event_count;
    *out_dispatched = 1;
    return slot_result;
}

static h2_pal_result_t system_event_dispatch_queue(void)
{
    int dispatched;
    do {
        h2_pal_result_t result = system_event_dispatch_one(&dispatched);
        if (result != H2_PAL_OK) {
            return result;
        }
    } while (dispatched != 0);
    return H2_PAL_OK;
}

h2_pal_result_t h2_bk3633_platform_system_event_dispatch_pending(void)
{
    if (s_system_event_initialized == 0) {
        return H2_PAL_ERR_INVALID_STATE;
    }

    h2_pal_result_t dispatch_result = system_event_dispatch_queue();
    if (dispatch_result != H2_PAL_OK) {
        return dispatch_result;
    }
    if (s_system_event_post_error != H2_PAL_OK) {
        dispatch_result = s_system_event_post_error;
        s_system_event_post_error = H2_PAL_OK;
    }

    h2_pal_result_t ble_result = h2_bk3633_platform_ble_dispatch_pending();
    if (dispatch_result == H2_PAL_OK && ble_result != H2_PAL_OK) {
        dispatch_result = ble_result;
    }
    h2_pal_result_t retry_dispatch_result = system_event_dispatch_queue();
    if (retry_dispatch_result != H2_PAL_OK) {
        return retry_dispatch_result;
    }
    if (s_system_event_post_error != H2_PAL_OK) {
        if (dispatch_result == H2_PAL_OK) {
            dispatch_result = s_system_event_post_error;
        }
        s_system_event_post_error = H2_PAL_OK;
    }
    return dispatch_result;
}

h2_pal_result_t h2_bk3633_platform_system_event_dispatch_next(
    bool *out_more_work)
{
    if (out_more_work != NULL) {
        *out_more_work = false;
    }
    if (s_system_event_initialized == 0) {
        return H2_PAL_ERR_INVALID_STATE;
    }

    int dispatched = 0;
    h2_pal_result_t dispatch_result =
        system_event_dispatch_one(&dispatched);
    if (dispatch_result != H2_PAL_OK) {
        return dispatch_result;
    }

    if (dispatched == 0) {
        h2_pal_result_t ble_result =
            h2_bk3633_platform_ble_dispatch_pending();
        if (dispatch_result == H2_PAL_OK && ble_result != H2_PAL_OK) {
            dispatch_result = ble_result;
        }
        h2_pal_result_t post_ble_result =
            system_event_dispatch_one(&dispatched);
        if (dispatch_result == H2_PAL_OK && post_ble_result != H2_PAL_OK) {
            dispatch_result = post_ble_result;
        }
    }
    if (s_system_event_post_error != H2_PAL_OK) {
        if (dispatch_result == H2_PAL_OK) {
            dispatch_result = s_system_event_post_error;
        }
        s_system_event_post_error = H2_PAL_OK;
    }
    if (out_more_work != NULL) {
        /* A dispatched system event deliberately defers BLE/provider work to
         * another bounded turn. One conservative empty follow-up turn is
         * harmless when no such work exists. */
        *out_more_work = dispatched != 0 || s_system_event_count != 0u;
    }
    return dispatch_result;
}
