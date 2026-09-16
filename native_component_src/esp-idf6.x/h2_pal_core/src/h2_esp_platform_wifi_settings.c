#include "h2_esp_platform_core.h"
#include "h2_esp_wifi_saved_record.h"

#include <string.h>

int h2_esp_wifi_saved_import(
    const uint8_t record[H2_ESP_WIFI_SAVED_RECORD_SIZE],
    uint8_t blob[H2_WIFI_SAVED_LIST_BLOB_SIZE]) {
  if (record[0] != 1u || record[1] > H2_PAL_WIFI_SSID_MAX ||
      record[2] > H2_PAL_WIFI_PASSWORD_MAX || record[3] > 1u)
    return H2_PAL_ERR_IO;
  h2_pal_wifi_saved_network_t saved = {0};
  h2_pal_wifi_sta_config_t *config = &saved.config;
  config->ssid_len = record[1];
  config->password_len = record[2];
  config->bssid_set = record[3];
  config->channel = record[4];
  memcpy(config->bssid, record + 5u, 6u);
  memcpy(config->ssid, record + 11u, config->ssid_len);
  memcpy(config->password, record + 43u, config->password_len);
  saved.last_connected_seq = 1u;
  return h2_wifi_saved_list_encode(&saved, config->ssid_len ? 1u : 0u, blob,
                                   H2_WIFI_SAVED_LIST_BLOB_SIZE) == H2_PAL_OK
             ? H2_PAL_OK
             : H2_PAL_ERR_IO;
}

typedef struct {
  enum { READ, INSERT, REMOVE, CLEAR } op;
  const h2_pal_wifi_sta_config_t *config;
  const char *ssid;
  size_t ssid_len;
  h2_pal_wifi_saved_network_t *out;
  size_t capacity;
  size_t *count;
} saved_request_t;

static int saved_transaction(void *context) {
  saved_request_t *request = context;
  uint8_t blob[H2_WIFI_SAVED_LIST_BLOB_SIZE];
  h2_pal_wifi_saved_network_t saved[H2_PAL_WIFI_SAVED_NETWORK_MAX] = {0};
  size_t count = 0;
  int rc = H2_PAL_OK;
  if (request->op != CLEAR) {
    rc = h2_esp_platform_wifi_saved_record(blob, false);
    if (rc != H2_PAL_OK)
      return rc;
    rc = h2_wifi_saved_list_decode(blob, sizeof(blob), saved, &count);
    if (rc != H2_PAL_OK)
      return H2_PAL_ERR_IO;
  }
  if (request->op == READ) {
    count = count < request->capacity ? count : request->capacity;
    if (count)
      memcpy(request->out, saved, count * sizeof(*saved));
    *request->count = count;
    return H2_PAL_OK;
  }
  if (request->op == INSERT)
    rc = h2_wifi_saved_list_insert(saved, &count, request->config);
  else if (request->op == REMOVE)
    rc = h2_wifi_saved_list_remove(saved, &count, request->ssid,
                                   request->ssid_len);
  if (rc != H2_PAL_OK)
    return rc;
  rc = h2_wifi_saved_list_encode(saved, count, blob, sizeof(blob));
  if (rc != H2_PAL_OK)
    return rc;
  /* An explicit empty blob prevents importing old FLASH credentials again. */
  return h2_esp_platform_wifi_saved_record(blob, true);
}

static int run_request(saved_request_t *request) {
  int rc = h2_esp_platform_wifi_ensure_started();
  return rc == H2_PAL_OK ? h2_esp_platform_wifi_saved_transaction(
                               saved_transaction, request)
                         : rc;
}

static int
h2_esp_wifi_settings_list_saved_sta_configs(void *user,
                                            h2_pal_wifi_saved_network_t *out,
                                            size_t capacity, size_t *count) {
  (void)user;
  if (!count)
    return H2_PAL_ERR_INVALID_ARG;
  *count = 0;
  if (capacity && !out)
    return H2_PAL_ERR_INVALID_ARG;
  saved_request_t request = {
      .op = READ, .out = out, .capacity = capacity, .count = count};
  return run_request(&request);
}

static int
h2_esp_wifi_settings_get_saved_sta_config(void *user,
                                          h2_pal_wifi_sta_config_t *out) {
  if (!out)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out, 0, sizeof(*out));
  h2_pal_wifi_saved_network_t saved;
  size_t count;
  int rc = h2_esp_wifi_settings_list_saved_sta_configs(user, &saved, 1, &count);
  if (rc != H2_PAL_OK)
    return rc;
  if (!count)
    return H2_PAL_ERR_NOT_FOUND;
  *out = saved.config;
  return H2_PAL_OK;
}

/* Validate in a separate frame before entering the larger transaction. */
static int validate_config(const h2_pal_wifi_sta_config_t *config) {
  if (!config)
    return H2_PAL_ERR_INVALID_ARG;
  const h2_pal_wifi_saved_network_t saved = {.config = *config};
  uint8_t blob[H2_WIFI_SAVED_LIST_BLOB_SIZE];
  return h2_wifi_saved_list_encode(&saved, 1, blob, sizeof(blob));
}

static int h2_esp_wifi_settings_set_saved_sta_config(
    void *user, const h2_pal_wifi_sta_config_t *config) {
  (void)user;
  /* Validate before any storage access or migration. */
  int rc = validate_config(config);
  if (rc != H2_PAL_OK)
    return rc;
  saved_request_t request = {.op = INSERT, .config = config};
  return run_request(&request);
}

static int h2_esp_wifi_settings_remove_saved_sta_config(void *user,
                                                        const char *ssid,
                                                        size_t ssid_len) {
  (void)user;
  if (!ssid || !ssid_len || ssid_len > H2_PAL_WIFI_SSID_MAX)
    return H2_PAL_ERR_INVALID_ARG;
  saved_request_t request = {.op = REMOVE, .ssid = ssid, .ssid_len = ssid_len};
  return run_request(&request);
}

static int h2_esp_wifi_settings_clear_saved_sta_config(void *user) {
  (void)user;
  saved_request_t request = {.op = CLEAR};
  return run_request(&request);
}

static int h2_esp_wifi_settings_has_saved_sta_config(void *user, int *out) {
  if (!out)
    return H2_PAL_ERR_INVALID_ARG;
  *out = 0;
  h2_pal_wifi_sta_config_t config;
  int rc = h2_esp_wifi_settings_get_saved_sta_config(user, &config);
  if (rc == H2_PAL_ERR_NOT_FOUND)
    return H2_PAL_OK;
  if (rc == H2_PAL_OK)
    *out = 1;
  return rc;
}

static const h2_pal_wifi_settings_vtable_t s_h2_esp_wifi_settings_vtable = {
    .get_saved_sta_config = h2_esp_wifi_settings_get_saved_sta_config,
    .set_saved_sta_config = h2_esp_wifi_settings_set_saved_sta_config,
    .clear_saved_sta_config = h2_esp_wifi_settings_clear_saved_sta_config,
    .has_saved_sta_config = h2_esp_wifi_settings_has_saved_sta_config,
    .list_saved_sta_configs = h2_esp_wifi_settings_list_saved_sta_configs,
    .remove_saved_sta_config = h2_esp_wifi_settings_remove_saved_sta_config,
};
static h2_pal_wifi_settings_t s_h2_esp_wifi_settings = {
    .user = NULL,
    .vtable = &s_h2_esp_wifi_settings_vtable,
};
h2_pal_wifi_settings_t *h2_esp_platform_wifi_settings(void) {
  return &s_h2_esp_wifi_settings;
}
