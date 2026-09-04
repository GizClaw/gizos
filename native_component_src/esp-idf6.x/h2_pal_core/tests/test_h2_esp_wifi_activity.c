#include "h2_esp_wifi_activity_tracker.h"

#include <assert.h>

int main(void) {
    h2_esp_wifi_activity_tracker_t tracker = {0};
    h2_esp_wifi_activity_tracker_clear(&tracker);
    assert(!h2_esp_wifi_activity_tracker_is_active(&tracker));

    /* A single operation reports both transitions. */
    assert(h2_esp_wifi_activity_tracker_enter(&tracker));
    assert(h2_esp_wifi_activity_tracker_is_active(&tracker));
    assert(h2_esp_wifi_activity_tracker_exit(&tracker));
    assert(!h2_esp_wifi_activity_tracker_is_active(&tracker));

    /* Overlapping operations report activity once, and only the last one to
     * finish reports the end: a scan returning while a connect is still
     * running must not resume advertising. */
    assert(h2_esp_wifi_activity_tracker_enter(&tracker));
    assert(!h2_esp_wifi_activity_tracker_enter(&tracker));
    assert(!h2_esp_wifi_activity_tracker_enter(&tracker));
    assert(!h2_esp_wifi_activity_tracker_exit(&tracker));
    assert(h2_esp_wifi_activity_tracker_is_active(&tracker));
    assert(!h2_esp_wifi_activity_tracker_exit(&tracker));
    assert(h2_esp_wifi_activity_tracker_is_active(&tracker));
    assert(h2_esp_wifi_activity_tracker_exit(&tracker));
    assert(!h2_esp_wifi_activity_tracker_is_active(&tracker));

    /* An unbalanced exit cannot underflow the counter or fabricate a
     * transition for the next operation. */
    assert(!h2_esp_wifi_activity_tracker_exit(&tracker));
    assert(tracker.depth == 0);
    assert(h2_esp_wifi_activity_tracker_enter(&tracker));
    assert(h2_esp_wifi_activity_tracker_exit(&tracker));

    return 0;
}
