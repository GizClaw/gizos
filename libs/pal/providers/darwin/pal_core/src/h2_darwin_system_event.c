#include "h2_darwin_platform.h"
#include "h2_darwin_netif_internal.h"

#include <pthread.h>
#include <stddef.h>
#include <string.h>

#define H2_DARWIN_SYSTEM_EVENT_MAX_SUBSCRIPTIONS 48u

struct h2_pal_system_event_subscription {
    int active;
    size_t in_flight;
    h2_pal_system_event_type_t type;
    h2_pal_system_event_handler_t handler;
    void *handler_user;
};

typedef struct h2_darwin_system_event_dispatch {
    h2_pal_system_event_subscription_t *subscription;
    h2_pal_system_event_handler_t handler;
    void *handler_user;
} h2_darwin_system_event_dispatch_t;

static h2_pal_system_event_subscription_t
    s_subscriptions[H2_DARWIN_SYSTEM_EVENT_MAX_SUBSCRIPTIONS];
static pthread_mutex_t s_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_idle = PTHREAD_COND_INITIALIZER;
static int s_initialized;

static int h2_darwin_system_event_init(void *user) {
    (void)user;
    pthread_mutex_lock(&s_mutex);
    if (s_initialized != 0) {
        pthread_mutex_unlock(&s_mutex);
        return H2_PAL_OK;
    }
    s_initialized = 1;
    pthread_mutex_unlock(&s_mutex);
    h2_pal_result_t rc = h2_darwin_netif_monitor_start();
    if (rc != H2_PAL_OK) {
        pthread_mutex_lock(&s_mutex);
        s_initialized = 0;
        pthread_mutex_unlock(&s_mutex);
    }
    return rc;
}

static void h2_darwin_system_event_deinit(void *user) {
    (void)user;
    h2_darwin_netif_monitor_stop();
    pthread_mutex_lock(&s_mutex);
    s_initialized = 0;
    for (size_t i = 0u; i < H2_DARWIN_SYSTEM_EVENT_MAX_SUBSCRIPTIONS; ++i) {
        while (s_subscriptions[i].in_flight != 0u) {
            pthread_cond_wait(&s_idle, &s_mutex);
        }
    }
    memset(s_subscriptions, 0, sizeof(s_subscriptions));
    pthread_mutex_unlock(&s_mutex);
}

static int h2_darwin_system_event_post(
    void *user,
    const h2_pal_system_event_t *event,
    uint32_t timeout_ms) {
    (void)user;
    (void)timeout_ms;
    int rc = h2_pal_system_event_validate(event);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    h2_darwin_system_event_dispatch_t dispatches[
        H2_DARWIN_SYSTEM_EVENT_MAX_SUBSCRIPTIONS];
    size_t count = 0u;
    pthread_mutex_lock(&s_mutex);
    if (s_initialized == 0) {
        pthread_mutex_unlock(&s_mutex);
        return H2_PAL_ERR_INVALID_STATE;
    }
    for (size_t i = 0u; i < H2_DARWIN_SYSTEM_EVENT_MAX_SUBSCRIPTIONS; ++i) {
        h2_pal_system_event_subscription_t *subscription = &s_subscriptions[i];
        if (subscription->active != 0 && subscription->type == event->type &&
            subscription->handler != NULL) {
            ++subscription->in_flight;
            dispatches[count++] = (h2_darwin_system_event_dispatch_t){
                .subscription = subscription,
                .handler = subscription->handler,
                .handler_user = subscription->handler_user,
            };
        }
    }
    pthread_mutex_unlock(&s_mutex);

    int result = H2_PAL_OK;
    for (size_t i = 0u; i < count; ++i) {
        int handler_rc = dispatches[i].handler(
            dispatches[i].handler_user, event);
        pthread_mutex_lock(&s_mutex);
        --dispatches[i].subscription->in_flight;
        pthread_cond_broadcast(&s_idle);
        pthread_mutex_unlock(&s_mutex);
        if (result == H2_PAL_OK && handler_rc != H2_PAL_OK) {
            result = handler_rc;
        }
    }
    return result;
}

static int h2_darwin_system_event_subscribe(
    void *user,
    h2_pal_system_event_type_t type,
    h2_pal_system_event_handler_t handler,
    void *handler_user,
    h2_pal_system_event_subscription_t **out_subscription) {
    (void)user;
    if (type <= H2_PAL_SYSTEM_EVENT_TYPE_NONE ||
        type >= H2_PAL_SYSTEM_EVENT_TYPE_COUNT || handler == NULL ||
        out_subscription == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_subscription = NULL;
    pthread_mutex_lock(&s_mutex);
    if (s_initialized == 0) {
        pthread_mutex_unlock(&s_mutex);
        return H2_PAL_ERR_INVALID_STATE;
    }
    for (size_t i = 0u; i < H2_DARWIN_SYSTEM_EVENT_MAX_SUBSCRIPTIONS; ++i) {
        h2_pal_system_event_subscription_t *subscription = &s_subscriptions[i];
        if (subscription->active == 0 && subscription->in_flight == 0u) {
            subscription->active = 1;
            subscription->type = type;
            subscription->handler = handler;
            subscription->handler_user = handler_user;
            *out_subscription = subscription;
            pthread_mutex_unlock(&s_mutex);
            return H2_PAL_OK;
        }
    }
    pthread_mutex_unlock(&s_mutex);
    return H2_PAL_ERR_FULL;
}

static void h2_darwin_system_event_unsubscribe(
    void *user,
    h2_pal_system_event_subscription_t *subscription) {
    (void)user;
    if (subscription == NULL) {
        return;
    }
    pthread_mutex_lock(&s_mutex);
    subscription->active = 0;
    while (subscription->in_flight != 0u) {
        pthread_cond_wait(&s_idle, &s_mutex);
    }
    memset(subscription, 0, sizeof(*subscription));
    pthread_mutex_unlock(&s_mutex);
}

static const h2_pal_system_event_vtable_t s_vtable = {
    .init = h2_darwin_system_event_init,
    .deinit = h2_darwin_system_event_deinit,
    .post = h2_darwin_system_event_post,
    .subscribe = h2_darwin_system_event_subscribe,
    .unsubscribe = h2_darwin_system_event_unsubscribe,
};
static const h2_pal_system_event_api_t s_api = {
    .user = NULL,
    .vtable = &s_vtable,
};

const h2_pal_system_event_api_t *h2_darwin_system_event_api(void) {
    return &s_api;
}
