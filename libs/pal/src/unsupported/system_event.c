#include "h2_pal.h"
#include <stddef.h>
#include <string.h>

static int unsupported_system_event_init(void *p0) {
    (void)p0;
    return H2_PAL_ERR_UNSUPPORTED;
}

static void unsupported_system_event_deinit(void *p0) {
    (void)p0;
}

static int unsupported_system_event_post(void *p0, const h2_pal_system_event_t *p1, uint32_t p2) {
    (void)p0;
    (void)p1;
    (void)p2;
    return H2_PAL_ERR_UNSUPPORTED;
}

static int unsupported_system_event_subscribe(void *p0, h2_pal_system_event_type_t p1, h2_pal_system_event_handler_t p2, void *p3, h2_pal_system_event_subscription_t **p4) {
    (void)p0;
    (void)p1;
    (void)p2;
    (void)p3;
    (void)p4;
    return H2_PAL_ERR_UNSUPPORTED;
}

static void unsupported_system_event_unsubscribe(void *p0, h2_pal_system_event_subscription_t *p1) {
    (void)p0;
    (void)p1;
}

static const h2_pal_system_event_vtable_t unsupported_system_event_vtable = {
    .init = unsupported_system_event_init,
    .deinit = unsupported_system_event_deinit,
    .post = unsupported_system_event_post,
    .subscribe = unsupported_system_event_subscribe,
    .unsubscribe = unsupported_system_event_unsubscribe,
};
static const h2_pal_system_event_api_t unsupported_system_event_api = { .user = NULL, .vtable = &unsupported_system_event_vtable };
const h2_pal_system_event_api_t *h2_pal_unsupported_system_event_api(void) { return &unsupported_system_event_api; }
