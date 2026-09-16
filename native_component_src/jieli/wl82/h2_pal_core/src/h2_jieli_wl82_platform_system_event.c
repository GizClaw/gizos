#include "h2_jieli_wl82_platform_core.h"
#include "h2_jieli_wl82_sdk_port.h"
#include "h2_jieli_wl82_atomic.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define H2_JIELI_SYSTEM_EVENT_MAX_SUBSCRIPTIONS \
    (H2_PAL_SYSTEM_EVENT_TYPE_COUNT + 8u)

#define H2_JIELI_WL82_SYSTEM_EVENT_PAYLOAD_MAX 1024u
#define H2_JIELI_WL82_SYSTEM_EVENT_QUEUE_DEPTH 4u

typedef struct {
    uint64_t generation_ceiling;
    uint64_t timestamp_ms;
    uint32_t source_id;
    uint8_t type;
    uint8_t flags;
    uint16_t payload_size;
    union {
        uint8_t inline_bytes[8];
        void *heap;
    } data;
} event_message_t;

_Static_assert(sizeof(event_message_t) == H2_JIELI_SDK_EVENT_MESSAGE_SIZE,
               "sys_event has only a 32-byte read buffer");
_Static_assert(H2_PAL_SYSTEM_EVENT_TYPE_COUNT <= UINT8_MAX, "event type must fit");

static uint32_t s_depth;
static uint32_t s_overflow;
/* Accessed only under the registry lock, including the first delivery. */
static const void *s_dispatcher_task;
static void event_dispatch(const void *message, size_t size);

struct h2_pal_system_event_subscription {
    h2_pal_system_event_type_t type;
    h2_pal_system_event_handler_t handler;
    void *handler_user;
    uint64_t generation;
    unsigned queued;
    int dispatching;
    unsigned waiters;
    int retiring;
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
    for (;;) {
        if (!(state & EVENT_ACTIVE) &&
            !(retiring && (state & EVENT_CLOSING) && (state & EVENT_REFS))) {
            return NULL;
        }
        if ((state & EVENT_REFS) == EVENT_REFS) {
            if (!retiring) return NULL;
            /* Retirement must wait for reference capacity instead of abandoning callbacks. */
            h2_jieli_sdk_sleep_ms(1u);
            state = h2_jieli_atomic_load_u32(&s_lifecycle);
            continue;
        }
        if (h2_jieli_atomic_cas_u32(&s_lifecycle, &state, state + 1u)) return s_lock;
    }
}

