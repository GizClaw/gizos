#include "h2_esp_ble_connect_role.h"

#include <assert.h>

int main(void) {
    /* An established outgoing link completes the connect() waiter. */
    assert(h2_esp_ble_connect_event_is_central(0, true, true, true, false));

    /*
     * A peer connecting to a local advertising set while connect() is still
     * pending is a peripheral link; it must not complete the waiter.
     */
    assert(!h2_esp_ble_connect_event_is_central(0, true, false, true, true));
    assert(!h2_esp_ble_connect_event_is_central(0, true, false, true, false));

    /* The descriptor role wins over a stale pending flag. */
    assert(h2_esp_ble_connect_event_is_central(0, true, true, false, false));

    /* A failed connect() attempt still reaches its waiter. */
    assert(h2_esp_ble_connect_event_is_central(13, false, false, true, false));

    /* Failures reported through an advertising set stay peripheral. */
    assert(!h2_esp_ble_connect_event_is_central(13, false, false, true, true));
    assert(!h2_esp_ble_connect_event_is_central(13, false, false, false, false));
    return 0;
}
