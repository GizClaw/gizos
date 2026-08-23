#include "h2_bk_platform_core.h"

#include <components/log.h>
#include <os/mem.h>
#include <os/os.h>
#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "queue.h"
#include "semphr.h"

#define H2_BK_SYSTEM_EVENT_MAX_SUBSCRIPTIONS 64u
#define H2_BK_SYSTEM_EVENT_QUEUE_DEPTH 16u
#define H2_BK_SYSTEM_EVENT_TASK_STACK 4096u

typedef struct h2_bk_system_event_message {
    h2_pal_system_event_type_t type;
    uint32_t source_id;
    uint64_t timestamp_ms;
    size_t payload_size;
    uint8_t payload[];
} h2_bk_system_event_message_t;

typedef struct h2_bk_system_event_handler_ref {
    h2_pal_system_event_subscription_t *subscription;
    h2_pal_system_event_handler_t handler;
    void *handler_user;
} h2_bk_system_event_handler_ref_t;

struct h2_pal_system_event_subscription {
    int active;
    h2_pal_system_event_type_t type;
    h2_pal_system_event_handler_t handler;
    void *handler_user;
    size_t in_flight;
    beken_semaphore_t drain;
};

static h2_pal_system_event_subscription_t
    s_h2_bk_system_event_subscriptions[H2_BK_SYSTEM_EVENT_MAX_SUBSCRIPTIONS];
/* FreeRTOS static control blocks must start zeroed on every BK image. Generic
 * Loader/App startup does not clear a fixed .psram.bss region. */
static StaticSemaphore_t s_h2_bk_system_event_drain_storage[H2_BK_SYSTEM_EVENT_MAX_SUBSCRIPTIONS];
static StaticQueue_t s_h2_bk_system_event_queue_control;
static uint8_t s_h2_bk_system_event_queue_storage[H2_BK_SYSTEM_EVENT_QUEUE_DEPTH *
                                                  sizeof(h2_bk_system_event_message_t *)];
static StaticSemaphore_t s_h2_bk_system_event_mutex_control;
static beken_queue_t s_h2_bk_system_event_queue;
static beken_thread_t s_h2_bk_system_event_thread;
static beken_mutex_t s_h2_bk_system_event_mutex;
static int s_h2_bk_system_event_initialized;

static void h2_bk_system_event_free_message(h2_bk_system_event_message_t *message) {
    if (message != NULL) {
        psram_free(message);
    }
}

static void h2_bk_system_event_dispatch_message(const h2_bk_system_event_message_t *message) {
    if (message == NULL) {
        return;
    }

    h2_pal_system_event_t event;
    memset(&event, 0, sizeof(event));
    event.type = message->type;
    event.source_id = message->source_id;
    event.timestamp_ms = message->timestamp_ms;
    event.payload_size = message->payload_size;
    event.payload = message->payload_size > 0u ? message->payload : NULL;

    h2_bk_system_event_handler_ref_t refs[H2_BK_SYSTEM_EVENT_MAX_SUBSCRIPTIONS];
    size_t ref_count = 0u;

    (void)rtos_lock_mutex(&s_h2_bk_system_event_mutex);
    for (size_t i = 0u; i < H2_BK_SYSTEM_EVENT_MAX_SUBSCRIPTIONS; ++i) {
        h2_pal_system_event_subscription_t *sub = &s_h2_bk_system_event_subscriptions[i];
        if (sub->active != 0 && sub->type == message->type && sub->handler != NULL) {
            sub->in_flight++;
            refs[ref_count++] = (h2_bk_system_event_handler_ref_t){
                .subscription = sub,
                .handler = sub->handler,
                .handler_user = sub->handler_user,
            };
        }
    }
    (void)rtos_unlock_mutex(&s_h2_bk_system_event_mutex);

    for (size_t i = 0u; i < ref_count; ++i) {
        h2_pal_system_event_subscription_t *sub = refs[i].subscription;
        (void)refs[i].handler(refs[i].handler_user, &event);

        (void)rtos_lock_mutex(&s_h2_bk_system_event_mutex);
        if (sub->in_flight > 0u) {
            sub->in_flight--;
        }
        if (sub->active == 0 && sub->in_flight == 0u && sub->drain != NULL) {
            (void)rtos_set_semaphore(&sub->drain);
        }
        (void)rtos_unlock_mutex(&s_h2_bk_system_event_mutex);
    }
}

static void h2_bk_system_event_task(void *arg) {
    (void)arg;

    for (;;) {
        h2_bk_system_event_message_t *message = NULL;
        if (rtos_pop_from_queue(&s_h2_bk_system_event_queue, &message, BEKEN_WAIT_FOREVER) !=
            kNoErr) {
            continue;
        }
        if (message == NULL) {
            break;
        }
        if (message->type == H2_PAL_SYSTEM_EVENT_TYPE_NONE) {
            (void)h2_bk_platform_netif_reconcile_default();
        } else {
            h2_bk_system_event_dispatch_message(message);
        }
        h2_bk_system_event_free_message(message);
    }

    rtos_delete_thread(NULL);
}

