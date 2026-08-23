#include "h2_bk_platform_core.h"

#include "easyflash.h"

#include <stdint.h>
#include <string.h>

#define H2_BK_WIFI_SETTINGS_KEY "h2.wifi_sta"
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
_Static_assert(
    H2_PAL_WIFI_PASSWORD_MAX == H2_BK_WIFI_SETTINGS_V1_PASSWORD_MAX,
    "bump the BK saved Wi-Fi format version when the password capacity changes");

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

static void h2_bk_wifi_settings_write_u16_le(
    h2_bk_wifi_settings_record_t record,
    size_t offset,
    uint16_t value) {
    record[offset] = (uint8_t)(value & 0xffu);
    record[offset + 1u] = (uint8_t)((value >> 8u) & 0xffu);
}

static void h2_bk_wifi_settings_write_u32_le(
    h2_bk_wifi_settings_record_t record,
    size_t offset,
    uint32_t value) {
    record[offset] = (uint8_t)(value & 0xffu);
    record[offset + 1u] = (uint8_t)((value >> 8u) & 0xffu);
    record[offset + 2u] = (uint8_t)((value >> 16u) & 0xffu);
    record[offset + 3u] = (uint8_t)((value >> 24u) & 0xffu);
}

static uint16_t h2_bk_wifi_settings_read_u16_le(
    const h2_bk_wifi_settings_record_t record,
    size_t offset) {
    return (uint16_t)record[offset] |
           ((uint16_t)record[offset + 1u] << 8u);
}

static uint32_t h2_bk_wifi_settings_read_u32_le(
    const h2_bk_wifi_settings_record_t record,
    size_t offset) {
    return (uint32_t)record[offset] |
           ((uint32_t)record[offset + 1u] << 8u) |
           ((uint32_t)record[offset + 2u] << 16u) |
           ((uint32_t)record[offset + 3u] << 24u);
}

static void h2_bk_wifi_settings_record_from_config(
    h2_bk_wifi_settings_record_t record,
    const h2_pal_wifi_sta_config_t *config) {
    memset(record, 0, H2_BK_WIFI_SETTINGS_V1_RECORD_SIZE);
    h2_bk_wifi_settings_write_u32_le(
        record,
        H2_BK_WIFI_SETTINGS_V1_MAGIC_OFFSET,
        H2_BK_WIFI_SETTINGS_MAGIC);
    h2_bk_wifi_settings_write_u16_le(
        record,
        H2_BK_WIFI_SETTINGS_V1_VERSION_OFFSET,
        H2_BK_WIFI_SETTINGS_VERSION);
    record[H2_BK_WIFI_SETTINGS_V1_SSID_LEN_OFFSET] =
        (uint8_t)config->ssid_len;
    record[H2_BK_WIFI_SETTINGS_V1_PASSWORD_LEN_OFFSET] =
        (uint8_t)config->password_len;
    record[H2_BK_WIFI_SETTINGS_V1_BSSID_SET_OFFSET] = config->bssid_set;
    record[H2_BK_WIFI_SETTINGS_V1_CHANNEL_OFFSET] = config->channel;
    memcpy(
        &record[H2_BK_WIFI_SETTINGS_V1_SSID_OFFSET],
        config->ssid,
        config->ssid_len);
    memcpy(
        &record[H2_BK_WIFI_SETTINGS_V1_PASSWORD_OFFSET],
        config->password,
        config->password_len);
    if (config->bssid_set != 0u) {
        memcpy(
            &record[H2_BK_WIFI_SETTINGS_V1_BSSID_OFFSET],
            config->bssid,
            sizeof(config->bssid));
    }
}

static int h2_bk_wifi_settings_record_to_config(
    h2_pal_wifi_sta_config_t *config,
    const h2_bk_wifi_settings_record_t record) {
    uint8_t ssid_len = record[H2_BK_WIFI_SETTINGS_V1_SSID_LEN_OFFSET];
    uint8_t password_len =
        record[H2_BK_WIFI_SETTINGS_V1_PASSWORD_LEN_OFFSET];
    uint8_t bssid_set =
        record[H2_BK_WIFI_SETTINGS_V1_BSSID_SET_OFFSET];
    if (h2_bk_wifi_settings_read_u32_le(
            record,
            H2_BK_WIFI_SETTINGS_V1_MAGIC_OFFSET) !=
            H2_BK_WIFI_SETTINGS_MAGIC ||
        h2_bk_wifi_settings_read_u16_le(
            record,
            H2_BK_WIFI_SETTINGS_V1_VERSION_OFFSET) !=
            H2_BK_WIFI_SETTINGS_VERSION ||
        ssid_len == 0u ||
        ssid_len > H2_BK_WIFI_SETTINGS_V1_SSID_MAX ||
        password_len > H2_BK_WIFI_SETTINGS_V1_PASSWORD_MAX ||
        bssid_set > 1u) {
        return H2_PAL_ERR_IO;
    }
    memset(config, 0, sizeof(*config));
    config->ssid_len = ssid_len;
    config->password_len = password_len;
    config->bssid_set = bssid_set;
    config->channel = record[H2_BK_WIFI_SETTINGS_V1_CHANNEL_OFFSET];
    memcpy(
        config->ssid,
        &record[H2_BK_WIFI_SETTINGS_V1_SSID_OFFSET],
        config->ssid_len);
    memcpy(
        config->password,
        &record[H2_BK_WIFI_SETTINGS_V1_PASSWORD_OFFSET],
        config->password_len);
    if (config->bssid_set != 0u) {
        memcpy(
            config->bssid,
            &record[H2_BK_WIFI_SETTINGS_V1_BSSID_OFFSET],
            sizeof(config->bssid));
    }
    return H2_PAL_OK;
}