static void event_destroy(void)
{
    h2_jieli_sdk_mutex_destroy(s_lock);
    s_lock = NULL;
    memset(s_subscriptions, 0, sizeof(s_subscriptions));
    h2_jieli_atomic_store_u32(&s_depth, 0u);
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
    if (h2_jieli_sdk_event_dispatcher_start(event_dispatch) != 0) {
        event_destroy();
        return H2_PAL_ERR_TASK;
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

static void event_clear_retired(h2_pal_system_event_subscription_t *sub)
{
    if (sub->retiring && sub->queued == 0u && !sub->dispatching && sub->waiters == 0u)
        memset(sub, 0, sizeof(*sub));
}

static int event_reserve(void)
{
    uint32_t depth = h2_jieli_atomic_load_u32(&s_depth);
    for (;;) {
        if (depth >= H2_JIELI_WL82_SYSTEM_EVENT_QUEUE_DEPTH) return 0;
        if (h2_jieli_atomic_cas_u32(&s_depth, &depth, depth + 1u)) return 1;
    }
}

static void event_message_release(event_message_t *message)
{
    if (message->payload_size > sizeof(message->data.inline_bytes))
        h2_jieli_sdk_free(message->data.heap);
    (void)h2_jieli_atomic_fetch_sub_u32(&s_depth, 1u);
    event_release();
}

static int event_matches(const h2_pal_system_event_subscription_t *sub,
                         const event_message_t *message)
{
    return sub->type == (h2_pal_system_event_type_t)message->type &&
           sub->generation <= message->generation_ceiling;
}

static void event_dispatch(const void *bytes, size_t size)
{
    /* The SDK payload is byte-aligned, so do not cast it to our envelope. */
    event_message_t message;
    if (bytes == NULL || size != sizeof(message)) return;
    memcpy(&message, bytes, sizeof(message));
    h2_pal_system_event_t event = {
        .type = (h2_pal_system_event_type_t)message.type,
        .source_id = message.source_id,
        .timestamp_ms = message.timestamp_ms,
        .payload_size = message.payload_size,
        .payload = message.payload_size > sizeof(message.data.inline_bytes)
            ? message.data.heap : message.data.inline_bytes,
    };
    h2_jieli_sdk_mutex_t *lock = s_lock; /* The queued reference pins this lock. */
    event_lock_wait(lock);
    s_dispatcher_task = h2_jieli_sdk_task_current();
    (void)h2_jieli_sdk_mutex_unlock(lock);
    for (size_t i = 0; i < H2_JIELI_SYSTEM_EVENT_MAX_SUBSCRIPTIONS; ++i) {
        event_lock_wait(lock);
        h2_pal_system_event_subscription_t *sub = &s_subscriptions[i];
        if (sub->queued != 0u && event_matches(sub, &message)) {
            --sub->queued;
            if (!sub->retiring && sub->handler != NULL) {
                h2_pal_system_event_handler_t handler = sub->handler;
                void *handler_user = sub->handler_user;
                sub->dispatching = 1;
                (void)h2_jieli_sdk_mutex_unlock(lock);
                (void)handler(handler_user, &event);
                event_lock_wait(lock);
                sub->dispatching = 0;
            }
            event_clear_retired(sub);
        }
        (void)h2_jieli_sdk_mutex_unlock(lock);
    }
    event_message_release(&message);
}

static int system_event_post(
    void *user,
    const h2_pal_system_event_t *event,
    uint32_t timeout_ms)
{
    (void)user;
    int result = h2_pal_system_event_validate(event);
    if (result != H2_PAL_OK) return result;
    if (event->payload_size > H2_JIELI_WL82_SYSTEM_EVENT_PAYLOAD_MAX)
        return H2_PAL_ERR_INVALID_ARG;
    if (h2_jieli_sdk_in_interrupt()) return H2_PAL_ERR_INVALID_STATE;
    h2_jieli_sdk_mutex_t *lock = event_retain(0);
    if (lock == NULL) return H2_PAL_ERR_INVALID_STATE;
    if (!event_reserve()) {
        (void)h2_jieli_atomic_fetch_add_u32(&s_overflow, 1u);
        event_release();
        return H2_PAL_ERR_FULL;
    }
    event_message_t message = {
        .type = (uint8_t)event->type,
        .source_id = event->source_id,
        .timestamp_ms = event->timestamp_ms,
        .payload_size = (uint16_t)event->payload_size,
    };
    if (message.payload_size > sizeof(message.data.inline_bytes)) {
        message.data.heap = h2_jieli_sdk_malloc(message.payload_size);
        if (message.data.heap == NULL) {
            event_message_release(&message);
            return H2_PAL_ERR_NO_MEMORY;
        }
        memcpy(message.data.heap, event->payload, message.payload_size);
    } else if (message.payload_size != 0u) {
        memcpy(message.data.inline_bytes, event->payload, message.payload_size);
    }
    int lock_result = h2_jieli_sdk_mutex_lock(lock, timeout_ms);
    if (lock_result != 0) {
        event_message_release(&message);
        return lock_result == 1 ? H2_PAL_ERR_TIMEOUT : H2_PAL_ERR_IO;
    }
    message.generation_ceiling = s_generation;
    unsigned matched = 0;
    for (size_t i = 0; i < H2_JIELI_SYSTEM_EVENT_MAX_SUBSCRIPTIONS; ++i) {
        h2_pal_system_event_subscription_t *sub = &s_subscriptions[i];
        if (!sub->retiring && sub->handler != NULL && event_matches(sub, &message)) {
            ++sub->queued;
            ++matched;
        }
    }
    (void)h2_jieli_sdk_mutex_unlock(lock);
    if (matched == 0u) {
        event_message_release(&message);
        return H2_PAL_OK;
    }
    result = h2_jieli_sdk_event_post(&message, sizeof(message));
    if (result == 0) return H2_PAL_OK;
    event_lock_wait(lock);
    for (size_t i = 0; i < H2_JIELI_SYSTEM_EVENT_MAX_SUBSCRIPTIONS; ++i) {
        h2_pal_system_event_subscription_t *sub = &s_subscriptions[i];
        if (sub->queued != 0u && event_matches(sub, &message)) {
            --sub->queued;
            event_clear_retired(sub);
        }
    }
    (void)h2_jieli_sdk_mutex_unlock(lock);
    if (result == 1) (void)h2_jieli_atomic_fetch_add_u32(&s_overflow, 1u);
    event_message_release(&message);
    return result == 1 ? H2_PAL_ERR_FULL : H2_PAL_ERR_IO;
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
    if (task != s_dispatcher_task) {
        ++subscription->waiters; /* prevent slot reuse while waiting */
        while (subscription->queued != 0u || subscription->dispatching) {
            (void)h2_jieli_sdk_mutex_unlock(lock);
            h2_jieli_sdk_sleep_ms(1u);
            event_lock_wait(lock);
        }
        --subscription->waiters;
    }
    event_clear_retired(subscription);
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

uint32_t h2_jieli_wl82_platform_system_event_overflow_count(void)
{
    return h2_jieli_atomic_load_u32(&s_overflow);
}
