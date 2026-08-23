#include "h2_runtime_internal.h"

#include <string.h>

static h2_pal_result_t unsupported_system_state(
    const h2_runtime_t *runtime,
    void *out_state,
    size_t out_state_size) {
    if (!h2_runtime_ready(runtime) || out_state == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    memset(out_state, 0, out_state_size);
    return H2_PAL_ERR_UNSUPPORTED;
}

h2_pal_result_t h2_runtime_system_state_wifi_sta(
    const h2_runtime_t *runtime,
    h2_runtime_system_wifi_sta_state_t *out_state) {
    return unsupported_system_state(runtime, out_state, sizeof(*out_state));
}

h2_pal_result_t h2_runtime_system_state_wifi_ap(
    const h2_runtime_t *runtime,
    h2_runtime_system_wifi_ap_state_t *out_state) {
    return unsupported_system_state(runtime, out_state, sizeof(*out_state));
}

h2_pal_result_t h2_runtime_system_state_gpio_irq(
    const h2_runtime_t *runtime,
    h2_runtime_system_gpio_irq_state_t *out_state) {
    return unsupported_system_state(runtime, out_state, sizeof(*out_state));
}

h2_pal_result_t h2_runtime_system_state_ble(
    const h2_runtime_t *runtime,
    h2_runtime_system_ble_state_t *out_state) {
    return unsupported_system_state(runtime, out_state, sizeof(*out_state));
}

h2_pal_result_t h2_runtime_system_state_modem(
    const h2_runtime_t *runtime,
    h2_runtime_system_modem_state_t *out_state) {
    return unsupported_system_state(runtime, out_state, sizeof(*out_state));
}

h2_pal_result_t h2_runtime_system_state_mqtt(
    const h2_runtime_t *runtime,
    h2_runtime_system_mqtt_state_t *out_state) {
    return unsupported_system_state(runtime, out_state, sizeof(*out_state));
}

h2_pal_result_t h2_runtime_system_state_webrtc(
    const h2_runtime_t *runtime,
    h2_runtime_system_webrtc_state_t *out_state) {
    return unsupported_system_state(runtime, out_state, sizeof(*out_state));
}
