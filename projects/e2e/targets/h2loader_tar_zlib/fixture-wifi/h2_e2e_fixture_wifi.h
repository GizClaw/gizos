#ifndef H2_E2E_FIXTURE_WIFI_H
#define H2_E2E_FIXTURE_WIFI_H

#include "h2/pal/hal/h2_pal_wifi.h"
#include <stddef.h>

#if defined(H2_E2E_WIFI_SSID) != defined(H2_E2E_WIFI_PASSWORD)
#error Explicit fixture Wi-Fi requires both SSID and password
#endif

/* Native entry input only; NULL retains the original saved-network setup.
 * The borrowed immutable config is consumed by non-saving PAL connect. */
static inline const h2_pal_wifi_sta_config_t *h2_e2e_fixture_wifi(void) {
#if defined(H2_E2E_WIFI_SSID)
    static const h2_pal_wifi_sta_config_t wifi = {
        .ssid = H2_E2E_WIFI_SSID,
        .ssid_len = sizeof(H2_E2E_WIFI_SSID) - 1u,
        .password = H2_E2E_WIFI_PASSWORD,
        .password_len = sizeof(H2_E2E_WIFI_PASSWORD) - 1u,
    };
    return &wifi;
#else
    return NULL;
#endif
}
#endif
