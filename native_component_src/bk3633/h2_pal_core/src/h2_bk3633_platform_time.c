#include "h2_bk3633_platform_core.h"

#if defined(H2_BK3633_POWER_SDK_FAKE)
#include "h2_bk3633_power_sdk_fake.h"
#else
#include "arch.h"
#include "rwip.h"
#endif

static uint32_t s_last_half_slot;
static uint64_t s_extended_half_slots;
static uint64_t s_last_half_microseconds;
static int s_time_initialized;
static h2_bm8563_t s_wall_rtc;
static int s_wall_rtc_initialized;

static uint32_t time_critical_enter(void) {
#if defined(H2_BK3633_POWER_SDK_FAKE)
    return 0u;
#else
    uint32_t state = __disable_fiq() != 0 ? 1u : 0u;
    if (__disable_irq() != 0) {
        state |= 2u;
    }
    return state;
#endif
}

static void time_critical_exit(uint32_t state) {
#if defined(H2_BK3633_POWER_SDK_FAKE)
    (void)state;
#else
    if ((state & 1u) == 0u) {
        __enable_fiq();
    }
    if ((state & 2u) == 0u) {
        __enable_irq();
    }
#endif
}

static h2_pal_result_t time_get_monotonic_half_us(uint64_t *out_half_us) {
    rwip_time_t now;
    uint32_t critical_state;
    uint32_t forward_half_slots;
    uint64_t elapsed_half_microseconds;

    if (out_half_us == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }

    /* rwip_time_get() samples interrupt-owned half-slot and fine counters. */
    critical_state = time_critical_enter();
    now = rwip_time_get();
    now.hs &= (uint32_t)RWIP_MAX_CLOCK_TIME;
    if (s_time_initialized == 0) {
        s_last_half_slot = now.hs;
        s_extended_half_slots = now.hs;
        s_time_initialized = 1;
    } else {
        forward_half_slots =
            (now.hs - s_last_half_slot) & (uint32_t)RWIP_MAX_CLOCK_TIME;
        if (forward_half_slots >
            ((uint32_t)RWIP_MAX_CLOCK_TIME + 1u) / 2u) {
            /* Reject a stale/backward sample instead of treating it as an
             * entire 28-bit clock wrap (about 23 hours). */
            *out_half_us = s_last_half_microseconds;
            time_critical_exit(critical_state);
            return H2_PAL_OK;
        }
        s_last_half_slot = now.hs;
        s_extended_half_slots += forward_half_slots;
    }

    /* One half-slot is 312.5 us (625 half-us); hus is in half-us. */
    elapsed_half_microseconds =
        s_extended_half_slots * 625u + (uint64_t)now.hus;
    if (elapsed_half_microseconds > s_last_half_microseconds) {
        s_last_half_microseconds = elapsed_half_microseconds;
    }
    *out_half_us = s_last_half_microseconds;
    time_critical_exit(critical_state);
    return H2_PAL_OK;
}

static h2_pal_result_t time_get_monotonic_ms(void *user, uint64_t *out_ms) {
    uint64_t half_us;
    h2_pal_result_t result;

    (void)user;
    if (out_ms == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    result = time_get_monotonic_half_us(&half_us);
    if (result != H2_PAL_OK) {
        return result;
    }
    *out_ms = half_us / 2000u;
    return H2_PAL_OK;
}

static h2_pal_result_t time_get_monotonic_us(void *user, uint64_t *out_us) {
    uint64_t half_us;
    h2_pal_result_t result;

    (void)user;
    if (out_us == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    result = time_get_monotonic_half_us(&half_us);
    if (result != H2_PAL_OK) {
        return result;
    }
    *out_us = half_us / 2u;
    return H2_PAL_OK;
}

static h2_pal_result_t time_get_wall_ms(void *user, uint64_t *out_ms) {
    (void)user;
    if (out_ms == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (s_wall_rtc_initialized == 0) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    return h2_bm8563_get_unix_ms(&s_wall_rtc, out_ms);
}

static h2_pal_result_t time_set_wall_ms(void *user, uint64_t wall_ms) {
    (void)user;
    if (s_wall_rtc_initialized == 0) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    return h2_bm8563_set_unix_ms(&s_wall_rtc, wall_ms);
}

static h2_pal_result_t
time_get_wall_status(void *user, h2_pal_time_wall_status_t *out_status) {
    (void)user;
    if (out_status == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    uint64_t wall_ms;
    h2_pal_result_t rc;

    if (s_wall_rtc_initialized == 0) {
        out_status->valid = 0u;
        out_status->source = H2_PAL_TIME_WALL_SOURCE_UNKNOWN;
        return H2_PAL_OK;
    }
    rc = h2_bm8563_get_unix_ms(&s_wall_rtc, &wall_ms);
    (void)wall_ms;
    out_status->valid = (rc == H2_PAL_OK) ? 1u : 0u;
    out_status->source = (rc == H2_PAL_OK) ? H2_PAL_TIME_WALL_SOURCE_RTC
                                           : H2_PAL_TIME_WALL_SOURCE_UNKNOWN;
    return H2_PAL_OK;
}

static h2_pal_result_t time_sleep_ms(void *user, uint32_t ms) {
    (void)user;
    (void)ms;
    return H2_PAL_ERR_UNSUPPORTED;
}

h2_pal_result_t
h2_bk3633_platform_time_init(const h2_bk3633_platform_time_config_t *config) {
    h2_pal_result_t rc;

    if (config == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    h2_bk3633_platform_time_deinit();
    rc = h2_bm8563_init(&s_wall_rtc, &config->rtc);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    s_wall_rtc_initialized = 1;
    return H2_PAL_OK;
}

void h2_bk3633_platform_time_deinit(void) {
    h2_bm8563_deinit(&s_wall_rtc);
    s_wall_rtc_initialized = 0;
}

const h2_pal_time_api_t *h2_bk3633_platform_time_api(void) {
    static const h2_pal_time_vtable_t vtable = {
        .get_monotonic_ms = time_get_monotonic_ms,
        .get_monotonic_us = time_get_monotonic_us,
        .get_wall_ms = time_get_wall_ms,
        .set_wall_ms = time_set_wall_ms,
        .get_wall_status = time_get_wall_status,
        .sleep_ms = time_sleep_ms,
    };
    static const h2_pal_time_api_t api = {
        .user = NULL,
        .vtable = &vtable,
    };
    return &api;
}
