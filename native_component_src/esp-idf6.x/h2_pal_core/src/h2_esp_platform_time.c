#include "h2_esp_platform_core.h"

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <sys/time.h>

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
    if (gettimeofday(&tv, NULL) != 0) {
        return H2_PAL_ERR_UNAVAILABLE;
    }
    *out_ms = ((uint64_t)tv.tv_sec * 1000u) + ((uint64_t)tv.tv_usec / 1000u);
    return H2_PAL_OK;
}

static h2_pal_time_wall_status_t s_esp_wall_status;

static h2_pal_result_t esp_time_set_wall_ms(void *user, uint64_t wall_ms) {
    (void)user;
    if (wall_ms == 0u) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    struct timeval tv = {
        .tv_sec = (time_t)(wall_ms / 1000u),
        .tv_usec = (suseconds_t)((wall_ms % 1000u) * 1000u),
    };
    if (settimeofday(&tv, NULL) != 0) {
        return H2_PAL_ERR_IO;
    }
    s_esp_wall_status.valid = 1u;
    s_esp_wall_status.source = H2_PAL_TIME_WALL_SOURCE_NTP;
    return H2_PAL_OK;
}

static h2_pal_result_t esp_time_get_wall_status(void *user, h2_pal_time_wall_status_t *out_status) {
    (void)user;
    if (out_status == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_status = s_esp_wall_status;
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
