#include "h2_jieli_wl82_platform_core.h"
#include "h2_jieli_wl82_sdk_port.h"
#include "h2_jieli_wl82_atomic.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define H2_JIELI_SYSTEM_EVENT_MAX_SUBSCRIPTIONS \
    (H2_PAL_SYSTEM_EVENT_TYPE_COUNT + 8u)

typedef struct event_dispatch event_dispatch_t;
struct h2_pal_system_event_subscription {
    h2_pal_system_event_type_t type;
    h2_pal_system_event_handler_t handler;
    void *handler_user;
    uint64_t generation;
    event_dispatch_t *dispatches;
    unsigned waiters;
    int retiring;
};

/* Stack-owned nodes identify nested/self dispatch without SDK TLS support. */
struct event_dispatch {
    event_dispatch_t *next;
    const void *task;
};

static void event_lock_wait(h2_jieli_sdk_mutex_t *lock) {
    /* Retirement cannot abandon a borrowed callback on a transient lock error. */
    while (h2_jieli_sdk_mutex_lock(lock, H2_JIELI_SDK_WAIT_FOREVER) != 0)
        h2_jieli_sdk_sleep_ms(1u);
}

static h2_pal_system_event_subscription_t
    s_subscriptions[H2_JIELI_SYSTEM_EVENT_MAX_SUBSCRIPTIONS];
static h2_jieli_sdk_mutex_t *s_lock;
/* Registry-lock protected while ACTIVE; reset only before publishing ACTIVE.
 * Never wrap: exhaustion rejects new subscriptions until complete teardown. */
static uint64_t s_generation;

/* Phase, init owners and in-flight operations share one atomic word. Every
 * successful init acquires one owner; deinit releases one, and only the last
 * owner closes admission. Keeping both counts in the same CAS prevents a new
 * owner from racing final teardown. At most 16383 owners / 32767 operations
 * fit; saturation is rejected without changing state. Deinit while inactive
 * is harmless, but callers must balance their own successful init calls.
 * Deinit from a handler never waits for that handler: the final operation
 * performs deferred destruction after the final owner enters CLOSING. */
#define EVENT_ACTIVE (1u << 31)
#define EVENT_CLOSING (1u << 30)
#define EVENT_INITIALIZING (1u << 29)
#define EVENT_OWNER_ONE (1u << 15)
#define EVENT_OWNERS (EVENT_INITIALIZING - EVENT_OWNER_ONE)
#define EVENT_REFS (EVENT_OWNER_ONE - 1u)
static uint32_t s_lifecycle;

/* Retirement may join CLOSING while a live operation still pins the registry.
 * A CAS cannot resurrect CLOSING with zero references during destruction. */
static h2_jieli_sdk_mutex_t *event_retain(int retiring)
{
    uint32_t state = h2_jieli_atomic_load_u32(&s_lifecycle);
    do {
        if ((!(state & EVENT_ACTIVE) &&
             !(retiring && (state & EVENT_CLOSING) && (state & EVENT_REFS))) ||
            (state & EVENT_REFS) == EVENT_REFS) {
            return NULL;
        }
    } while (!h2_jieli_atomic_cas_u32(&s_lifecycle, &state, state + 1u));
    return s_lock;
}

static void event_destroy(void)
{
    h2_jieli_sdk_mutex_destroy(s_lock);
    s_lock = NULL;
    memset(s_subscriptions, 0, sizeof(s_subscriptions));
    h2_jieli_atomic_store_u32(&s_lifecycle, 0u);
}

static void event_release(void)
{
    if (h2_jieli_atomic_fetch_sub_u32(&s_lifecycle, 1u) == EVENT_CLOSING + 1u) {
        event_destroy();
    }
}

static int system_event_init(void *user)
{
    (void)user;
    uint32_t state = h2_jieli_atomic_load_u32(&s_lifecycle);
    for (;;) {
        if (state & EVENT_ACTIVE) {
            if ((state & EVENT_OWNERS) == EVENT_OWNERS) return H2_PAL_ERR_FULL;
            if (h2_jieli_atomic_cas_u32(&s_lifecycle, &state, state + EVENT_OWNER_ONE))
                return H2_PAL_OK;
        } else if (state != 0u) {
            return H2_PAL_ERR_BUSY;
        } else if (h2_jieli_atomic_cas_u32(&s_lifecycle, &state, EVENT_INITIALIZING)) {
            break;
        }
    }
    s_lock = h2_jieli_sdk_mutex_create();
    if (s_lock == NULL) {
        h2_jieli_atomic_store_u32(&s_lifecycle, 0u);
        return H2_PAL_ERR_NO_MEMORY;
    }
    memset(s_subscriptions, 0, sizeof(s_subscriptions));
    s_generation = 0u;
    h2_jieli_atomic_store_u32(&s_lifecycle, EVENT_ACTIVE | EVENT_OWNER_ONE);
    return H2_PAL_OK;
}

