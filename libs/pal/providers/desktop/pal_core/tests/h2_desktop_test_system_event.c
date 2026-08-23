#include "h2_desktop_test_system_event.h"

#include <string.h>

#define H2_DESKTOP_TEST_SYSTEM_EVENT_MAX_SUBSCRIPTIONS 16u

struct h2_pal_system_event_subscription {
    int active;
    h2_pal_system_event_type_t type;
    h2_pal_system_event_handler_t handler;
    void *handler_user;
};

static h2_pal_system_event_subscription_t
    s_subscriptions[H2_DESKTOP_TEST_SYSTEM_EVENT_MAX_SUBSCRIPTIONS];
static int s_initialized;

static int test_event_init(void *user) {
    (void)user;
    if (s_initialized) {
        return H2_PAL_OK;
    }
    memset(s_subscriptions, 0, sizeof(s_subscriptions));
    s_initialized = 1;
    return H2_PAL_OK;
}

static void test_event_deinit(void *user) {
    (void)user;
    s_initialized = 0;
    memset(s_subscriptions, 0, sizeof(s_subscriptions));
}

static int test_event_post(
    void *user,
    const h2_pal_system_event_t *event,
    uint32_t timeout_ms) {
    struct dispatch {
        h2_pal_system_event_handler_t handler;
        void *handler_user;
    } dispatches[H2_DESKTOP_TEST_SYSTEM_EVENT_MAX_SUBSCRIPTIONS];
    size_t count = 0u;
    size_t index;
    int result;
    (void)user;
    (void)timeout_ms;
    result = h2_pal_system_event_validate(event);
    if (result != H2_PAL_OK) {
        return result;
    }
    if (!s_initialized) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    result = H2_PAL_OK;
    for (index = 0u;
         index < H2_DESKTOP_TEST_SYSTEM_EVENT_MAX_SUBSCRIPTIONS;
         ++index) {
        const h2_pal_system_event_subscription_t *subscription =
            &s_subscriptions[index];
        if (subscription->active && subscription->type == event->type) {
            dispatches[count++] = (struct dispatch){
                .handler = subscription->handler,
                .handler_user = subscription->handler_user,
            };
        }
    }
    for (index = 0u; index < count; ++index) {
        int handler_result = dispatches[index].handler(
            dispatches[index].handler_user, event);
        if (result == H2_PAL_OK && handler_result != H2_PAL_OK) {
            result = handler_result;
        }
    }
    return result;
}

static int test_event_subscribe(
    void *user,
    h2_pal_system_event_type_t type,
    h2_pal_system_event_handler_t handler,
    void *handler_user,
    h2_pal_system_event_subscription_t **out_subscription) {
    size_t index;
    (void)user;
    if (!s_initialized) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    if (type <= H2_PAL_SYSTEM_EVENT_TYPE_NONE ||
        type >= H2_PAL_SYSTEM_EVENT_TYPE_COUNT || handler == NULL ||
        out_subscription == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_subscription = NULL;
    for (index = 0u;
         index < H2_DESKTOP_TEST_SYSTEM_EVENT_MAX_SUBSCRIPTIONS;
         ++index) {
        h2_pal_system_event_subscription_t *subscription =
            &s_subscriptions[index];
        if (!subscription->active) {
            subscription->active = 1;
            subscription->type = type;
            subscription->handler = handler;
            subscription->handler_user = handler_user;
            *out_subscription = subscription;
            return H2_PAL_OK;
        }
    }
    return H2_PAL_ERR_FULL;
}

static void test_event_unsubscribe(
    void *user,
    h2_pal_system_event_subscription_t *subscription) {
    (void)user;
    if (subscription != NULL) {
        memset(subscription, 0, sizeof(*subscription));
    }
}

const h2_pal_system_event_api_t *h2_desktop_test_system_event_api(void) {
    static const h2_pal_system_event_vtable_t vtable = {
        .init = test_event_init,
        .deinit = test_event_deinit,
        .post = test_event_post,
        .subscribe = test_event_subscribe,
        .unsubscribe = test_event_unsubscribe,
    };
    static const h2_pal_system_event_api_t api = {
        .user = NULL,
        .vtable = &vtable,
    };
    return &api;
}

int h2_desktop_test_system_event_init(void) {
    return h2_pal_system_event_init(h2_desktop_test_system_event_api());
}

void h2_desktop_test_system_event_deinit(void) {
    h2_pal_system_event_deinit(h2_desktop_test_system_event_api());
}
