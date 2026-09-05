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

void h2_runtime_system_state_publish_wifi_sta(
    h2_runtime_t *runtime,
    const h2_runtime_system_wifi_sta_state_t *state) {
    if (!h2_runtime_ready(runtime) || state == NULL) {
        return;
    }
    h2_runtime_system_state_publication_t *pub = &runtime->private_state->system_state;
    /*
     * Odd marks the snapshot in flight, so a reader that sees an odd counter
     * or a counter that moved knows it copied a torn value and retries. Only
     * the system-event ingest publishes, so no writer lock is needed.
     */
    unsigned int sequence =
        atomic_load_explicit(&pub->sequence, memory_order_relaxed);
    atomic_store_explicit(&pub->sequence, sequence + 1u, memory_order_relaxed);
    atomic_thread_fence(memory_order_release);
    pub->wifi_sta = *state;
    atomic_store_explicit(&pub->sequence, sequence + 2u, memory_order_release);
}

h2_pal_result_t h2_runtime_system_state_wifi_sta(
    const h2_runtime_t *runtime,
    h2_runtime_system_wifi_sta_state_t *out_state) {
    if (!h2_runtime_ready(runtime) || out_state == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    const h2_runtime_system_state_publication_t *pub =
        &runtime->private_state->system_state;
    for (;;) {
        unsigned int before =
            atomic_load_explicit(&pub->sequence, memory_order_acquire);
        if ((before & 1u) != 0u) {
            continue;
        }
        *out_state = pub->wifi_sta;
        atomic_thread_fence(memory_order_acquire);
        unsigned int after =
            atomic_load_explicit(&pub->sequence, memory_order_relaxed);
        if (before == after) {
            return H2_PAL_OK;
        }
    }
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
