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
    /* A pre-epoch or malformed reading would wrap when cast to uint64_t and
     * could pass the plausibility floor as a huge value. */
    if (tv.tv_sec < 0 || tv.tv_usec < 0 || tv.tv_usec >= 1000000) {
        return H2_PAL_ERR_UNAVAILABLE;
    }
    *out_ms = ((uint64_t)tv.tv_sec * 1000u) + ((uint64_t)tv.tv_usec / 1000u);
    return H2_PAL_OK;
}

/* Validity is read out of the clock itself, so nothing has to be remembered
 * across program starts: the AON RTC keeps counting across reboots, so a
 * plausible reading means the clock still carries a calibration. A reading
 * near the epoch means the RTC restarted and the clock waits for the next
 * set_wall_ms. Earliest reading accepted as calibrated: 2020-01-01T00:00:00Z. */
#define H2_BK_WALL_MIN_VALID_MS UINT64_C(1577836800000)

static h2_pal_result_t bk_time_set_wall_ms(void *user, uint64_t wall_ms) {
    (void)user;
    if (wall_ms == 0u) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    struct timeval tv = {
        .tv_sec = (time_t)(wall_ms / 1000u),
        .tv_usec = (suseconds_t)((wall_ms % 1000u) * 1000u),
    };
    return bk_rtc_settimeofday(&tv, NULL) == 0 ? H2_PAL_OK : H2_PAL_ERR_IO;
}

static h2_pal_result_t bk_time_get_wall_status(void *user, h2_pal_time_wall_status_t *out_status) {
    (void)user;
    if (out_status == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    uint64_t wall_ms = 0u;
    /* A clock that cannot be read is an error, not an uncalibrated clock. */
    const h2_pal_result_t rc = bk_time_get_wall_ms(user, &wall_ms);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    out_status->valid = wall_ms >= H2_BK_WALL_MIN_VALID_MS;
    out_status->source = out_status->valid
                             ? H2_PAL_TIME_WALL_SOURCE_RTC
                             : H2_PAL_TIME_WALL_SOURCE_BOOT_DEFAULT;
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
