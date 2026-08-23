#include "h2_windows_internal.h"

#include <string.h>

struct h2_pal_system_event_subscription {
    h2_windows_platform_t *platform;
    h2_pal_system_event_type_t type;
    h2_pal_system_event_handler_t handler;
    void *handler_user;
    LONG in_flight;
    int active;
    int deferred_free;
};

#if defined(_MSC_VER)
static __declspec(thread) unsigned windows_dispatch_depth;
#else
static _Thread_local unsigned windows_dispatch_depth;
#endif

static void windows_system_event_release_dispatch(
    h2_windows_platform_t *platform,
    h2_pal_system_event_subscription_t *subscription) {
    LONG remaining = InterlockedDecrement(&subscription->in_flight);
    int release = 0;
    EnterCriticalSection(&platform->lock);
    if (remaining == 0 && subscription->deferred_free) {
        subscription->deferred_free = 0;
        release = 1;
    }
    WakeAllConditionVariable(&platform->idle);
    LeaveCriticalSection(&platform->lock);
    if (release) {
        h2_windows_heap_free(subscription);
        h2_windows_object_release(platform);
    }
}

static int windows_system_event_post(void *user,
                                     const h2_pal_system_event_t *event,
                                     uint32_t timeout_ms) {
    h2_windows_platform_t *platform = user;
    (void)timeout_ms;
    if (h2_pal_system_event_validate(event) != H2_PAL_OK) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    h2_pal_system_event_subscription_t
        *dispatch[H2_WINDOWS_SUBSCRIPTION_CAPACITY];
    size_t dispatch_count = 0u;
    EnterCriticalSection(&platform->lock);
    if (!platform->system_event_initialized) {
        LeaveCriticalSection(&platform->lock);
        return H2_PAL_ERR_INVALID_STATE;
    }
    for (size_t index = 0u; index < H2_WINDOWS_SUBSCRIPTION_CAPACITY;
         ++index) {
        h2_pal_system_event_subscription_t *subscription =
            platform->subscriptions[index];
        if (subscription != NULL && subscription->active &&
            subscription->type == event->type) {
            (void)InterlockedIncrement(&subscription->in_flight);
            dispatch[dispatch_count++] = subscription;
        }
    }
    LeaveCriticalSection(&platform->lock);
    int result = H2_PAL_OK;
    for (size_t index = 0u; index < dispatch_count; ++index) {
        h2_pal_system_event_subscription_t *subscription = dispatch[index];
        EnterCriticalSection(&platform->lock);
        int active = subscription->active;
        LeaveCriticalSection(&platform->lock);
        if (active) {
            ++windows_dispatch_depth;
            int handler_result =
                subscription->handler(subscription->handler_user, event);
            --windows_dispatch_depth;
            if (handler_result != 0 && result == H2_PAL_OK) {
                result = H2_PAL_EXIT;
            }
        }
        windows_system_event_release_dispatch(platform, subscription);
    }
    return result;
}

static VOID CALLBACK windows_route_work(PTP_CALLBACK_INSTANCE instance,
                                        void *context, PTP_WORK work) {
    (void)instance;
    (void)work;
    h2_windows_platform_t *platform = context;
    do {
        (void)InterlockedExchange(&platform->route_dirty, 0);
        h2_pal_netif_ref_t current;
        int current_valid = 0;
        if (h2_windows_netif_default_ref(platform, &current,
                                         &current_valid) != H2_PAL_OK) {
            continue;
        }
        EnterCriticalSection(&platform->lock);
        h2_pal_netif_ref_t previous = platform->default_netif;
        int previous_valid = platform->default_netif_valid;
        int changed = previous_valid != current_valid ||
                      (previous_valid &&
                       !h2_pal_netif_ref_equal(&previous, &current));
        if (changed) {
            platform->default_netif = current;
            platform->default_netif_valid = current_valid;
        }
        int initialized = platform->system_event_initialized;
        LeaveCriticalSection(&platform->lock);
        if (!changed || !initialized) {
            continue;
        }
        h2_pal_netif_default_changed_t payload;
        memset(&payload, 0, sizeof(payload));
        payload.previous_valid = (uint8_t)previous_valid;
        payload.current_valid = (uint8_t)current_valid;
        if (previous_valid) {
            payload.previous = previous;
        }
        if (current_valid) {
            payload.current = current;
        }
        uint64_t timestamp = 0u;
        (void)h2_windows_monotonic_ms(platform, &timestamp);
        h2_pal_system_event_t event = {
            .type = H2_PAL_SYSTEM_EVENT_TYPE_NETIF_DEFAULT_CHANGED,
            .source_id = current_valid ? current.id : 0u,
            .timestamp_ms = timestamp,
            .payload = &payload,
            .payload_size = sizeof(payload),
        };
        (void)windows_system_event_post(platform, &event, 0u);
    } while (InterlockedCompareExchange(&platform->route_dirty, 0, 0) != 0);
}

