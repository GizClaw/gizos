#include "h2_bk_platform_core.h"

#include "FreeRTOS.h"
#include "easyflash.h"
#include "h2_wifi_sta.h"
#include "semphr.h"
#include "task.h"

#include <stdint.h>
#include <string.h>

#define H2_BK_WIFI_SETTINGS_KEY "h2.wifi_sta"
#define H2_BK_WIFI_SETTINGS_LIST_KEY "h2.wifi_nets_v1"
#define H2_BK_WIFI_SETTINGS_MAGIC 0x48325746u
#define H2_BK_WIFI_SETTINGS_VERSION 1u

/*
 * Version 1 is a fixed little-endian wire format. Its offsets and two trailing
 * reserved bytes intentionally preserve records written by the original BK
 * implementation, which used the same 116-byte layout through a native struct.
 */
#define H2_BK_WIFI_SETTINGS_V1_SSID_MAX 32u
#define H2_BK_WIFI_SETTINGS_V1_PASSWORD_MAX 64u
#define H2_BK_WIFI_SETTINGS_V1_MAGIC_OFFSET 0u
#define H2_BK_WIFI_SETTINGS_V1_VERSION_OFFSET 4u
#define H2_BK_WIFI_SETTINGS_V1_SSID_LEN_OFFSET 6u
#define H2_BK_WIFI_SETTINGS_V1_PASSWORD_LEN_OFFSET 7u
#define H2_BK_WIFI_SETTINGS_V1_BSSID_OFFSET 8u
#define H2_BK_WIFI_SETTINGS_V1_BSSID_SET_OFFSET 14u
#define H2_BK_WIFI_SETTINGS_V1_CHANNEL_OFFSET 15u
#define H2_BK_WIFI_SETTINGS_V1_SSID_OFFSET 16u
#define H2_BK_WIFI_SETTINGS_V1_PASSWORD_OFFSET 49u
#define H2_BK_WIFI_SETTINGS_V1_RECORD_SIZE 116u

_Static_assert(
    H2_PAL_WIFI_SSID_MAX == H2_BK_WIFI_SETTINGS_V1_SSID_MAX,
    "bump the BK saved Wi-Fi format version when the SSID capacity changes");
_Static_assert(H2_PAL_WIFI_PASSWORD_MAX == H2_BK_WIFI_SETTINGS_V1_PASSWORD_MAX,
               "bump the BK saved Wi-Fi format version when the password "
               "capacity changes");

typedef uint8_t
    h2_bk_wifi_settings_record_t[H2_BK_WIFI_SETTINGS_V1_RECORD_SIZE];

static int h2_bk_wifi_settings_map_error(EfErrCode error) {
  switch (error) {
  case EF_NO_ERR:
    return H2_PAL_OK;
  case EF_ENV_NAME_ERR:
  case EF_ENV_NAME_EXIST:
    return H2_PAL_ERR_INVALID_ARG;
  case EF_ENV_INIT_FAILED:
    return H2_PAL_ERR_UNAVAILABLE;
  case EF_ENV_FULL:
    return H2_PAL_ERR_NO_SPACE;
  case EF_ERASE_ERR:
  case EF_READ_ERR:
  case EF_WRITE_ERR:
  default:
    return H2_PAL_ERR_IO;
  }
}

static int h2_bk_wifi_settings_init(void) {
  return h2_bk_wifi_settings_map_error(easyflash_init());
}

static uint16_t
h2_bk_wifi_settings_read_u16_le(const h2_bk_wifi_settings_record_t record,
                                size_t offset) {
  return (uint16_t)record[offset] | ((uint16_t)record[offset + 1u] << 8u);
}

static uint32_t
h2_bk_wifi_settings_read_u32_le(const h2_bk_wifi_settings_record_t record,
                                size_t offset) {
  return (uint32_t)record[offset] | ((uint32_t)record[offset + 1u] << 8u) |
         ((uint32_t)record[offset + 2u] << 16u) |
         ((uint32_t)record[offset + 3u] << 24u);
}

