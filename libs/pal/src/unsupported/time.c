#include "h2_pal.h"
#include <stddef.h>
#include <string.h>

static h2_pal_result_t unsupported_time_get_monotonic_ms(void *p0, uint64_t *p1) {
    (void)p0;
    (void)p1;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_time_get_monotonic_us(void *p0, uint64_t *p1) {
    (void)p0;
    (void)p1;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_time_get_wall_ms(void *p0, uint64_t *p1) {
    (void)p0;
    (void)p1;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_time_set_wall_ms(void *p0, uint64_t p1) {
    (void)p0;
    (void)p1;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_time_get_wall_status(void *p0, h2_pal_time_wall_status_t *p1) {
    (void)p0;
    (void)p1;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_time_sleep_ms(void *p0, uint32_t p1) {
    (void)p0;
    (void)p1;
    return H2_PAL_ERR_UNSUPPORTED;
}

static const h2_pal_time_vtable_t unsupported_time_vtable = {
    .get_monotonic_ms = unsupported_time_get_monotonic_ms,
    .get_monotonic_us = unsupported_time_get_monotonic_us,
    .get_wall_ms = unsupported_time_get_wall_ms,
    .set_wall_ms = unsupported_time_set_wall_ms,
    .get_wall_status = unsupported_time_get_wall_status,
    .sleep_ms = unsupported_time_sleep_ms,
};
static const h2_pal_time_api_t unsupported_time_api = { .user = NULL, .vtable = &unsupported_time_vtable };
const h2_pal_time_api_t *h2_pal_unsupported_time_api(void) { return &unsupported_time_api; }
