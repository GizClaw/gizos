#include "h2_esp_platform_core.h"
#include "h2_esp_wifi_saved_record.h"

#include <string.h>

static int h2_esp_wifi_settings_get_saved_sta_config(
    void *user,
    h2_pal_wifi_sta_config_t *out_config) {
    (void)user;
    if (out_config == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    memset(out_config, 0, sizeof(*out_config));
    int rc = h2_esp_platform_wifi_ensure_started();
    if (rc != H2_PAL_OK) {
        return rc;
    }
    uint8_t record[H2_ESP_WIFI_SAVED_RECORD_SIZE] = {0};
    rc = h2_esp_platform_wifi_saved_record(record, false);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    if (record[0] != 1u || record[1] > H2_PAL_WIFI_SSID_MAX ||
        record[2] > H2_PAL_WIFI_PASSWORD_MAX || record[3] > 1u) {
        return H2_PAL_ERR_IO;
    }
    if (record[1] == 0u) {
        return H2_PAL_ERR_NOT_FOUND;
    }
    out_config->ssid_len = record[1];
    out_config->password_len = record[2];
    out_config->bssid_set = record[3];
    out_config->channel = record[4];
    memcpy(out_config->bssid, record + 5u, sizeof(out_config->bssid));
    memcpy(out_config->ssid, record + 11u, out_config->ssid_len);
    memcpy(out_config->password, record + 43u, out_config->password_len);
    return H2_PAL_OK;
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
    uint8_t record[H2_ESP_WIFI_SAVED_RECORD_SIZE] = {1u};
    record[1] = (uint8_t)config->ssid_len;
    record[2] = (uint8_t)config->password_len;
    record[3] = config->bssid_set != 0u ? 1u : 0u;
    record[4] = config->channel;
    memcpy(record + 5u, config->bssid, sizeof(config->bssid));
    memcpy(record + 11u, config->ssid, config->ssid_len);
    memcpy(record + 43u, config->password, config->password_len);
    return h2_esp_platform_wifi_saved_record(record, true);
}

static int h2_esp_wifi_settings_clear_saved_sta_config(void *user) {
    (void)user;
    int rc = h2_esp_platform_wifi_ensure_started();
    if (rc != H2_PAL_OK) {
        return rc;
    }
    /* Persist an empty record so legacy driver credentials cannot be imported
     * again after forget or restart. The active connection stays untouched. */
    uint8_t record[H2_ESP_WIFI_SAVED_RECORD_SIZE] = {1u};
    return h2_esp_platform_wifi_saved_record(record, true);
}

static int h2_esp_wifi_settings_has_saved_sta_config(void *user, int *out_has_config) {
    if (out_has_config == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_has_config = 0;
    h2_pal_wifi_sta_config_t config;
    int rc = h2_esp_wifi_settings_get_saved_sta_config(user, &config);
    if (rc == H2_PAL_ERR_NOT_FOUND) {
        return H2_PAL_OK;
    }
    if (rc == H2_PAL_OK) {
        *out_has_config = 1;
    }
    return rc;
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
