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

h2_pal_result_t h2_runtime_system_state_init(h2_runtime_t *runtime) {
    if (!h2_runtime_ready(runtime)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    const h2_pal_mutex_config_t config = {
        .name = "h2-runtime-system-state",
        .allocator = runtime->mem,
        .flags = H2_PAL_MUTEX_FLAG_NONE,
    };
    h2_pal_result_t rc = h2_pal_mutex_create(
        runtime->sync, &config, &runtime->private_state->system_state.mutex);
    if (rc == H2_PAL_ERR_UNSUPPORTED) {
        /*
         * A Runtime without a Sync provider has no tasks to publish from
         * either, so the snapshot has a single accessor and needs no lock.
         * Requiring one here would stop such a Runtime from initializing at
         * all, which it did before this snapshot existed.
         */
        runtime->private_state->system_state.mutex = NULL;
        return H2_PAL_OK;
    }
    return rc;
}

void h2_runtime_system_state_release(h2_runtime_t *runtime) {
    if (runtime == NULL || runtime->private_state == NULL ||
        runtime->private_state->system_state.mutex == NULL) {
        return;
    }
    (void)h2_pal_mutex_destroy(
        runtime->sync, runtime->private_state->system_state.mutex);
    runtime->private_state->system_state.mutex = NULL;
}

void h2_runtime_system_state_publish_wifi_sta(
    h2_runtime_t *runtime,
    const h2_runtime_system_wifi_sta_state_t *state) {
    if (!h2_runtime_ready(runtime) || state == NULL) {
        return;
    }
    h2_runtime_system_state_publication_t *pub =
        &runtime->private_state->system_state;
    if (pub->mutex == NULL) {
        pub->wifi_sta = *state;
        return;
    }
    if (h2_pal_mutex_lock(runtime->sync, pub->mutex) != H2_PAL_OK) {
        return;
    }
    pub->wifi_sta = *state;
    (void)h2_pal_mutex_unlock(runtime->sync, pub->mutex);
}

h2_pal_result_t h2_runtime_system_state_wifi_sta(
    const h2_runtime_t *runtime,
    h2_runtime_system_wifi_sta_state_t *out_state) {
    if (!h2_runtime_ready(runtime) || out_state == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    h2_runtime_system_state_publication_t *pub =
        &runtime->private_state->system_state;
    if (pub->mutex == NULL) {
        *out_state = pub->wifi_sta;
        return H2_PAL_OK;
    }
    h2_pal_result_t rc = h2_pal_mutex_lock(runtime->sync, pub->mutex);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    *out_state = pub->wifi_sta;
    (void)h2_pal_mutex_unlock(runtime->sync, pub->mutex);
    return H2_PAL_OK;
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
