#include "h2_esp_platform_core.h"

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <sys/time.h>

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
    /* A pre-epoch or malformed reading would wrap when cast to uint64_t and
     * could pass the plausibility floor as a huge value. */
    if (tv.tv_sec < 0 || tv.tv_usec < 0 || tv.tv_usec >= 1000000) {
        return H2_PAL_ERR_UNAVAILABLE;
    }
    *out_ms = ((uint64_t)tv.tv_sec * 1000u) + ((uint64_t)tv.tv_usec / 1000u);
    return H2_PAL_OK;
}

/* Validity is read out of the clock itself, so nothing has to be remembered
 * across program starts. The RTC timer keeps counting through deep sleep and
 * software resets, so a plausible reading means the clock still carries a
 * calibration and the UI can show the time while charging. A power-on or
 * brownout reset restarts the RTC timer near the epoch, which reads as
 * implausible and keeps the clock uncalibrated until the next set_wall_ms.
 * Earliest reading accepted as calibrated: 2020-01-01T00:00:00Z. */
#define H2_ESP_WALL_MIN_VALID_MS 1577836800000ull

static h2_pal_result_t esp_time_set_wall_ms(void *user, uint64_t wall_ms) {
    (void)user;
    if (wall_ms == 0u) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    struct timeval tv = {
        .tv_sec = (time_t)(wall_ms / 1000u),
        .tv_usec = (suseconds_t)((wall_ms % 1000u) * 1000u),
    };
    return h2_esp_settimeofday(&tv) == 0 ? H2_PAL_OK : H2_PAL_ERR_IO;
}

static h2_pal_result_t esp_time_get_wall_status(void *user, h2_pal_time_wall_status_t *out_status) {
    (void)user;
    if (out_status == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    uint64_t wall_ms = 0u;
    /* A clock that cannot be read is an error, not an uncalibrated clock. */
    const h2_pal_result_t rc = esp_time_get_wall_ms(user, &wall_ms);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    out_status->valid = wall_ms >= H2_ESP_WALL_MIN_VALID_MS;
    out_status->source = out_status->valid
                             ? H2_PAL_TIME_WALL_SOURCE_RTC
                             : H2_PAL_TIME_WALL_SOURCE_BOOT_DEFAULT;
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