static int h2_bk_wifi_settings_get_saved_sta_config(
    void *user,
    h2_pal_wifi_sta_config_t *out_config) {
    (void)user;
    if (out_config == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }

    int rc = h2_bk_wifi_settings_init();
    if (rc != H2_PAL_OK) {
        memset(out_config, 0, sizeof(*out_config));
        return rc;
    }
    size_t saved_len = 0u;
    (void)ef_get_env_blob(
        H2_BK_WIFI_SETTINGS_KEY,
        NULL,
        0u,
        &saved_len);
    if (saved_len == 0u) {
        memset(out_config, 0, sizeof(*out_config));
        return H2_PAL_ERR_NOT_FOUND;
    }
    if (saved_len != H2_BK_WIFI_SETTINGS_V1_RECORD_SIZE) {
        memset(out_config, 0, sizeof(*out_config));
        return H2_PAL_ERR_IO;
    }
    h2_bk_wifi_settings_record_t record;
    memset(record, 0, sizeof(record));
    if (ef_get_env_blob(
            H2_BK_WIFI_SETTINGS_KEY,
            record,
            sizeof(record),
            NULL) != sizeof(record)) {
        memset(out_config, 0, sizeof(*out_config));
        return H2_PAL_ERR_IO;
    }
    rc = h2_bk_wifi_settings_record_to_config(out_config, record);
    if (rc != H2_PAL_OK) {
        memset(out_config, 0, sizeof(*out_config));
    }
    return rc;
}

static int h2_bk_wifi_settings_set_saved_sta_config(
    void *user,
    const h2_pal_wifi_sta_config_t *config) {
    (void)user;
    int rc = h2_pal_wifi_settings_validate_sta_config(config);
    if (rc != H2_PAL_OK) {
        return rc;
    }

    rc = h2_bk_wifi_settings_init();
    if (rc != H2_PAL_OK) {
        return rc;
    }
    h2_bk_wifi_settings_record_t record;
    h2_bk_wifi_settings_record_from_config(record, config);
    return h2_bk_wifi_settings_map_error(ef_set_env_blob(
        H2_BK_WIFI_SETTINGS_KEY,
        record,
        sizeof(record)));
}

static int h2_bk_wifi_settings_clear_saved_sta_config(void *user) {
    (void)user;
    int rc = h2_bk_wifi_settings_init();
    if (rc != H2_PAL_OK) {
        return rc;
    }
    size_t saved_len = 0u;
    (void)ef_get_env_blob(
        H2_BK_WIFI_SETTINGS_KEY,
        NULL,
        0u,
        &saved_len);
    if (saved_len == 0u) {
        return H2_PAL_OK;
    }
    return h2_bk_wifi_settings_map_error(
        ef_del_env(H2_BK_WIFI_SETTINGS_KEY));
}

static int h2_bk_wifi_settings_has_saved_sta_config(
    void *user,
    int *out_has_config) {
    (void)user;
    if (out_has_config == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_has_config = 0;

    h2_pal_wifi_sta_config_t config;
    int rc = h2_bk_wifi_settings_get_saved_sta_config(user, &config);
    if (rc != H2_PAL_OK) {
        if (rc == H2_PAL_ERR_NOT_FOUND) {
            return H2_PAL_OK;
        }
        return rc;
    }

    *out_has_config = 1;
    return H2_PAL_OK;
}

static const h2_pal_wifi_settings_vtable_t s_h2_bk_wifi_settings_vtable = {
    .get_saved_sta_config = h2_bk_wifi_settings_get_saved_sta_config,
    .set_saved_sta_config = h2_bk_wifi_settings_set_saved_sta_config,
    .clear_saved_sta_config = h2_bk_wifi_settings_clear_saved_sta_config,
    .has_saved_sta_config = h2_bk_wifi_settings_has_saved_sta_config,
};

static h2_pal_wifi_settings_t s_h2_bk_wifi_settings = {
    .user = NULL,
    .vtable = &s_h2_bk_wifi_settings_vtable,
};

h2_pal_wifi_settings_t *h2_bk_platform_wifi_settings(void) {
    return &s_h2_bk_wifi_settings;
}
