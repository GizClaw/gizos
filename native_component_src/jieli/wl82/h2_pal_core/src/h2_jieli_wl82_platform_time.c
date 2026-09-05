#include "h2_jieli_wl82_platform_core.h"
#include "h2_jieli_wl82_sdk_port.h"

/* The SDK tick timer exposes a 32-bit millisecond counter (~49.7 days). The
 * provider extends it to 64 bits by tracking wraps; callers must sample at
 * least once per wrap, which every Runtime poll loop does. */
static uint32_t s_last_ms;
static uint32_t s_wraps;

static uint64_t monotonic_ms(void)
{
    const uint32_t now = h2_jieli_sdk_time_ms();
    if (now < s_last_ms) {
        s_wraps++;
    }
    s_last_ms = now;
    return ((uint64_t)s_wraps << 32) | now;
}

static h2_pal_result_t time_get_monotonic_ms(void *user, uint64_t *out_ms)
{
    (void)user;
    if (out_ms == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_ms = monotonic_ms();
    return H2_PAL_OK;
}

static h2_pal_result_t time_get_monotonic_us(void *user, uint64_t *out_us)
{
    (void)user;
    if (out_us == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_us = monotonic_ms() * 1000u;
    return H2_PAL_OK;
}

static h2_pal_result_t time_get_wall_ms(void *user, uint64_t *out_ms)
{
    (void)user;
    (void)out_ms;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t time_set_wall_ms(void *user, uint64_t wall_ms)
{
    (void)user;
    (void)wall_ms;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t time_get_wall_status(void *user, h2_pal_time_wall_status_t *out_status)
{
    (void)user;
    if (out_status == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    out_status->valid = 0u;
    out_status->source = H2_PAL_TIME_WALL_SOURCE_UNKNOWN;
    return H2_PAL_OK;
}

static h2_pal_result_t time_sleep_ms(void *user, uint32_t ms)
{
    (void)user;
    h2_jieli_sdk_sleep_ms(ms);
    return H2_PAL_OK;
}

static const h2_pal_time_vtable_t s_time_vtable = {
    .get_monotonic_ms = time_get_monotonic_ms,
    .get_monotonic_us = time_get_monotonic_us,
    .get_wall_ms = time_get_wall_ms,
    .set_wall_ms = time_set_wall_ms,
    .get_wall_status = time_get_wall_status,
    .sleep_ms = time_sleep_ms,
};

static const h2_pal_time_api_t s_time_api = {
    .user = NULL,
    .vtable = &s_time_vtable,
};

const h2_pal_time_api_t *h2_jieli_wl82_platform_time_api(void)
{
    return &s_time_api;
}