static int h2_bk_wifi_settings_record_to_config(
    h2_pal_wifi_sta_config_t *config,
    const h2_bk_wifi_settings_record_t record) {
  uint8_t ssid_len = record[H2_BK_WIFI_SETTINGS_V1_SSID_LEN_OFFSET];
  uint8_t password_len = record[H2_BK_WIFI_SETTINGS_V1_PASSWORD_LEN_OFFSET];
  uint8_t bssid_set = record[H2_BK_WIFI_SETTINGS_V1_BSSID_SET_OFFSET];
  if (h2_bk_wifi_settings_read_u32_le(record,
                                      H2_BK_WIFI_SETTINGS_V1_MAGIC_OFFSET) !=
          H2_BK_WIFI_SETTINGS_MAGIC ||
      h2_bk_wifi_settings_read_u16_le(record,
                                      H2_BK_WIFI_SETTINGS_V1_VERSION_OFFSET) !=
          H2_BK_WIFI_SETTINGS_VERSION ||
      ssid_len == 0u || ssid_len > H2_BK_WIFI_SETTINGS_V1_SSID_MAX ||
      password_len > H2_BK_WIFI_SETTINGS_V1_PASSWORD_MAX || bssid_set > 1u) {
    return H2_PAL_ERR_IO;
  }
  memset(config, 0, sizeof(*config));
  config->ssid_len = ssid_len;
  config->password_len = password_len;
  config->bssid_set = bssid_set;
  config->channel = record[H2_BK_WIFI_SETTINGS_V1_CHANNEL_OFFSET];
  memcpy(config->ssid, &record[H2_BK_WIFI_SETTINGS_V1_SSID_OFFSET],
         config->ssid_len);
  memcpy(config->password, &record[H2_BK_WIFI_SETTINGS_V1_PASSWORD_OFFSET],
         config->password_len);
  if (config->bssid_set != 0u) {
    memcpy(config->bssid, &record[H2_BK_WIFI_SETTINGS_V1_BSSID_OFFSET],
           sizeof(config->bssid));
  }
  return H2_PAL_OK;
}

/* Hold one provider mutex across migration and read-modify-write; easyflash's
 * per-operation locking alone cannot serialize a list insertion. */
static StaticSemaphore_t saved_mutex_storage;
static SemaphoreHandle_t saved_mutex;
static portMUX_TYPE saved_init_lock = portMUX_INITIALIZER_UNLOCKED;

static int saved_lock(void) {
  taskENTER_CRITICAL(&saved_init_lock);
  if (!saved_mutex)
    saved_mutex = xSemaphoreCreateMutexStatic(&saved_mutex_storage);
  taskEXIT_CRITICAL(&saved_init_lock);
  return saved_mutex && xSemaphoreTake(saved_mutex, portMAX_DELAY) == pdTRUE
             ? H2_PAL_OK
             : H2_PAL_ERR_NO_MEMORY;
}

static int write_list(const h2_pal_wifi_saved_network_t *saved, size_t count) {
  uint8_t blob[H2_WIFI_SAVED_LIST_BLOB_SIZE];
  int rc = h2_wifi_saved_list_encode(saved, count, blob, sizeof(blob));
  if (rc != H2_PAL_OK)
    return rc;
  return h2_bk_wifi_settings_map_error(
      ef_set_env_blob(H2_BK_WIFI_SETTINGS_LIST_KEY, blob, sizeof(blob)));
}

static int read_list(h2_pal_wifi_saved_network_t *saved, size_t *count) {
  uint8_t blob[H2_WIFI_SAVED_LIST_BLOB_SIZE];
  size_t length = 0;
  *count = 0;
  (void)ef_get_env_blob(H2_BK_WIFI_SETTINGS_LIST_KEY, NULL, 0, &length);
  if (length) {
    if (length != sizeof(blob) ||
        ef_get_env_blob(H2_BK_WIFI_SETTINGS_LIST_KEY, blob, sizeof(blob),
                        NULL) != sizeof(blob))
      return H2_PAL_ERR_IO;
    return h2_wifi_saved_list_decode(blob, sizeof(blob), saved, count);
  }
  (void)ef_get_env_blob(H2_BK_WIFI_SETTINGS_KEY, NULL, 0, &length);
  if (length) {
    h2_bk_wifi_settings_record_t old;
    if (length != sizeof(old) ||
        ef_get_env_blob(H2_BK_WIFI_SETTINGS_KEY, old, sizeof(old), NULL) !=
            sizeof(old))
      return H2_PAL_ERR_IO;
    h2_pal_wifi_sta_config_t config;
    int rc = h2_bk_wifi_settings_record_to_config(&config, old);
    if (rc != H2_PAL_OK)
      return rc;
    rc = h2_wifi_saved_list_insert(saved, count, &config);
    if (rc != H2_PAL_OK)
      return H2_PAL_ERR_IO;
  }
  /* Empty lists are durable too: clear must not resurrect the old key. */
  return write_list(saved, *count);
}

