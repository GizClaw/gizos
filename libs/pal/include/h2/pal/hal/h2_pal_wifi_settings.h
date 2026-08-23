#ifndef H2_PAL_WIFI_SETTINGS_H
#define H2_PAL_WIFI_SETTINGS_H

#include "h2/pal/core/h2_pal_errors.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define H2_PAL_WIFI_SSID_MAX 32
#define H2_PAL_WIFI_PASSWORD_MAX 64

typedef struct h2_pal_wifi_sta_config {
    char ssid[H2_PAL_WIFI_SSID_MAX + 1];
    size_t ssid_len;
    char password[H2_PAL_WIFI_PASSWORD_MAX + 1];
    size_t password_len;
    uint8_t bssid[6];
    uint8_t bssid_set;
    uint8_t channel;
} h2_pal_wifi_sta_config_t;

typedef struct h2_pal_wifi_settings_api h2_pal_wifi_settings_api_t;
typedef h2_pal_wifi_settings_api_t h2_pal_wifi_settings_t;

typedef int (*h2_pal_wifi_settings_get_saved_sta_config_fn)(
    void *user,
    h2_pal_wifi_sta_config_t *out_config);
typedef int (*h2_pal_wifi_settings_set_saved_sta_config_fn)(
    void *user,
    const h2_pal_wifi_sta_config_t *config);
typedef int (*h2_pal_wifi_settings_clear_saved_sta_config_fn)(void *user);
typedef int (*h2_pal_wifi_settings_has_saved_sta_config_fn)(
    void *user,
    int *out_has_config);

typedef struct h2_pal_wifi_settings_vtable {
    h2_pal_wifi_settings_get_saved_sta_config_fn get_saved_sta_config;
    h2_pal_wifi_settings_set_saved_sta_config_fn set_saved_sta_config;
    h2_pal_wifi_settings_clear_saved_sta_config_fn clear_saved_sta_config;
    h2_pal_wifi_settings_has_saved_sta_config_fn has_saved_sta_config;
} h2_pal_wifi_settings_vtable_t;

struct h2_pal_wifi_settings_api {
    void *user;
    const h2_pal_wifi_settings_vtable_t *vtable;
};

static inline int h2_pal_wifi_settings_validate_sta_config(
    const h2_pal_wifi_sta_config_t *config) {
    if (config == NULL ||
        config->ssid_len == 0u ||
        config->ssid_len > H2_PAL_WIFI_SSID_MAX ||
        config->password_len > H2_PAL_WIFI_PASSWORD_MAX) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    return H2_PAL_OK;
}

static inline int h2_pal_wifi_settings_get_saved_sta_config(
    const h2_pal_wifi_settings_api_t *settings,
    h2_pal_wifi_sta_config_t *out_config) {
    if (settings == NULL || out_config == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (settings->vtable == NULL || settings->vtable->get_saved_sta_config == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return settings->vtable->get_saved_sta_config(settings->user, out_config);
}

static inline int h2_pal_wifi_settings_set_saved_sta_config(
    const h2_pal_wifi_settings_api_t *settings,
    const h2_pal_wifi_sta_config_t *config) {
    if (settings == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (settings->vtable == NULL || settings->vtable->set_saved_sta_config == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    int rc = h2_pal_wifi_settings_validate_sta_config(config);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    return settings->vtable->set_saved_sta_config(settings->user, config);
}

static inline int h2_pal_wifi_settings_clear_saved_sta_config(
    const h2_pal_wifi_settings_api_t *settings) {
    if (settings == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (settings->vtable == NULL || settings->vtable->clear_saved_sta_config == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return settings->vtable->clear_saved_sta_config(settings->user);
}

static inline int h2_pal_wifi_settings_has_saved_sta_config(
    const h2_pal_wifi_settings_api_t *settings,
    int *out_has_config) {
    if (settings == NULL || out_has_config == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_has_config = 0;
    if (settings->vtable != NULL && settings->vtable->has_saved_sta_config != NULL) {
        return settings->vtable->has_saved_sta_config(settings->user, out_has_config);
    }
    if (settings->vtable == NULL || settings->vtable->get_saved_sta_config == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }

    h2_pal_wifi_sta_config_t config;
    int rc = settings->vtable->get_saved_sta_config(settings->user, &config);
    if (rc == H2_PAL_ERR_NOT_FOUND) {
        return H2_PAL_OK;
    }
    if (rc != H2_PAL_OK) {
        return rc;
    }
    *out_has_config = config.ssid_len > 0u ? 1 : 0;
    return H2_PAL_OK;
}

#ifdef __cplusplus
}
#endif

#endif
