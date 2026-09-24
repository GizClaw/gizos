#include "h2_esp_platform_core.h"

/* Called by a board runtime owner before it publishes PAL API views. */
h2_pal_result_t h2_esp_platform_atomic_consumers_init(void) {
    h2_pal_result_t rc = h2_esp_platform_wifi_atomic_init();
    if (rc != H2_PAL_OK) return rc;
    rc = h2_esp_platform_webrtc_atomic_init();
    if (rc != H2_PAL_OK) {
        (void)h2_esp_platform_wifi_atomic_shutdown();
        return rc;
    }
    return H2_PAL_OK;
}

h2_pal_result_t h2_esp_platform_atomic_consumers_shutdown(void) {
    h2_pal_result_t rc = h2_esp_platform_wifi_atomic_can_shutdown();
    if (rc != H2_PAL_OK) return rc;
    rc = h2_esp_platform_webrtc_atomic_shutdown();
    if (rc != H2_PAL_OK) return rc;
    return h2_esp_platform_wifi_atomic_shutdown();
}
