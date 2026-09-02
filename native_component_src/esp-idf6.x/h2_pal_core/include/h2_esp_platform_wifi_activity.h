#ifndef H2_ESP_PLATFORM_WIFI_ACTIVITY_H
#define H2_ESP_PLATFORM_WIFI_ACTIVITY_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** ESP component-private observer for disruptive Wi-Fi radio transitions. */
typedef void (*h2_esp_platform_wifi_activity_fn)(void *user, bool active);

/**
 * Install the single process-wide Wi-Fi activity observer.
 *
 * This is an ESP component integration hook, not a portable PAL contract.
 * Passing a NULL callback unregisters the current observer. The callback must
 * not block because Wi-Fi operations invoke it inline.
 */
void h2_esp_platform_wifi_set_activity_observer(
    h2_esp_platform_wifi_activity_fn callback,
    void *user);

#ifdef __cplusplus
}
#endif

#endif
