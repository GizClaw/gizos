#include "h2_pal.h"
#include <stddef.h>
#include <string.h>

static h2_pal_result_t unsupported_touch_open(void *user) {
    (void)user;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_touch_get_info(
    void *user,
    h2_pal_touch_info_t *out_info) {
    (void)user;
    (void)out_info;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_touch_poll_event(
    void *user,
    h2_pal_touch_event_t *out_event) {
    (void)user;
    (void)out_event;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_touch_close(void *user) {
    (void)user;
    return H2_PAL_ERR_UNSUPPORTED;
}

static const h2_pal_touch_vtable_t unsupported_touch_vtable = {
    .open = unsupported_touch_open,
    .get_info = unsupported_touch_get_info,
    .poll_event = unsupported_touch_poll_event,
    .close = unsupported_touch_close,
};
static const h2_pal_touch_api_t unsupported_touch_api = {
    .user = NULL,
    .vtable = &unsupported_touch_vtable,
};
const h2_pal_touch_api_t *h2_pal_unsupported_touch_api(void) {
    return &unsupported_touch_api;
}
