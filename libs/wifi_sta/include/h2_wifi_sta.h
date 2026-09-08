#ifndef H2_WIFI_STA_H
#define H2_WIFI_STA_H

#include "h2/pal/hal/h2_pal_wifi.h"
#include "h2/pal/os/h2_pal_time.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Borrowed provider dependencies, valid for the complete synchronous call. */
typedef struct h2_wifi_sta_dependencies {
    const h2_pal_wifi_sta_api_t *sta;
    const h2_pal_wifi_settings_api_t *settings;
    const h2_pal_time_api_t *time;
} h2_wifi_sta_dependencies_t;

/**
 * @brief Shared implementation of the PAL connect_and_save contract.
 * @param deps Raw provider operations, settings and monotonic clock.
 * @param config Borrowed target credentials; never retained.
 * @param timeout_ms Nonzero association/DHCP budget in milliseconds.
 * @return PAL result, including the storage failure without hiding it.
 * Provider must serialize connection mutations across this entire call and
 * supply raw operations that do not reacquire that admission gate. No task,
 * allocation or global state is created. Settings replacement must be atomic.
 */
int h2_wifi_sta_connect_and_save(const h2_wifi_sta_dependencies_t *deps,
                                const h2_pal_wifi_sta_config_t *config,
                                uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
#endif
