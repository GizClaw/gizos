#include "h2_windows_platform.h"
#include "../src/h2_windows_internal.h"

#include <assert.h>

static int count_netif(void *user, const h2_pal_netif_ref_t *ref,
                       const h2_pal_netif_status_t *status) {
    size_t *count = user;
    assert(ref != NULL && status != NULL);
    ++*count;
    return 0;
}

static int count_event(void *user, const h2_pal_system_event_t *event) {
    size_t *count = user;
    assert(event->type == H2_PAL_SYSTEM_EVENT_TYPE_NETIF_DEFAULT_CHANGED);
    ++*count;
    return 0;
}

typedef struct self_unsubscribe_context {
    const h2_pal_system_event_api_t *events;
    h2_pal_system_event_subscription_t *subscription;
    size_t count;
} self_unsubscribe_context_t;

static int self_unsubscribe(void *user,
                            const h2_pal_system_event_t *event) {
    self_unsubscribe_context_t *context = user;
    assert(event->type == H2_PAL_SYSTEM_EVENT_TYPE_NETIF_DEFAULT_CHANGED);
    ++context->count;
    h2_pal_system_event_unsubscribe(context->events,
                                    context->subscription);
    context->subscription = NULL;
    return 0;
}

int main(void) {
    const h2_windows_route_candidate_t routes[] = {
        {.interface_id = 7u, .route_metric = 10u, .interface_metric = 5u,
         .connected = 1},
        {.interface_id = 3u, .route_metric = 12u, .interface_metric = 3u,
         .connected = 1},
        {.interface_id = 1u, .route_metric = 1u, .interface_metric = 1u,
         .connected = 0},
    };
    assert(h2_windows_netif_select_default(
               routes, sizeof(routes) / sizeof(routes[0])) == 3u);
    const h2_windows_platform_config_t config = {0};
    h2_windows_platform_t *platform = NULL;
    assert(h2_windows_platform_create(&config, &platform) == H2_PAL_OK);
    size_t netif_count = 0u;
    assert(h2_pal_netif_list(h2_windows_netif_api(platform), NULL, count_netif,
                             &netif_count) == H2_PAL_OK);
    assert(netif_count != 0u);
    const h2_pal_system_event_api_t *events =
        h2_windows_system_event_api(platform);
    assert(h2_pal_system_event_init(events) == H2_PAL_OK);
    size_t event_count = 0u;
    h2_pal_system_event_subscription_t *subscription = NULL;
    assert(h2_pal_system_event_subscribe(
               events, H2_PAL_SYSTEM_EVENT_TYPE_NETIF_DEFAULT_CHANGED,
               count_event, &event_count, &subscription) == H2_PAL_OK);
    h2_pal_system_event_t event = {
        .type = H2_PAL_SYSTEM_EVENT_TYPE_NETIF_DEFAULT_CHANGED,
    };
    assert(h2_pal_system_event_post(events, &event, 0u) == H2_PAL_OK);
    assert(event_count == 1u);
    h2_pal_system_event_unsubscribe(events, subscription);

    self_unsubscribe_context_t self_context = {
        .events = events,
    };
    assert(h2_pal_system_event_subscribe(
               events, H2_PAL_SYSTEM_EVENT_TYPE_NETIF_DEFAULT_CHANGED,
               self_unsubscribe, &self_context,
               &self_context.subscription) == H2_PAL_OK);
    assert(h2_pal_system_event_post(events, &event, 0u) == H2_PAL_OK);
    assert(self_context.count == 1u);
    assert(self_context.subscription == NULL);
    assert(h2_pal_system_event_post(events, &event, 0u) == H2_PAL_OK);
    assert(self_context.count == 1u);
    h2_pal_system_event_deinit(events);
    assert(h2_windows_platform_destroy(&platform) == H2_PAL_OK);
    return 0;
}
