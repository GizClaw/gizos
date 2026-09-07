#include "h2_esp_platform_core.h"

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <sys/time.h>
#include <stdatomic.h>

#if defined(H2_ESP_TIME_TEST)
int h2_esp_platform_time_test_gettimeofday(struct timeval *tv);
int h2_esp_platform_time_test_settimeofday(const struct timeval *tv);
#define h2_esp_gettimeofday(tv) h2_esp_platform_time_test_gettimeofday(tv)
#define h2_esp_settimeofday(tv) h2_esp_platform_time_test_settimeofday(tv)
#else
#define h2_esp_gettimeofday(tv) gettimeofday((tv), NULL)
#define h2_esp_settimeofday(tv) settimeofday((tv), NULL)
#endif

static h2_pal_result_t esp_time_get_monotonic_ms(void *user, uint64_t *out_ms) {
    (void)user;
    if (out_ms == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_ms = (uint64_t)(esp_timer_get_time() / 1000);
    return H2_PAL_OK;
}

static h2_pal_result_t esp_time_get_monotonic_us(void *user, uint64_t *out_us) {
    (void)user;
    if (out_us == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_us = (uint64_t)esp_timer_get_time();
    return H2_PAL_OK;
}

static h2_pal_result_t esp_time_get_wall_ms(void *user, uint64_t *out_ms) {
    (void)user;
    if (out_ms == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    struct timeval tv;
    if (h2_esp_gettimeofday(&tv) != 0) {
        return H2_PAL_ERR_UNAVAILABLE;
    }
    *out_ms = ((uint64_t)tv.tv_sec * 1000u) + ((uint64_t)tv.tv_usec / 1000u);
    return H2_PAL_OK;
}

/* Validity is read out of the clock itself. The RTC timer keeps counting
 * through deep sleep and software resets, so a plausible reading means the
 * clock still carries a calibration from an earlier session and the UI can
 * show the time while charging. A power-on or brownout reset restarts the RTC
 * timer near the epoch, which reads as implausible and keeps the clock
 * uncalibrated until the next set_wall_ms. Earliest reading accepted as
 * calibrated: 2020-01-01T00:00:00Z. */
#define H2_ESP_WALL_MIN_VALID_MS 1577836800000ull

static atomic_bool s_esp_wall_set_in_session;

#if defined(H2_ESP_TIME_TEST)
/* Emulates a program start: process state is lost, the RTC clock is not. */
void h2_esp_platform_time_test_restart(void) {
    atomic_store_explicit(&s_esp_wall_set_in_session, 0, memory_order_release);
}
#endif

static h2_pal_result_t esp_time_set_wall_ms(void *user, uint64_t wall_ms) {
    (void)user;
    if (wall_ms == 0u) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    struct timeval tv = {
        .tv_sec = (time_t)(wall_ms / 1000u),
        .tv_usec = (suseconds_t)((wall_ms % 1000u) * 1000u),
    };
    if (h2_esp_settimeofday(&tv) != 0) {
        return H2_PAL_ERR_IO;
    }
    atomic_store_explicit(&s_esp_wall_set_in_session, 1, memory_order_release);
    return H2_PAL_OK;
}

static h2_pal_result_t esp_time_get_wall_status(void *user, h2_pal_time_wall_status_t *out_status) {
    (void)user;
    if (out_status == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    uint64_t wall_ms = 0u;
    out_status->valid = esp_time_get_wall_ms(user, &wall_ms) == H2_PAL_OK &&
                        wall_ms >= H2_ESP_WALL_MIN_VALID_MS;
    if (!out_status->valid) {
        out_status->source = H2_PAL_TIME_WALL_SOURCE_BOOT_DEFAULT;
    } else if (atomic_load_explicit(&s_esp_wall_set_in_session, memory_order_acquire)) {
        out_status->source = H2_PAL_TIME_WALL_SOURCE_USER;
    } else {
        /* Calibrated in an earlier session and carried over by the RTC timer. */
        out_status->source = H2_PAL_TIME_WALL_SOURCE_RTC;
    }
    return H2_PAL_OK;
}

static h2_pal_result_t esp_time_sleep_ms(void *user, uint32_t ms) {
    (void)user;
    vTaskDelay(pdMS_TO_TICKS(ms));
    return H2_PAL_OK;
}

const h2_pal_time_api_t *h2_esp_platform_time_api(void) {
    static const h2_pal_time_vtable_t vtable = {
        .get_monotonic_ms = esp_time_get_monotonic_ms,
        .get_monotonic_us = esp_time_get_monotonic_us,
        .get_wall_ms = esp_time_get_wall_ms,
        .set_wall_ms = esp_time_set_wall_ms,
        .get_wall_status = esp_time_get_wall_status,
        .sleep_ms = esp_time_sleep_ms,
    };
    static const h2_pal_time_api_t api = {
        .user = NULL,
        .vtable = &vtable,
    };
    return &api;
}