static VOID CALLBACK windows_route_changed(void *context,
                                           MIB_IPFORWARD_ROW2 *row,
                                           MIB_NOTIFICATION_TYPE type) {
    (void)row;
    (void)type;
    h2_windows_platform_t *platform = context;
    if (InterlockedCompareExchange(&platform->route_stopping, 0, 0) != 0) {
        return;
    }
    EnterCriticalSection(&platform->lock);
    if (InterlockedCompareExchange(&platform->route_stopping, 0, 0) == 0 &&
        platform->system_event_initialized && platform->route_work != NULL &&
        InterlockedExchange(&platform->route_dirty, 1) == 0) {
        SubmitThreadpoolWork(platform->route_work);
    }
    LeaveCriticalSection(&platform->lock);
}

static int windows_system_event_init(void *user) {
    h2_windows_platform_t *platform = user;
    EnterCriticalSection(&platform->lock);
    if (platform->system_event_initialized) {
        LeaveCriticalSection(&platform->lock);
        return H2_PAL_OK;
    }
    (void)InterlockedExchange(&platform->route_stopping, 0);
    (void)InterlockedExchange(&platform->route_dirty, 0);
    platform->route_work = CreateThreadpoolWork(windows_route_work, platform,
                                                NULL);
    if (platform->route_work == NULL) {
        LeaveCriticalSection(&platform->lock);
        return H2_PAL_ERR_NO_MEMORY;
    }
    h2_pal_result_t result = h2_windows_netif_default_ref(
        platform, &platform->default_netif, &platform->default_netif_valid);
    if (result != H2_PAL_OK) {
        CloseThreadpoolWork(platform->route_work);
        platform->route_work = NULL;
        LeaveCriticalSection(&platform->lock);
        return result;
    }
    HANDLE notification = NULL;
    DWORD native_result = NotifyRouteChange2(
        AF_INET, windows_route_changed, platform, FALSE, &notification);
    if (native_result != NO_ERROR) {
        CloseThreadpoolWork(platform->route_work);
        platform->route_work = NULL;
        LeaveCriticalSection(&platform->lock);
        return h2_windows_error_from_win32(native_result);
    }
    platform->route_notification = notification;
    platform->system_event_initialized = 1;
    (void)InterlockedExchange(&platform->route_dirty, 1);
    SubmitThreadpoolWork(platform->route_work);
    LeaveCriticalSection(&platform->lock);
    return H2_PAL_OK;
}

