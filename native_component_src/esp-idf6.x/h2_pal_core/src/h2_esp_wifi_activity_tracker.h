#ifndef H2_ESP_WIFI_ACTIVITY_TRACKER_H
#define H2_ESP_WIFI_ACTIVITY_TRACKER_H

#include <stdbool.h>

/**
 * Reference counter for in-flight disruptive Wi-Fi radio operations.
 *
 * Scan, connect and disconnect are not serialized against each other, so the
 * observer must not treat the first one to return as the end of radio
 * activity. Enter/exit report only the transitions out of and back into the
 * idle state, so a consumer sees active once while any operation is running.
 */
typedef struct h2_esp_wifi_activity_tracker {
    int depth;
} h2_esp_wifi_activity_tracker_t;

static inline void h2_esp_wifi_activity_tracker_clear(
    h2_esp_wifi_activity_tracker_t *tracker) {
    tracker->depth = 0;
}

static inline bool h2_esp_wifi_activity_tracker_is_active(
    const h2_esp_wifi_activity_tracker_t *tracker) {
    return tracker->depth > 0;
}

/** Returns true only for the operation that started radio activity. */
static inline bool h2_esp_wifi_activity_tracker_enter(
    h2_esp_wifi_activity_tracker_t *tracker) {
    tracker->depth += 1;
    return tracker->depth == 1;
}

/**
 * Returns true only for the operation that ended radio activity. An
 * unbalanced exit is ignored instead of driving the counter negative.
 */
static inline bool h2_esp_wifi_activity_tracker_exit(
    h2_esp_wifi_activity_tracker_t *tracker) {
    if (tracker->depth <= 0) {
        return false;
    }
    tracker->depth -= 1;
    return tracker->depth == 0;
}

#endif
