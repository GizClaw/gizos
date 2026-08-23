#include "h2_pal.h"
#include <stddef.h>
#include <string.h>

static h2_pal_result_t unsupported_timer_create(void *p0, const h2_pal_timer_config_t *p1, h2_pal_timer_t **p2) {
    (void)p0;
    (void)p1;
    (void)p2;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_timer_destroy(void *p0, h2_pal_timer_t *p1) {
    (void)p0;
    (void)p1;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_timer_start(void *p0, h2_pal_timer_t *p1) {
    (void)p0;
    (void)p1;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_timer_stop(void *p0, h2_pal_timer_t *p1) {
    (void)p0;
    (void)p1;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_timer_reset(void *p0, h2_pal_timer_t *p1) {
    (void)p0;
    (void)p1;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_timer_set_period_ms(void *p0, h2_pal_timer_t *p1, uint32_t p2) {
    (void)p0;
    (void)p1;
    (void)p2;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_timer_is_running(void *p0, h2_pal_timer_t *p1, int *p2) {
    (void)p0;
    (void)p1;
    (void)p2;
    return H2_PAL_ERR_UNSUPPORTED;
}

static const h2_pal_timer_vtable_t unsupported_timer_vtable = {
    .create = unsupported_timer_create,
    .destroy = unsupported_timer_destroy,
    .start = unsupported_timer_start,
    .stop = unsupported_timer_stop,
    .reset = unsupported_timer_reset,
    .set_period_ms = unsupported_timer_set_period_ms,
    .is_running = unsupported_timer_is_running,
};
static const h2_pal_timer_api_t unsupported_timer_api = { .user = NULL, .vtable = &unsupported_timer_vtable };
const h2_pal_timer_api_t *h2_pal_unsupported_timer_api(void) { return &unsupported_timer_api; }