h2_pal_result_t h2_bk_platform_netif_reconcile_default_async(void) {
    if (s_h2_bk_system_event_initialized == 0 ||
        s_h2_bk_system_event_queue == NULL) {
        BK_LOGW("h2_netif", "async reconcile unavailable\r\n");
        return H2_PAL_ERR_INVALID_STATE;
    }
    h2_bk_system_event_message_t *message =
        (h2_bk_system_event_message_t *)psram_malloc(sizeof(*message));
    if (message == NULL) {
        BK_LOGW("h2_netif", "async reconcile allocation failed\r\n");
        return H2_PAL_ERR_NO_MEMORY;
    }
    os_memset(message, 0, sizeof(*message));
    if (rtos_push_to_queue(&s_h2_bk_system_event_queue, &message, 0u) !=
        kNoErr) {
        h2_bk_system_event_free_message(message);
        BK_LOGW("h2_netif", "async reconcile queue full\r\n");
        return H2_PAL_ERR_TIMEOUT;
    }
    return H2_PAL_OK;
}

static int h2_bk_system_event_init(void *user) {
    (void)user;
    if (s_h2_bk_system_event_initialized != 0) {
        return h2_bk_platform_netif_monitor_init();
    }

    s_h2_bk_system_event_mutex =
        (beken_mutex_t)xSemaphoreCreateMutexStatic(&s_h2_bk_system_event_mutex_control);
    if (s_h2_bk_system_event_mutex == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    s_h2_bk_system_event_queue = (beken_queue_t)xQueueCreateStatic(
        H2_BK_SYSTEM_EVENT_QUEUE_DEPTH, sizeof(h2_bk_system_event_message_t *),
        s_h2_bk_system_event_queue_storage, &s_h2_bk_system_event_queue_control);
    if (s_h2_bk_system_event_queue == NULL) {
        (void)rtos_deinit_mutex(&s_h2_bk_system_event_mutex);
        s_h2_bk_system_event_mutex = NULL;
        return H2_PAL_ERR_NO_MEMORY;
    }
    for (size_t i = 0u; i < H2_BK_SYSTEM_EVENT_MAX_SUBSCRIPTIONS; ++i) {
        s_h2_bk_system_event_subscriptions[i].drain =
            (beken_semaphore_t)xSemaphoreCreateBinaryStatic(&s_h2_bk_system_event_drain_storage[i]);
        if (s_h2_bk_system_event_subscriptions[i].drain == NULL) {
            (void)rtos_deinit_queue(&s_h2_bk_system_event_queue);
            (void)rtos_deinit_mutex(&s_h2_bk_system_event_mutex);
            s_h2_bk_system_event_queue = NULL;
            s_h2_bk_system_event_mutex = NULL;
            return H2_PAL_ERR_NO_MEMORY;
        }
    }
    if (rtos_core0_create_psram_thread(&s_h2_bk_system_event_thread, 6u, "h2_sys_evt",
                                       (beken_thread_function_t)h2_bk_system_event_task,
                                       H2_BK_SYSTEM_EVENT_TASK_STACK, NULL) != kNoErr) {
        (void)rtos_deinit_queue(&s_h2_bk_system_event_queue);
        (void)rtos_deinit_mutex(&s_h2_bk_system_event_mutex);
        s_h2_bk_system_event_queue = NULL;
        s_h2_bk_system_event_mutex = NULL;
        return H2_PAL_ERR_TASK;
    }
    printf("H2_PAL_TASK_READY name=h2_sys_evt core=0 priority=6 "
           "stack=psram size=%u\n",
           (unsigned int)H2_BK_SYSTEM_EVENT_TASK_STACK);

    s_h2_bk_system_event_initialized = 1;
    return h2_bk_platform_netif_monitor_init();
}

static void h2_bk_system_event_deinit(void *user) {
    (void)user;
    if (s_h2_bk_system_event_initialized == 0) {
        return;
    }

    h2_bk_platform_netif_monitor_deinit();

    /*
     * The AP system-event dispatcher has process lifetime. Deinit clears H2
     * listeners but does not tear down the queue/task, because WiFi/BLE
     * callbacks may still emit platform events after a test or app subsystem
     * has released its subscriptions.
     */
    h2_pal_system_event_subscription_t *drains[H2_BK_SYSTEM_EVENT_MAX_SUBSCRIPTIONS];
    size_t drain_count = 0u;
    int on_event_thread = rtos_is_current_thread(&s_h2_bk_system_event_thread);

    (void)rtos_lock_mutex(&s_h2_bk_system_event_mutex);
    for (size_t i = 0u; i < H2_BK_SYSTEM_EVENT_MAX_SUBSCRIPTIONS; ++i) {
        h2_pal_system_event_subscription_t *sub = &s_h2_bk_system_event_subscriptions[i];
        sub->active = 0;
        sub->type = H2_PAL_SYSTEM_EVENT_TYPE_NONE;
        sub->handler = NULL;
        sub->handler_user = NULL;
        if (sub->in_flight == 0u && sub->drain != NULL) {
            (void)rtos_set_semaphore(&sub->drain);
        } else if (!on_event_thread && sub->drain != NULL) {
            drains[drain_count++] = sub;
        }
    }
    (void)rtos_unlock_mutex(&s_h2_bk_system_event_mutex);

    for (size_t i = 0u; i < drain_count; ++i) {
        (void)rtos_get_semaphore(&drains[i]->drain, BEKEN_WAIT_FOREVER);
    }
}

static int h2_bk_system_event_post(void *user, const h2_pal_system_event_t *event,
                                   uint32_t timeout_ms) {
    (void)user;

    int rc = h2_pal_system_event_validate(event);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    if (s_h2_bk_system_event_initialized == 0 || s_h2_bk_system_event_queue == NULL) {
        return H2_PAL_ERR_INVALID_STATE;
    }

    size_t message_size = sizeof(h2_bk_system_event_message_t) + event->payload_size;
    h2_bk_system_event_message_t *message =
        (h2_bk_system_event_message_t *)psram_malloc(message_size);
    if (message == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    os_memset(message, 0, sizeof(*message));
    message->type = event->type;
    message->source_id = event->source_id;
    message->timestamp_ms = event->timestamp_ms;
    message->payload_size = event->payload_size;
    if (event->payload_size > 0u) {
        os_memcpy(message->payload, event->payload, event->payload_size);
    }

    if (rtos_push_to_queue(&s_h2_bk_system_event_queue, &message, timeout_ms) != kNoErr) {
        h2_bk_system_event_free_message(message);
        return H2_PAL_ERR_TIMEOUT;
    }
    return H2_PAL_OK;
}

static int h2_bk_system_event_subscribe(void *user, h2_pal_system_event_type_t type,
                                        h2_pal_system_event_handler_t handler, void *handler_user,
                                        h2_pal_system_event_subscription_t **out_subscription) {
    (void)user;
    if (type <= H2_PAL_SYSTEM_EVENT_TYPE_NONE || type >= H2_PAL_SYSTEM_EVENT_TYPE_COUNT ||
        handler == NULL || out_subscription == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (s_h2_bk_system_event_initialized == 0) {
        return H2_PAL_ERR_INVALID_STATE;
    }

    *out_subscription = NULL;
    (void)rtos_lock_mutex(&s_h2_bk_system_event_mutex);
    for (size_t i = 0u; i < H2_BK_SYSTEM_EVENT_MAX_SUBSCRIPTIONS; ++i) {
        h2_pal_system_event_subscription_t *sub = &s_h2_bk_system_event_subscriptions[i];
        if (sub->active == 0 && sub->in_flight == 0u) {
            while (rtos_get_semaphore(&sub->drain, 0u) == kNoErr) {
            }
            sub->active = 1;
            sub->type = type;
            sub->handler = handler;
            sub->handler_user = handler_user;
            *out_subscription = sub;
            (void)rtos_unlock_mutex(&s_h2_bk_system_event_mutex);
            return H2_PAL_OK;
        }
    }
    (void)rtos_unlock_mutex(&s_h2_bk_system_event_mutex);
    return H2_PAL_ERR_FULL;
}

static void h2_bk_system_event_unsubscribe(void *user,
                                           h2_pal_system_event_subscription_t *subscription) {
    (void)user;
    if (subscription == NULL || s_h2_bk_system_event_initialized == 0) {
        return;
    }
    (void)rtos_lock_mutex(&s_h2_bk_system_event_mutex);
    subscription->active = 0;
    subscription->type = H2_PAL_SYSTEM_EVENT_TYPE_NONE;
    subscription->handler = NULL;
    subscription->handler_user = NULL;
    size_t in_flight = subscription->in_flight;
    beken_semaphore_t drain = subscription->drain;
    (void)rtos_unlock_mutex(&s_h2_bk_system_event_mutex);

    if (in_flight == 0u || drain == NULL || rtos_is_current_thread(&s_h2_bk_system_event_thread)) {
        return;
    }
    (void)rtos_get_semaphore(&drain, BEKEN_WAIT_FOREVER);
}

static const h2_pal_system_event_vtable_t s_h2_bk_system_event_api_vtable = {
    .init = h2_bk_system_event_init,
    .deinit = h2_bk_system_event_deinit,
    .post = h2_bk_system_event_post,
    .subscribe = h2_bk_system_event_subscribe,
    .unsubscribe = h2_bk_system_event_unsubscribe,
};
static const h2_pal_system_event_api_t s_h2_bk_system_event_api = {
    .user = NULL,
    .vtable = &s_h2_bk_system_event_api_vtable,
};

const h2_pal_system_event_api_t *h2_bk_platform_system_event_api(void) {
    return &s_h2_bk_system_event_api;
}
