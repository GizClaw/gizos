#include "h2_runtime_internal.h"

#include <string.h>

static const h2_pal_wifi_sta_api_t *backend(h2_runtime_t *runtime) {
    return &runtime->private_state->wifi_sta_backend;
}

static int get_status(void *user, h2_pal_wifi_sta_status_t *out) {
    return h2_pal_wifi_sta_get_status(backend(user), out);
}

static int scan(void *user, const h2_pal_wifi_scan_request_t *request,
                h2_pal_wifi_scan_result_fn result, void *result_user,
                uint32_t timeout_ms) {
    return h2_pal_wifi_sta_scan(backend(user), request, result, result_user, timeout_ms);
}

static int get_mac(void *user, uint8_t out[6]) {
    return h2_pal_wifi_sta_get_mac(backend(user), out);
}

static int set_power_save(void *user, h2_pal_wifi_power_save_t mode) {
    return h2_pal_wifi_sta_set_power_save(backend(user), mode);
}

static int disconnect(void *user) {
    h2_runtime_t *runtime = user;
    if (atomic_flag_test_and_set(&runtime->private_state->wifi_connect_busy))
        return H2_PAL_ERR_BUSY;
    int rc = h2_pal_wifi_sta_disconnect(backend(runtime));
    atomic_flag_clear(&runtime->private_state->wifi_connect_busy);
    return rc;
}

static int remaining_budget(h2_runtime_t *runtime, uint64_t started,
                            uint32_t budget, uint32_t *remaining) {
    uint64_t now = 0;
    int rc = h2_pal_time_get_monotonic_ms(runtime->time, &now);
    if (rc != H2_PAL_OK)
        return rc;
    if (now < started || now - started >= budget)
        return H2_PAL_ERR_TIMEOUT;
    *remaining = budget - (uint32_t)(now - started);
    return H2_PAL_OK;
}

static int connect_and_save(h2_runtime_t *runtime,
                            const h2_pal_wifi_sta_config_t *config,
                            uint32_t timeout_ms) {
    /* Refuse before changing networks if durable storage is unavailable. */
    const h2_pal_wifi_settings_api_t *settings = runtime->wifi_settings;
    if (!settings || !settings->vtable ||
        !settings->vtable->set_saved_sta_config)
        return H2_PAL_ERR_UNSUPPORTED;
    h2_pal_wifi_sta_config_t saved = {0};
    int rc = h2_pal_wifi_settings_get_saved_sta_config(settings, &saved);
    if (rc != H2_PAL_OK && rc != H2_PAL_ERR_NOT_FOUND)
        return rc;
    memset(&saved, 0, sizeof(saved));
    uint64_t started = 0;
    rc = h2_pal_time_get_monotonic_ms(runtime->time, &started);
    if (rc != H2_PAL_OK)
        return rc;
    uint32_t budget = timeout_ms ? timeout_ms : 15000u;
    /* Re-authenticate even for the same SSID: cached association is not
     * evidence that a newly supplied password works. */
    h2_pal_wifi_sta_status_t previous = {0};
    rc = h2_pal_wifi_sta_get_status(backend(runtime), &previous);
    if (rc != H2_PAL_OK)
        return rc;
    if (previous.state == H2_PAL_WIFI_STA_STATE_CONNECTED ||
        previous.state == H2_PAL_WIFI_STA_STATE_GOT_IP) {
        rc = h2_pal_wifi_sta_disconnect(backend(runtime));
        if (rc != H2_PAL_OK)
            return rc;
    }
    uint32_t remaining = 0;
    rc = remaining_budget(runtime, started, budget, &remaining);
    if (rc != H2_PAL_OK)
        return rc;
    rc = h2_pal_wifi_sta_connect(backend(runtime), config, remaining);
    while (rc == H2_PAL_OK) {
        h2_pal_wifi_sta_status_t status = {0};
        rc = h2_pal_wifi_sta_get_status(backend(runtime), &status);
        if (rc != H2_PAL_OK)
            break;
        rc = remaining_budget(runtime, started, budget, &remaining);
        if (rc != H2_PAL_OK)
            break;
        if (status.state == H2_PAL_WIFI_STA_STATE_GOT_IP && status.ip_valid &&
            status.ssid_len == config->ssid_len &&
            memcmp(status.ssid, config->ssid, config->ssid_len) == 0) {
            return h2_pal_wifi_settings_set_saved_sta_config(settings, config);
        }
        if (status.state == H2_PAL_WIFI_STA_STATE_FAILED ||
            status.state == H2_PAL_WIFI_STA_STATE_DISCONNECTED)
            return H2_PAL_ERR_UNAVAILABLE;
        rc = h2_pal_time_sleep_ms(runtime->time, remaining < 20u ? remaining : 20u);
    }
    return rc;
}

static int connect(void *user, const h2_pal_wifi_sta_config_t *config,
                   uint32_t timeout_ms) {
    h2_runtime_t *runtime = user;
    int rc = h2_pal_wifi_settings_validate_sta_config(config);
    if (rc != H2_PAL_OK)
        return rc;
    if (atomic_flag_test_and_set(&runtime->private_state->wifi_connect_busy))
        return H2_PAL_ERR_BUSY;
    rc = timeout_ms == 0u
        ? h2_pal_wifi_sta_connect(backend(runtime), config, 0u)
        : connect_and_save(runtime, config, timeout_ms);
    atomic_flag_clear(&runtime->private_state->wifi_connect_busy);
    return rc;
}

void h2_runtime_wifi_bind(h2_runtime_t *runtime) {
    static const h2_pal_wifi_sta_vtable_t vtable = {
        .get_status = get_status,
        .scan = scan,
        .connect = connect,
        .disconnect = disconnect,
        .get_mac = get_mac,
        .set_power_save = set_power_save,
    };
    h2_runtime_private_t *state = runtime->private_state;
    state->wifi_sta_backend = state->wifi_sta_proxy;
    state->wifi_connect_busy = (atomic_flag)ATOMIC_FLAG_INIT;
    state->wifi_sta_proxy = (h2_pal_wifi_sta_api_t){runtime, &vtable};
}

h2_pal_result_t h2_runtime_wifi_connect_saved(h2_runtime_t *runtime,
                                             uint32_t timeout_ms) {
    if (!h2_runtime_ready(runtime))
        return H2_PAL_ERR_INVALID_ARG;
    h2_pal_wifi_sta_config_t config = {0};
    int rc = h2_pal_wifi_settings_get_saved_sta_config(runtime->wifi_settings, &config);
    if (rc == H2_PAL_OK)
        rc = h2_pal_wifi_sta_connect(runtime->wifi_sta, &config, timeout_ms ? timeout_ms : 15000u);
    memset(&config, 0, sizeof(config));
    return rc;
}
