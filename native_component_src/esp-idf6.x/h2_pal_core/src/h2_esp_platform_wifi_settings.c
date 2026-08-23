#include "h2_esp_platform_core.h"
#include "h2_esp_platform_safe_call.h"
#include "h2_esp_platform_wifi_internal.h"

#include "esp_wifi.h"

#include <string.h>

static size_t h2_esp_wifi_settings_strnlen(const uint8_t *value, size_t max_len) {
    size_t len = 0u;
    while (len < max_len && value[len] != 0u) {
        len++;
    }
    return len;
}

static int h2_esp_wifi_settings_map_error(esp_err_t err) {
    switch (err) {
    case ESP_OK:
        return H2_PAL_OK;
    case ESP_ERR_INVALID_ARG:
    case ESP_ERR_WIFI_PASSWORD:
        return H2_PAL_ERR_INVALID_ARG;
    case ESP_ERR_WIFI_NOT_INIT:
        return H2_PAL_ERR_UNAVAILABLE;
    case ESP_ERR_WIFI_IF:
        return H2_PAL_ERR_UNSUPPORTED;
    case ESP_ERR_WIFI_MODE:
    case ESP_ERR_WIFI_STATE:
        return H2_PAL_ERR_INVALID_STATE;
    case ESP_ERR_NO_MEM:
        return H2_PAL_ERR_NO_MEMORY;
    case ESP_ERR_WIFI_NVS:
    default:
        return H2_PAL_ERR_IO;
    }
}

static void h2_esp_wifi_settings_copy_from_esp(
    h2_pal_wifi_sta_config_t *out_config,
    const wifi_config_t *config) {
    memset(out_config, 0, sizeof(*out_config));

    out_config->ssid_len = h2_esp_wifi_settings_strnlen(config->sta.ssid, H2_PAL_WIFI_SSID_MAX);
    memcpy(out_config->ssid, config->sta.ssid, out_config->ssid_len);
    out_config->ssid[out_config->ssid_len] = '\0';

    out_config->password_len = h2_esp_wifi_settings_strnlen(config->sta.password, H2_PAL_WIFI_PASSWORD_MAX);
    memcpy(out_config->password, config->sta.password, out_config->password_len);
    out_config->password[out_config->password_len] = '\0';

    out_config->bssid_set = config->sta.bssid_set ? 1u : 0u;
    memcpy(out_config->bssid, config->sta.bssid, sizeof(out_config->bssid));
    out_config->channel = config->sta.channel;
}

static void h2_esp_wifi_settings_copy_to_esp(
    wifi_config_t *out_config,
    const h2_pal_wifi_sta_config_t *config) {
    memset(out_config, 0, sizeof(*out_config));
    memcpy(out_config->sta.ssid, config->ssid, config->ssid_len);
    memcpy(out_config->sta.password, config->password, config->password_len);
    out_config->sta.bssid_set = config->bssid_set != 0u;
    memcpy(out_config->sta.bssid, config->bssid, sizeof(out_config->sta.bssid));
    out_config->sta.channel = config->channel;
}

static int h2_esp_wifi_settings_get_saved_sta_config(
    void *user,
    h2_pal_wifi_sta_config_t *out_config) {
    (void)user;
    if (out_config == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }

    int rc = h2_esp_platform_wifi_ensure_started();
    if (rc != H2_PAL_OK) {
        return rc;
    }

    wifi_config_t config;
    memset(&config, 0, sizeof(config));
    esp_err_t err = esp_wifi_get_config(WIFI_IF_STA, &config);
    if (err != ESP_OK) {
        return h2_esp_wifi_settings_map_error(err);
    }

    h2_esp_wifi_settings_copy_from_esp(out_config, &config);
    return out_config->ssid_len > 0u ? H2_PAL_OK : H2_PAL_ERR_NOT_FOUND;
}

static int h2_esp_wifi_settings_set_saved_sta_config(
    void *user,
    const h2_pal_wifi_sta_config_t *config) {
    (void)user;
    int rc = h2_pal_wifi_settings_validate_sta_config(config);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    rc = h2_esp_platform_wifi_ensure_started();
    if (rc != H2_PAL_OK) {
        return rc;
    }

    wifi_config_t esp_config;
    h2_esp_wifi_settings_copy_to_esp(&esp_config, config);
    return h2_esp_platform_wifi_set_config_safe(WIFI_IF_STA, &esp_config);
}

static int h2_esp_wifi_settings_clear_saved_sta_config(void *user) {
    (void)user;
    int rc = h2_esp_platform_wifi_ensure_started();
    if (rc != H2_PAL_OK) {
        return rc;
    }
    wifi_config_t config;
    memset(&config, 0, sizeof(config));
    return h2_esp_platform_wifi_set_config_safe(WIFI_IF_STA, &config);
}

static int h2_esp_wifi_settings_has_saved_sta_config(
    void *user,
    int *out_has_config) {
    (void)user;
    if (out_has_config == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_has_config = 0;

    int rc = h2_esp_platform_wifi_ensure_started();
    if (rc != H2_PAL_OK) {
        return rc;
    }

    wifi_config_t config;
    memset(&config, 0, sizeof(config));
    esp_err_t err = esp_wifi_get_config(WIFI_IF_STA, &config);
    if (err != ESP_OK) {
        return h2_esp_wifi_settings_map_error(err);
    }

    *out_has_config = h2_esp_wifi_settings_strnlen(config.sta.ssid, H2_PAL_WIFI_SSID_MAX) > 0u ? 1 : 0;
    return H2_PAL_OK;
}

static const h2_pal_wifi_settings_vtable_t s_h2_esp_wifi_settings_vtable = {
    .get_saved_sta_config = h2_esp_wifi_settings_get_saved_sta_config,
    .set_saved_sta_config = h2_esp_wifi_settings_set_saved_sta_config,
    .clear_saved_sta_config = h2_esp_wifi_settings_clear_saved_sta_config,
    .has_saved_sta_config = h2_esp_wifi_settings_has_saved_sta_config,
};

static h2_pal_wifi_settings_t s_h2_esp_wifi_settings = {
    .user = NULL,
    .vtable = &s_h2_esp_wifi_settings_vtable,
};

h2_pal_wifi_settings_t *h2_esp_platform_wifi_settings(void) {
    return &s_h2_esp_wifi_settings;
}
