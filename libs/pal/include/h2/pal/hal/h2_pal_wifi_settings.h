#ifndef H2_PAL_WIFI_SETTINGS_H
#define H2_PAL_WIFI_SETTINGS_H

#include "h2/pal/core/h2_pal_errors.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define H2_PAL_WIFI_SSID_MAX 32
#define H2_PAL_WIFI_PASSWORD_MAX 64
#define H2_PAL_WIFI_SAVED_NETWORK_MAX 8

typedef struct h2_pal_wifi_sta_config {
    char ssid[H2_PAL_WIFI_SSID_MAX + 1];
    size_t ssid_len;
    char password[H2_PAL_WIFI_PASSWORD_MAX + 1];
    size_t password_len;
    uint8_t bssid[6];
    uint8_t bssid_set;
    uint8_t channel;
} h2_pal_wifi_sta_config_t;

typedef struct h2_pal_wifi_saved_network {
  h2_pal_wifi_sta_config_t config;
  uint32_t last_connected_seq;
} h2_pal_wifi_saved_network_t;

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

typedef int (*h2_pal_wifi_settings_list_saved_sta_configs_fn)(
    void *user, h2_pal_wifi_saved_network_t *out_configs, size_t capacity,
    size_t *out_count);
typedef int (*h2_pal_wifi_settings_remove_saved_sta_config_fn)(void *user,
                                                               const char *ssid,
                                                               size_t ssid_len);

typedef struct h2_pal_wifi_settings_vtable {
  /** Return the most recently connected saved network. */
  h2_pal_wifi_settings_get_saved_sta_config_fn get_saved_sta_config;
  /** Insert/update by SSID, move to front, evict least recent when full.
   * The complete list update must be atomic on failure. */
  h2_pal_wifi_settings_set_saved_sta_config_fn set_saved_sta_config;
  /** Erase all saved networks without changing the active connection. */
  h2_pal_wifi_settings_clear_saved_sta_config_fn clear_saved_sta_config;
  h2_pal_wifi_settings_has_saved_sta_config_fn has_saved_sta_config;
  /** Optional; enumerate most recent first, reporting entries copied. */
  h2_pal_wifi_settings_list_saved_sta_configs_fn list_saved_sta_configs;
  /** Optional; remove exactly one SSID, or return NOT_FOUND. */
  h2_pal_wifi_settings_remove_saved_sta_config_fn remove_saved_sta_config;
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

/** A zero capacity is valid and reports zero entries, not the total size. */
static inline int h2_pal_wifi_settings_list_saved_sta_configs(
    const h2_pal_wifi_settings_api_t *settings,
    h2_pal_wifi_saved_network_t *out_configs, size_t capacity,
    size_t *out_count) {
  if (out_count == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  *out_count = 0u;
  if (settings == NULL || (capacity > 0u && out_configs == NULL))
    return H2_PAL_ERR_INVALID_ARG;
  if (settings->vtable != NULL &&
      settings->vtable->list_saved_sta_configs != NULL)
    return settings->vtable->list_saved_sta_configs(settings->user, out_configs,
                                                    capacity, out_count);
  h2_pal_wifi_sta_config_t config;
  memset(&config, 0, sizeof(config));
  int rc = h2_pal_wifi_settings_get_saved_sta_config(settings, &config);
  if (rc == H2_PAL_ERR_NOT_FOUND)
    return H2_PAL_OK;
  if (rc == H2_PAL_OK && capacity > 0u) {
    out_configs[0].config = config;
    out_configs[0].last_connected_seq = 0u;
    *out_count = 1u;
  }
  return rc;
}

static inline int h2_pal_wifi_settings_remove_saved_sta_config(
    const h2_pal_wifi_settings_api_t *settings, const char *ssid,
    size_t ssid_len) {
  if (settings == NULL || ssid == NULL || ssid_len == 0u ||
      ssid_len > H2_PAL_WIFI_SSID_MAX)
    return H2_PAL_ERR_INVALID_ARG;
  if (settings->vtable != NULL &&
      settings->vtable->remove_saved_sta_config != NULL)
    return settings->vtable->remove_saved_sta_config(settings->user, ssid,
                                                     ssid_len);
  h2_pal_wifi_sta_config_t config;
  memset(&config, 0, sizeof(config));
  int rc = h2_pal_wifi_settings_get_saved_sta_config(settings, &config);
  if (rc != H2_PAL_OK)
    return rc;
  if (config.ssid_len != ssid_len || memcmp(config.ssid, ssid, ssid_len) != 0)
    return H2_PAL_ERR_NOT_FOUND;
  return h2_pal_wifi_settings_clear_saved_sta_config(settings);
}

#ifdef __cplusplus
}
#endif

#endif
