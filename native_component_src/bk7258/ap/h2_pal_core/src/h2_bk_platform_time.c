#include "h2_bk_platform_core.h"

#include <driver/aon_rtc.h>
#include <os/os.h>
#include <stdint.h>
#include <sys/time.h>

static uint32_t s_bk_time_last_ms;
static uint64_t s_bk_time_high_ms;

static h2_pal_result_t bk_time_get_monotonic_ms(void *user, uint64_t *out_ms) {
    (void)user;
    if (out_ms == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    uint32_t int_level = rtos_enter_critical();
    uint32_t now_ms = (uint32_t)rtos_get_time();
    if (now_ms < s_bk_time_last_ms) {
        s_bk_time_high_ms += UINT64_C(1) << 32;
    }
    s_bk_time_last_ms = now_ms;
    *out_ms = s_bk_time_high_ms + (uint64_t)now_ms;
    rtos_exit_critical(int_level);

    return H2_PAL_OK;
}

static h2_pal_result_t bk_time_get_monotonic_us(void *user, uint64_t *out_us) {
    (void)user;
    if (out_us == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_us = bk_aon_rtc_get_us();
    return H2_PAL_OK;
}

static h2_pal_result_t bk_time_get_wall_ms(void *user, uint64_t *out_ms) {
    (void)user;
    if (out_ms == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    struct timeval tv;
    if (bk_rtc_gettimeofday(&tv, NULL) != 0) {
        return H2_PAL_ERR_UNAVAILABLE;
    }
    *out_ms = ((uint64_t)tv.tv_sec * 1000u) + ((uint64_t)tv.tv_usec / 1000u);
    return H2_PAL_OK;
}

static h2_pal_time_wall_status_t s_bk_wall_status;

static h2_pal_result_t bk_time_set_wall_ms(void *user, uint64_t wall_ms) {
    (void)user;
    if (wall_ms == 0u) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    struct timeval tv = {
        .tv_sec = (time_t)(wall_ms / 1000u),
        .tv_usec = (suseconds_t)((wall_ms % 1000u) * 1000u),
    };
    if (bk_rtc_settimeofday(&tv, NULL) != 0) {
        return H2_PAL_ERR_IO;
    }
    s_bk_wall_status.valid = 1u;
    s_bk_wall_status.source = H2_PAL_TIME_WALL_SOURCE_NTP;
    return H2_PAL_OK;
}

static h2_pal_result_t bk_time_get_wall_status(void *user, h2_pal_time_wall_status_t *out_status) {
    (void)user;
    if (out_status == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_status = s_bk_wall_status;
    return H2_PAL_OK;
}

static h2_pal_result_t bk_time_sleep_ms(void *user, uint32_t ms) {
    (void)user;
    return rtos_delay_milliseconds(ms) == 0 ? H2_PAL_OK : H2_PAL_ERR_INVALID_STATE;
}

const h2_pal_time_api_t *h2_bk_platform_time_api(void) {
    static const h2_pal_time_vtable_t vtable = {
        .get_monotonic_ms = bk_time_get_monotonic_ms,
        .get_monotonic_us = bk_time_get_monotonic_us,
        .get_wall_ms = bk_time_get_wall_ms,
        .set_wall_ms = bk_time_set_wall_ms,
        .get_wall_status = bk_time_get_wall_status,
        .sleep_ms = bk_time_sleep_ms,
    };
    static const h2_pal_time_api_t api = {
        .user = NULL,
        .vtable = &vtable,
    };
    return &api;
}