static int
h2_bk_wifi_settings_list_saved_sta_configs(void *user,
                                           h2_pal_wifi_saved_network_t *out,
                                           size_t capacity, size_t *out_count) {
  (void)user;
  if (!out_count)
    return H2_PAL_ERR_INVALID_ARG;
  *out_count = 0;
  if (capacity && !out)
    return H2_PAL_ERR_INVALID_ARG;
  int rc = saved_lock();
  if (rc != H2_PAL_OK)
    return rc;
  h2_pal_wifi_saved_network_t saved[H2_PAL_WIFI_SAVED_NETWORK_MAX] = {0};
  size_t count = 0;
  rc = h2_bk_wifi_settings_init();
  if (rc == H2_PAL_OK)
    rc = read_list(saved, &count);
  if (rc == H2_PAL_OK) {
    count = count < capacity ? count : capacity;
    if (count)
      memcpy(out, saved, count * sizeof(*saved));
    *out_count = count;
  }
  (void)xSemaphoreGive(saved_mutex);
  return rc;
}

static int
h2_bk_wifi_settings_get_saved_sta_config(void *user,
                                         h2_pal_wifi_sta_config_t *out) {
  if (!out)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out, 0, sizeof(*out));
  h2_pal_wifi_saved_network_t saved;
  size_t count;
  int rc = h2_bk_wifi_settings_list_saved_sta_configs(user, &saved, 1, &count);
  if (rc != H2_PAL_OK)
    return rc;
  if (!count)
    return H2_PAL_ERR_NOT_FOUND;
  *out = saved.config;
  return H2_PAL_OK;
}

static int mutate_list(const h2_pal_wifi_sta_config_t *config, const char *ssid,
                       size_t ssid_len) {
  int rc = saved_lock();
  if (rc != H2_PAL_OK)
    return rc;
  h2_pal_wifi_saved_network_t saved[H2_PAL_WIFI_SAVED_NETWORK_MAX] = {0};
  size_t count = 0;
  rc = h2_bk_wifi_settings_init();
  if (rc == H2_PAL_OK && (config || ssid))
    rc = read_list(saved, &count);
  if (rc == H2_PAL_OK && config)
    rc = h2_wifi_saved_list_insert(saved, &count, config);
  if (rc == H2_PAL_OK && ssid)
    rc = h2_wifi_saved_list_remove(saved, &count, ssid, ssid_len);
  if (rc == H2_PAL_OK)
    rc = write_list(saved, count);
  (void)xSemaphoreGive(saved_mutex);
  return rc;
}

/* Validate in a separate frame before entering the larger transaction. */
static int validate_config(const h2_pal_wifi_sta_config_t *config) {
  if (!config)
    return H2_PAL_ERR_INVALID_ARG;
  const h2_pal_wifi_saved_network_t saved = {.config = *config};
  uint8_t blob[H2_WIFI_SAVED_LIST_BLOB_SIZE];
  return h2_wifi_saved_list_encode(&saved, 1, blob, sizeof(blob));
}

static int h2_bk_wifi_settings_set_saved_sta_config(
    void *user, const h2_pal_wifi_sta_config_t *config) {
  (void)user;
  int rc = validate_config(config);
  return rc == H2_PAL_OK ? mutate_list(config, NULL, 0) : rc;
}
static int h2_bk_wifi_settings_remove_saved_sta_config(void *user,
                                                       const char *ssid,
                                                       size_t len) {
  (void)user;
  if (!ssid || !len || len > H2_PAL_WIFI_SSID_MAX)
    return H2_PAL_ERR_INVALID_ARG;
  return mutate_list(NULL, ssid, len);
}
static int h2_bk_wifi_settings_clear_saved_sta_config(void *user) {
  (void)user;
  return mutate_list(NULL, NULL, 0);
}
static int h2_bk_wifi_settings_has_saved_sta_config(void *user, int *out) {
  if (!out)
    return H2_PAL_ERR_INVALID_ARG;
  *out = 0;
  h2_pal_wifi_sta_config_t config;
  int rc = h2_bk_wifi_settings_get_saved_sta_config(user, &config);
  if (rc == H2_PAL_ERR_NOT_FOUND)
    return H2_PAL_OK;
  if (rc == H2_PAL_OK)
    *out = 1;
  return rc;
}
static const h2_pal_wifi_settings_vtable_t s_h2_bk_wifi_settings_vtable = {
    .get_saved_sta_config = h2_bk_wifi_settings_get_saved_sta_config,
    .set_saved_sta_config = h2_bk_wifi_settings_set_saved_sta_config,
    .clear_saved_sta_config = h2_bk_wifi_settings_clear_saved_sta_config,
    .has_saved_sta_config = h2_bk_wifi_settings_has_saved_sta_config,
    .list_saved_sta_configs = h2_bk_wifi_settings_list_saved_sta_configs,
    .remove_saved_sta_config = h2_bk_wifi_settings_remove_saved_sta_config,
};
static h2_pal_wifi_settings_t s_h2_bk_wifi_settings = {
    .user = NULL,
    .vtable = &s_h2_bk_wifi_settings_vtable,
};
h2_pal_wifi_settings_t *h2_bk_platform_wifi_settings(void) {
  return &s_h2_bk_wifi_settings;
}
