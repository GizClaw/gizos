#include "h2_bk7258_board_private.h"

static h2_pal_result_t touch_open(void *user) {
    (void)user;
    return H2_PAL_ERR_UNAVAILABLE;
}

static h2_pal_result_t touch_get_info(
    void *user,
    h2_pal_touch_info_t *out_info) {
    (void)user;
    (void)out_info;
    return H2_PAL_ERR_UNAVAILABLE;
}

static h2_pal_result_t touch_poll_event(
    void *user,
    h2_pal_touch_event_t *out_event) {
    (void)user;
    (void)out_event;
    return H2_PAL_ERR_UNAVAILABLE;
}

static h2_pal_result_t touch_close(void *user) {
    (void)user;
    return H2_PAL_ERR_UNAVAILABLE;
}

static const h2_pal_touch_vtable_t s_touch_vtable = {
    .open = touch_open,
    .get_info = touch_get_info,
    .poll_event = touch_poll_event,
    .close = touch_close,
};

static const h2_pal_touch_api_t s_touch_api = {
    .user = NULL,
    .vtable = &s_touch_vtable,
};

const h2_pal_touch_api_t *h2_bk7258_board_touch_api(void) {
    return &s_touch_api;
}
