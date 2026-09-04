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
 * Passing a NULL callback unregisters the current observer, and installing one
 * reports the current state immediately so an observer cannot miss an
 * operation that is already in flight.
 *
 * Overlapping operations are reference counted: the callback observes true
 * when the first disruptive operation starts and false only after the last one
 * has finished. It runs inline on the task that called the Wi-Fi operation, so
 * it must not block and must not re-enter the Wi-Fi API.
 */
void h2_esp_platform_wifi_set_activity_observer(
    h2_esp_platform_wifi_activity_fn callback,
    void *user);

#ifdef __cplusplus
}
#endif

#endif
