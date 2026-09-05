#include "h2_jieli_wl82_platform_core.h"
#include "h2_jieli_wl82_sdk_port.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define H2_JIELI_SYSTEM_EVENT_MAX_SUBSCRIPTIONS \
    (H2_PAL_SYSTEM_EVENT_TYPE_COUNT + 8u)

struct h2_pal_system_event_subscription {
    h2_pal_system_event_type_t type;
    h2_pal_system_event_handler_t handler;
    void *handler_user;
    uint32_t generation;
};

typedef struct h2_jieli_system_event_snapshot {
    h2_pal_system_event_subscription_t *subscription;
    h2_pal_system_event_handler_t handler;
    void *handler_user;
    uint32_t generation;
} h2_jieli_system_event_snapshot_t;

static h2_pal_system_event_subscription_t
    s_subscriptions[H2_JIELI_SYSTEM_EVENT_MAX_SUBSCRIPTIONS];
static h2_jieli_sdk_mutex_t *s_lock;
static uint32_t s_generation;

static int system_event_init(void *user)
{
    (void)user;
    if (s_lock != NULL) {
        return H2_PAL_OK;
    }
    s_lock = h2_jieli_sdk_mutex_create();
    if (s_lock == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    memset(s_subscriptions, 0, sizeof(s_subscriptions));
    s_generation = 0u;
    return H2_PAL_OK;
}

static void system_event_deinit(void *user)
{
    (void)user;
    h2_jieli_sdk_mutex_t *lock = s_lock;
    if (lock == NULL) {
        return;
    }
    if (h2_jieli_sdk_mutex_lock(lock, H2_JIELI_SDK_WAIT_FOREVER) == 0) {
        memset(s_subscriptions, 0, sizeof(s_subscriptions));
        s_lock = NULL;
        (void)h2_jieli_sdk_mutex_unlock(lock);
    }
    h2_jieli_sdk_mutex_destroy(lock);
}

static int system_event_post(
    void *user,
    const h2_pal_system_event_t *event,
    uint32_t timeout_ms)
{
    (void)user;
    int result = h2_pal_system_event_validate(event);
    if (result != H2_PAL_OK) {
        return result;
    }
    h2_jieli_sdk_mutex_t *lock = s_lock;
    if (lock == NULL) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    int lock_result = h2_jieli_sdk_mutex_lock(lock, timeout_ms);
    if (lock_result != 0) {
        return lock_result == 1 ? H2_PAL_ERR_TIMEOUT : H2_PAL_ERR_IO;
    }

    /* BLE and Wi-Fi SDK callbacks run on vendor tasks with small stacks.  Do
     * not place a maximum-sized subscription snapshot (about 1 KiB on WL82)
     * on those stacks.  A generation ceiling preserves snapshot semantics:
     * subscriptions created by a callback do not receive the current event. */
    const uint32_t generation_ceiling = s_generation;
    (void)h2_jieli_sdk_mutex_unlock(lock);

    result = H2_PAL_OK;
    for (size_t i = 0u; i < H2_JIELI_SYSTEM_EVENT_MAX_SUBSCRIPTIONS; ++i) {
        h2_jieli_system_event_snapshot_t snapshot = {0};
        if (h2_jieli_sdk_mutex_lock(lock, H2_JIELI_SDK_WAIT_FOREVER) != 0) {
            return H2_PAL_ERR_IO;
        }
        h2_pal_system_event_subscription_t *sub = &s_subscriptions[i];
        if (sub->handler != NULL && sub->type == event->type &&
            sub->generation <= generation_ceiling) {
            snapshot = (h2_jieli_system_event_snapshot_t){
                .subscription = sub,
                .handler = sub->handler,
                .handler_user = sub->handler_user,
                .generation = sub->generation,
            };
        }
        (void)h2_jieli_sdk_mutex_unlock(lock);
        if (snapshot.handler == NULL) {
            continue;
        }
        /* A callback may unsubscribe itself. Never hold the registry lock while
         * entering application code; generation prevents a recycled slot from
         * accidentally receiving this older event. */
        if (h2_jieli_sdk_mutex_lock(lock, H2_JIELI_SDK_WAIT_FOREVER) != 0) {
            return H2_PAL_ERR_IO;
        }
        const int active =
            snapshot.subscription->handler == snapshot.handler &&
            snapshot.subscription->generation == snapshot.generation;
        (void)h2_jieli_sdk_mutex_unlock(lock);
        if (active != 0) {
            int handler_result = snapshot.handler(snapshot.handler_user, event);
            if (result == H2_PAL_OK && handler_result != H2_PAL_OK) {
                result = handler_result;
            }
        }
    }
    return result;
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
        type >= H2_PAL_SYSTEM_EVENT_TYPE_COUNT || handler == NULL ||
        out_subscription == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_subscription = NULL;
    h2_jieli_sdk_mutex_t *lock = s_lock;
    if (lock == NULL) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    if (h2_jieli_sdk_mutex_lock(lock, H2_JIELI_SDK_WAIT_FOREVER) != 0) {
        return H2_PAL_ERR_IO;
    }
    int result = H2_PAL_ERR_FULL;
    for (size_t i = 0u; i < H2_JIELI_SYSTEM_EVENT_MAX_SUBSCRIPTIONS; ++i) {
        h2_pal_system_event_subscription_t *sub = &s_subscriptions[i];
        if (sub->handler == NULL) {
            sub->type = type;
            sub->handler = handler;
            sub->handler_user = handler_user;
            sub->generation = ++s_generation;
            if (sub->generation == 0u) {
                sub->generation = ++s_generation;
            }
            *out_subscription = sub;
            result = H2_PAL_OK;
            break;
        }
    }
    (void)h2_jieli_sdk_mutex_unlock(lock);
    return result;
}

static void system_event_unsubscribe(
    void *user,
    h2_pal_system_event_subscription_t *subscription)
{
    (void)user;
    if (subscription == NULL || s_lock == NULL) {
        return;
    }
    const uintptr_t begin = (uintptr_t)&s_subscriptions[0];
    const uintptr_t end = (uintptr_t)&s_subscriptions[H2_JIELI_SYSTEM_EVENT_MAX_SUBSCRIPTIONS];
    const uintptr_t address = (uintptr_t)subscription;
    if (address < begin || address >= end ||
        (address - begin) % sizeof(*subscription) != 0u) {
        return;
    }
    if (h2_jieli_sdk_mutex_lock(s_lock, H2_JIELI_SDK_WAIT_FOREVER) == 0) {
        memset(subscription, 0, sizeof(*subscription));
        (void)h2_jieli_sdk_mutex_unlock(s_lock);
    }
}

const h2_pal_system_event_api_t *h2_jieli_wl82_platform_system_event_api(void)
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
