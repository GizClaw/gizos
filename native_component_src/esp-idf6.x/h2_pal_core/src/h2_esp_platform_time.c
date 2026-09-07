#include "h2_esp_platform_core.h"

#include "esp_attr.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <sys/time.h>
#include <stdatomic.h>
#include <stdbool.h>

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

/* Validity lives in two places. The atomic is the fast path and is cleared by
 * every program start. The RTC slow-memory marker survives deep sleep and
 * software resets together with the RTC-timer backed system clock, so a
 * charger wake or a self-reboot keeps the last calibrated wall time. Power-on
 * and brownout resets leave RTC memory undefined and restart the RTC timer, so
 * the marker is dropped there and the clock waits for the next calibration. */
#define H2_ESP_WALL_RETAINED_MAGIC 0x57414c4cu /* "WALL" */
/* Earliest wall time accepted from a retained clock: 2020-01-01T00:00:00Z. */
#define H2_ESP_WALL_RETAINED_MIN_MS 1577836800000ull

static atomic_bool s_esp_wall_valid;
static atomic_bool s_esp_wall_probed;
static RTC_NOINIT_ATTR uint32_t s_esp_wall_retained_magic;

static bool esp_time_reset_keeps_rtc(void) {
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON:
    case ESP_RST_BROWNOUT:
    case ESP_RST_UNKNOWN:
        return false;
    default:
        return true;
    }
}

/* Runs once per program start, on the first validity query. */
static void esp_time_probe_retained(void) {
    if (atomic_exchange_explicit(&s_esp_wall_probed, 1, memory_order_acq_rel)) {
        return;
    }
    if (!esp_time_reset_keeps_rtc()) {
        s_esp_wall_retained_magic = 0u;
        return;
    }
    if (s_esp_wall_retained_magic != H2_ESP_WALL_RETAINED_MAGIC) {
        return;
    }
    struct timeval tv;
    if (h2_esp_gettimeofday(&tv) != 0 ||
        (uint64_t)tv.tv_sec * 1000u < H2_ESP_WALL_RETAINED_MIN_MS) {
        s_esp_wall_retained_magic = 0u;
        return;
    }
    atomic_store_explicit(&s_esp_wall_valid, 1, memory_order_release);
}

#if defined(H2_ESP_TIME_TEST)
/* Emulates a program restart: process state is lost, RTC memory is kept. */
void h2_esp_platform_time_test_restart(void) {
    atomic_store_explicit(&s_esp_wall_valid, 0, memory_order_release);
    atomic_store_explicit(&s_esp_wall_probed, 0, memory_order_release);
}

/* Emulates a power-on reset: RTC memory content is undefined. */
void h2_esp_platform_time_test_scramble_rtc(uint32_t value) {
    s_esp_wall_retained_magic = value;
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
    /* A fresh set outranks whatever the probe would have concluded. */
    atomic_store_explicit(&s_esp_wall_probed, 1, memory_order_release);
    s_esp_wall_retained_magic = H2_ESP_WALL_RETAINED_MAGIC;
    atomic_store_explicit(&s_esp_wall_valid, 1, memory_order_release);
    return H2_PAL_OK;
}

static h2_pal_result_t esp_time_get_wall_status(void *user, h2_pal_time_wall_status_t *out_status) {
    (void)user;
    if (out_status == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    esp_time_probe_retained();
    out_status->valid = atomic_load_explicit(&s_esp_wall_valid, memory_order_acquire);
    out_status->source = out_status->valid ? H2_PAL_TIME_WALL_SOURCE_USER
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