static void system_event_deinit(void *user)
{
    (void)user;
    uint32_t state = h2_jieli_atomic_load_u32(&s_lifecycle);
    uint32_t next;
    do {
        if (!(state & EVENT_ACTIVE)) return;
        next = (state & EVENT_OWNERS) > EVENT_OWNER_ONE
            ? state - EVENT_OWNER_ONE
            : EVENT_CLOSING | (state & EVENT_REFS);
    } while (!h2_jieli_atomic_cas_u32(&s_lifecycle, &state, next));
    if (next == EVENT_CLOSING) event_destroy();
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
    h2_jieli_sdk_mutex_t *lock = event_retain(0);
    if (lock == NULL) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    int lock_result = h2_jieli_sdk_mutex_lock(lock, timeout_ms);
    if (lock_result != 0) {
        event_release();
        return lock_result == 1 ? H2_PAL_ERR_TIMEOUT : H2_PAL_ERR_IO;
    }

    /* BLE and Wi-Fi SDK callbacks run on vendor tasks with small stacks.  Do
     * not place a maximum-sized subscription snapshot (about 1 KiB on WL82)
     * on those stacks.  A generation ceiling preserves snapshot semantics:
     * subscriptions created by a callback do not receive the current event. */
    const uint64_t generation_ceiling = s_generation;
    (void)h2_jieli_sdk_mutex_unlock(lock);

    result = H2_PAL_OK;
    for (size_t i = 0u; i < H2_JIELI_SYSTEM_EVENT_MAX_SUBSCRIPTIONS; ++i) {
        if (!(h2_jieli_atomic_load_u32(&s_lifecycle) & EVENT_ACTIVE)) break;
        event_dispatch_t dispatch = {.task = h2_jieli_sdk_task_current()};
        h2_pal_system_event_handler_t handler = NULL;
        void *handler_user = NULL;
        event_lock_wait(lock);
        h2_pal_system_event_subscription_t *sub = &s_subscriptions[i];
        if (!sub->retiring && sub->handler != NULL && sub->type == event->type &&
            sub->generation <= generation_ceiling) {
            handler = sub->handler;
            handler_user = sub->handler_user;
            dispatch.next = sub->dispatches;
            sub->dispatches = &dispatch;
        }
        (void)h2_jieli_sdk_mutex_unlock(lock);
        if (handler == NULL) continue;
        int handler_result = handler(handler_user, event);
        event_lock_wait(lock);
        event_dispatch_t **entry = &sub->dispatches;
        while (*entry != &dispatch) entry = &(*entry)->next;
        *entry = dispatch.next;
        if (sub->retiring && sub->dispatches == NULL && sub->waiters == 0u)
            memset(sub, 0, sizeof(*sub));
        (void)h2_jieli_sdk_mutex_unlock(lock);
        if (result == H2_PAL_OK && handler_result != H2_PAL_OK)
            result = handler_result;
    }
    event_release();
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
    h2_jieli_sdk_mutex_t *lock = event_retain(0);
    if (lock == NULL) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    if (h2_jieli_sdk_mutex_lock(lock, H2_JIELI_SDK_WAIT_FOREVER) != 0) {
        event_release();
        return H2_PAL_ERR_IO;
    }
    int result = H2_PAL_ERR_FULL;
    for (size_t i = 0u; i < H2_JIELI_SYSTEM_EVENT_MAX_SUBSCRIPTIONS &&
                        s_generation != UINT64_MAX; ++i) {
        h2_pal_system_event_subscription_t *sub = &s_subscriptions[i];
        if (sub->handler == NULL && !sub->retiring) {
            sub->type = type;
            sub->handler = handler;
            sub->handler_user = handler_user;
            sub->generation = ++s_generation;
            *out_subscription = sub;
            result = H2_PAL_OK;
            break;
        }
    }
    (void)h2_jieli_sdk_mutex_unlock(lock);
    event_release();
    return result;
}

static void system_event_unsubscribe(
    void *user,
    h2_pal_system_event_subscription_t *subscription)
{
    (void)user;
    if (subscription == NULL) {
        return;
    }
    const uintptr_t begin = (uintptr_t)&s_subscriptions[0];
    const uintptr_t end = (uintptr_t)&s_subscriptions[H2_JIELI_SYSTEM_EVENT_MAX_SUBSCRIPTIONS];
    const uintptr_t address = (uintptr_t)subscription;
    if (address < begin || address >= end ||
        (address - begin) % sizeof(*subscription) != 0u) {
        return;
    }
    h2_jieli_sdk_mutex_t *lock = event_retain(1);
    if (lock == NULL) return;
    event_lock_wait(lock);
    subscription->retiring = 1;
    subscription->handler = NULL; /* atomic with dispatch admission under lock */
    const void *task = h2_jieli_sdk_task_current();
    int self = 0;
    for (event_dispatch_t *d = subscription->dispatches; d != NULL; d = d->next)
        if (d->task == task) self = 1;
    if (!self) {
        ++subscription->waiters; /* prevent slot reuse while waiting */
        while (subscription->dispatches != NULL) {
            (void)h2_jieli_sdk_mutex_unlock(lock);
            h2_jieli_sdk_sleep_ms(1u);
            event_lock_wait(lock);
        }
        --subscription->waiters;
    }
    if (subscription->dispatches == NULL && subscription->waiters == 0u)
        memset(subscription, 0, sizeof(*subscription));
    (void)h2_jieli_sdk_mutex_unlock(lock);
    event_release();
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