h2_pal_result_t h2_windows_system_event_shutdown(
    h2_windows_platform_t *platform) {
    EnterCriticalSection(&platform->lock);
    if (!platform->system_event_initialized) {
        LeaveCriticalSection(&platform->lock);
        return H2_PAL_OK;
    }
    for (size_t index = 0u; index < H2_WINDOWS_SUBSCRIPTION_CAPACITY;
         ++index) {
        if (platform->subscriptions[index] != NULL) {
            LeaveCriticalSection(&platform->lock);
            return H2_PAL_ERR_BUSY;
        }
    }
    HANDLE notification = platform->route_notification;
    PTP_WORK work = platform->route_work;
    (void)InterlockedExchange(&platform->route_stopping, 1);
    LeaveCriticalSection(&platform->lock);
    if (notification != NULL) {
        DWORD result = CancelMibChangeNotify2(notification);
        if (result != NO_ERROR) {
            (void)InterlockedExchange(&platform->route_stopping, 0);
            return h2_windows_error_from_win32(result);
        }
    }
    EnterCriticalSection(&platform->lock);
    platform->route_notification = NULL;
    platform->route_work = NULL;
    platform->system_event_initialized = 0;
    LeaveCriticalSection(&platform->lock);
    if (work != NULL) {
        WaitForThreadpoolWorkCallbacks(work, TRUE);
        CloseThreadpoolWork(work);
    }
    return H2_PAL_OK;
}

static void windows_system_event_deinit(void *user) {
    (void)h2_windows_system_event_shutdown(user);
}

static int windows_system_event_subscribe(
    void *user, h2_pal_system_event_type_t type,
    h2_pal_system_event_handler_t handler, void *handler_user,
    h2_pal_system_event_subscription_t **out_subscription) {
    h2_windows_platform_t *platform = user;
    if (type <= H2_PAL_SYSTEM_EVENT_TYPE_NONE ||
        type >= H2_PAL_SYSTEM_EVENT_TYPE_COUNT || handler == NULL ||
        out_subscription == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_subscription = NULL;
    h2_pal_system_event_subscription_t *subscription =
        h2_windows_heap_alloc(sizeof(*subscription));
    if (subscription == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    memset(subscription, 0, sizeof(*subscription));
    subscription->platform = platform;
    subscription->type = type;
    subscription->handler = handler;
    subscription->handler_user = handler_user;
    subscription->active = 1;
    EnterCriticalSection(&platform->lock);
    if (!platform->system_event_initialized) {
        LeaveCriticalSection(&platform->lock);
        h2_windows_heap_free(subscription);
        return H2_PAL_ERR_INVALID_STATE;
    }
    for (size_t index = 0u; index < H2_WINDOWS_SUBSCRIPTION_CAPACITY;
         ++index) {
        if (platform->subscriptions[index] == NULL) {
            platform->subscriptions[index] = subscription;
            LeaveCriticalSection(&platform->lock);
            h2_windows_object_acquire(platform);
            *out_subscription = subscription;
            return H2_PAL_OK;
        }
    }
    LeaveCriticalSection(&platform->lock);
    h2_windows_heap_free(subscription);
    return H2_PAL_ERR_NO_SPACE;
}

static void windows_system_event_unsubscribe(
    void *user, h2_pal_system_event_subscription_t *subscription) {
    h2_windows_platform_t *platform = user;
    if (subscription == NULL || subscription->platform != platform) {
        return;
    }
    EnterCriticalSection(&platform->lock);
    subscription->active = 0;
    for (size_t index = 0u; index < H2_WINDOWS_SUBSCRIPTION_CAPACITY;
         ++index) {
        if (platform->subscriptions[index] == subscription) {
            platform->subscriptions[index] = NULL;
            break;
        }
    }
    while (InterlockedCompareExchange(&subscription->in_flight, 0, 0) != 0) {
        if (windows_dispatch_depth != 0u) {
            subscription->deferred_free = 1;
            LeaveCriticalSection(&platform->lock);
            return;
        }
        (void)SleepConditionVariableCS(&platform->idle, &platform->lock,
                                       INFINITE);
    }
    LeaveCriticalSection(&platform->lock);
    h2_windows_heap_free(subscription);
    h2_windows_object_release(platform);
}

const h2_pal_system_event_vtable_t h2_windows_system_event_vtable = {
    .init = windows_system_event_init,
    .deinit = windows_system_event_deinit,
    .post = windows_system_event_post,
    .subscribe = windows_system_event_subscribe,
    .unsubscribe = windows_system_event_unsubscribe,
};
