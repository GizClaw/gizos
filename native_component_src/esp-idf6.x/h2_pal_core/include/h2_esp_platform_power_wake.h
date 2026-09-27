#ifndef H2_ESP_PLATFORM_POWER_WAKE_H
#define H2_ESP_PLATFORM_POWER_WAKE_H

#include "h2/pal/hal/h2_pal_power.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Wake delay used when no deep-sleep wake timer is armed. */
#define H2_ESP_POWER_DEEP_SLEEP_DEFAULT_WAKE_US (1000ULL * 1000ULL)

/* Timer delay passed to esp_sleep_enable_timer_wakeup(): the armed delay, or
 * the provider default when armed_ms is 0. */
static inline uint64_t h2_esp_power_deep_sleep_wake_us(uint32_t armed_ms) {
    return armed_ms != 0u ? (uint64_t)armed_ms * 1000ULL
                          : H2_ESP_POWER_DEEP_SLEEP_DEFAULT_WAKE_US;
}

/* Boot source for the current boot: TIMER only when the chip reset out of
 * deep sleep and the timer is among the wake causes. */
static inline h2_pal_power_boot_source_t h2_esp_power_wake_boot_source(
    int reset_from_deep_sleep,
    int timer_wake_cause) {
    return reset_from_deep_sleep && timer_wake_cause
        ? H2_PAL_POWER_BOOT_SOURCE_TIMER
        : H2_PAL_POWER_BOOT_SOURCE_UNKNOWN;
}

#ifdef __cplusplus
}
#endif

#endif
