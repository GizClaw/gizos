#include "h2_desktop_platform.h"

#include <string.h>

#define H2_DESKTOP_BLE_CONN_HANDLE 1u
#define H2_DESKTOP_BLE_VALUE_HANDLE 2u
#define H2_DESKTOP_BLE_CCCD_HANDLE 3u
#define H2_DESKTOP_BLE_VALUE_MAX_LEN H2_PAL_BLE_ATT_MAX_VALUE_LEN
#define H2_DESKTOP_BLE_AD_TYPE_FLAGS 0x01u
#define H2_DESKTOP_BLE_AD_TYPE_UUID16_INCOMPLETE 0x02u
#define H2_DESKTOP_BLE_AD_TYPE_UUID16_COMPLETE 0x03u
#define H2_DESKTOP_BLE_AD_TYPE_UUID32_INCOMPLETE 0x04u
#define H2_DESKTOP_BLE_AD_TYPE_UUID32_COMPLETE 0x05u
#define H2_DESKTOP_BLE_AD_TYPE_UUID128_INCOMPLETE 0x06u
#define H2_DESKTOP_BLE_AD_TYPE_UUID128_COMPLETE 0x07u
#define H2_DESKTOP_BLE_AD_TYPE_NAME_INCOMPLETE 0x08u
#define H2_DESKTOP_BLE_AD_TYPE_NAME_COMPLETE 0x09u
#define H2_DESKTOP_BLE_AD_TYPE_SERVICE_DATA16 0x16u
#define H2_DESKTOP_BLE_AD_TYPE_SERVICE_DATA32 0x20u
#define H2_DESKTOP_BLE_AD_TYPE_SERVICE_DATA128 0x21u
#define H2_DESKTOP_BLE_AD_TYPE_MANUFACTURER 0xffu
#define H2_DESKTOP_BLE_MAX_SCAN_UUIDS (H2_PAL_BLE_EXT_ADV_DATA_MAX_LEN / 3u)
#define H2_DESKTOP_BLE_ADV_SET_COUNT 4u

struct h2_pal_ble_adv_set {
    h2_pal_ble_adv_params_t params;
    uint8_t data[H2_PAL_BLE_EXT_ADV_DATA_MAX_LEN];
    size_t data_len;
    int allocated;
    int data_staged;
    int active;
};

static uint8_t s_h2_desktop_ble_service_uuid[16] = { 0x48u, 0x32u };
static size_t s_h2_desktop_ble_service_uuid_len = 2u;
static uint8_t s_h2_desktop_ble_value[H2_DESKTOP_BLE_VALUE_MAX_LEN] = { 'o', 'k' };
static size_t s_h2_desktop_ble_value_len = 2u;
static size_t s_h2_desktop_ble_value_max_len =
    H2_DESKTOP_BLE_VALUE_MAX_LEN;
static h2_pal_ble_gatt_read_fn s_h2_desktop_ble_value_read;
static h2_pal_ble_gatt_write_fn s_h2_desktop_ble_value_write;
static void *s_h2_desktop_ble_value_user;
static uint8_t s_h2_desktop_ble_adv_data[H2_PAL_BLE_EXT_ADV_DATA_MAX_LEN];
static size_t s_h2_desktop_ble_adv_data_len;
static int s_h2_desktop_ble_adv_data_staged;
static int s_h2_desktop_ble_adv_active;
static int s_h2_desktop_ble_host_started;
static h2_pal_ble_pairing_config_t s_h2_desktop_ble_pairing;
static int s_h2_desktop_ble_paired;
static uint32_t s_h2_desktop_ble_value_permissions;
static int s_h2_desktop_ble_extended_supported = 1;
static int s_h2_desktop_ble_extended_scan_supported = 1;
static h2_pal_result_t s_h2_desktop_ble_adv_start_result = H2_PAL_OK;
static h2_pal_ble_uuid_t s_h2_desktop_ble_scan_uuids[H2_DESKTOP_BLE_MAX_SCAN_UUIDS];
static h2_pal_ble_adv_set_t s_h2_desktop_ble_adv_sets[H2_DESKTOP_BLE_ADV_SET_COUNT];
static const h2_pal_system_event_api_t *s_h2_desktop_ble_system_event;

static int h2_desktop_ble_system_event_valid(
    const h2_pal_system_event_api_t *system_event) {
    return system_event != NULL && system_event->vtable != NULL &&
           system_event->vtable->post != NULL;
}

static void h2_desktop_ble_post(
    h2_pal_system_event_type_t type,
    const void *payload,
    size_t payload_size) {
    h2_pal_system_event_t event;
    memset(&event, 0, sizeof(event));
    event.type = type;
    event.payload = payload;
    event.payload_size = payload_size;
    (void)h2_pal_system_event_post(
        s_h2_desktop_ble_system_event, &event, 0u);
}

static void h2_desktop_ble_post_adv(
    h2_pal_system_event_type_t type,
    h2_pal_ble_adv_set_t *set,
    h2_pal_result_t status) {
    h2_pal_ble_adv_set_event_t payload = {
        .set = set,
        .status = status,
    };
    h2_desktop_ble_post(type, &payload, sizeof(payload));
}

static int h2_desktop_ble_adv_set_valid(const h2_pal_ble_adv_set_t *set) {
    uintptr_t address = (uintptr_t)set;
    return address >= (uintptr_t)&s_h2_desktop_ble_adv_sets[0] &&
           address < (uintptr_t)&s_h2_desktop_ble_adv_sets[H2_DESKTOP_BLE_ADV_SET_COUNT] &&
           (address - (uintptr_t)&s_h2_desktop_ble_adv_sets[0]) %
                   sizeof(s_h2_desktop_ble_adv_sets[0]) ==
               0u &&
           set->allocated;
}

static int h2_desktop_ble_any_adv_set_active(void) {
    for (size_t i = 0u; i < H2_DESKTOP_BLE_ADV_SET_COUNT; ++i) {
        if (s_h2_desktop_ble_adv_sets[i].allocated &&
            s_h2_desktop_ble_adv_sets[i].active) {
            return 1;
        }
    }
    return 0;
}

static int h2_desktop_ble_adv_put(
    uint8_t *out,
    size_t *out_len,
    uint8_t type,
    const uint8_t *data,
    size_t data_len) {
    if (data_len > 254u || (data_len > 0u && data == NULL) ||
        *out_len > H2_PAL_BLE_EXT_ADV_DATA_MAX_LEN - data_len - 2u) {
        return 0;
    }
    out[(*out_len)++] = (uint8_t)(data_len + 1u);
    out[(*out_len)++] = type;
    if (data_len > 0u) {
        memcpy(&out[*out_len], data, data_len);
        *out_len += data_len;
    }
    return 1;
}

static int h2_desktop_ble_adv_put_uuid_list(
    uint8_t *out,
    size_t *out_len,
    const h2_pal_ble_adv_data_t *data,
    size_t uuid_len,
    uint8_t type) {
    size_t list_len = 0u;
    for (size_t i = 0u; i < data->service_uuid_count; ++i) {
        if (data->service_uuids[i].len == uuid_len) {
            if (list_len > 254u - uuid_len) {
                return 0;
            }
            list_len += uuid_len;
        }
    }
    if (list_len == 0u) {
        return 1;
    }
    if (*out_len > H2_PAL_BLE_EXT_ADV_DATA_MAX_LEN - list_len - 2u) {
        return 0;
    }
    out[(*out_len)++] = (uint8_t)(list_len + 1u);
    out[(*out_len)++] = type;
    for (size_t i = 0u; i < data->service_uuid_count; ++i) {
        const h2_pal_ble_uuid_t *uuid = &data->service_uuids[i];
        if (uuid->len == uuid_len) {
            memcpy(&out[*out_len], uuid->data, uuid_len);
            *out_len += uuid_len;
        }
    }
    return 1;
}

static h2_pal_result_t h2_desktop_ble_encode_adv_data(
    const h2_pal_ble_adv_data_t *data,
    uint8_t *out,
    size_t *out_len) {
    uint8_t flags = 0x06u;
    size_t len = 0u;
    if (data == NULL || out == NULL || out_len == NULL ||
        (data->service_uuid_count > 0u && data->service_uuids == NULL) ||
        (data->manufacturer_data.len > 0u && data->manufacturer_data.data == NULL) ||
        (data->service_data_uuid.len > 0u && data->service_data_uuid.data == NULL) ||
        (data->service_data.len > 0u && data->service_data.data == NULL) ||
        !h2_desktop_ble_adv_put(out, &len, H2_DESKTOP_BLE_AD_TYPE_FLAGS, &flags, sizeof(flags))) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    for (size_t i = 0u; i < data->service_uuid_count; ++i) {
        const h2_pal_ble_uuid_t *uuid = &data->service_uuids[i];
        if ((uuid->len != 2u && uuid->len != 4u && uuid->len != 16u) || uuid->data == NULL) {
            return H2_PAL_ERR_INVALID_ARG;
        }
    }
    if (!h2_desktop_ble_adv_put_uuid_list(
            out, &len, data, 2u, H2_DESKTOP_BLE_AD_TYPE_UUID16_COMPLETE) ||
        !h2_desktop_ble_adv_put_uuid_list(
            out, &len, data, 4u, H2_DESKTOP_BLE_AD_TYPE_UUID32_COMPLETE) ||
        !h2_desktop_ble_adv_put_uuid_list(
            out, &len, data, 16u, H2_DESKTOP_BLE_AD_TYPE_UUID128_COMPLETE)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (data->manufacturer_data.len > 0u &&
        !h2_desktop_ble_adv_put(out, &len, H2_DESKTOP_BLE_AD_TYPE_MANUFACTURER,
            data->manufacturer_data.data, data->manufacturer_data.len)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (data->service_data.len > 0u) {
        uint8_t service_data[254u];
        size_t uuid_len = data->service_data_uuid.len;
        uint8_t type = H2_DESKTOP_BLE_AD_TYPE_SERVICE_DATA16;
        if (uuid_len != 0u && uuid_len != 2u && uuid_len != 4u && uuid_len != 16u) {
            return H2_PAL_ERR_INVALID_ARG;
        }
        if (uuid_len + data->service_data.len > sizeof(service_data)) {
            return H2_PAL_ERR_INVALID_ARG;
        }
        if (uuid_len == 4u) type = H2_DESKTOP_BLE_AD_TYPE_SERVICE_DATA32;
        if (uuid_len == 16u) type = H2_DESKTOP_BLE_AD_TYPE_SERVICE_DATA128;
        if (uuid_len > 0u) memcpy(service_data, data->service_data_uuid.data, uuid_len);
        memcpy(service_data + uuid_len, data->service_data.data, data->service_data.len);
        if (!h2_desktop_ble_adv_put(
                out, &len, type, service_data, uuid_len + data->service_data.len)) {
            return H2_PAL_ERR_INVALID_ARG;
        }
    }
    if (data->local_name != NULL) {
        size_t name_len = strlen(data->local_name);
        if (!h2_desktop_ble_adv_put(out, &len, H2_DESKTOP_BLE_AD_TYPE_NAME_COMPLETE,
                (const uint8_t *)data->local_name, name_len)) {
            return H2_PAL_ERR_INVALID_ARG;
        }
    }
    *out_len = len;
    return H2_PAL_OK;
}

static void h2_desktop_ble_parse_adv_data(h2_pal_ble_scan_result_t *result) {
    size_t offset = 0u;
    size_t uuid_count = 0u;
    while (offset < s_h2_desktop_ble_adv_data_len) {
        size_t field_len = s_h2_desktop_ble_adv_data[offset++];
        if (field_len == 0u || field_len > s_h2_desktop_ble_adv_data_len - offset) {
            break;
        }
        uint8_t type = s_h2_desktop_ble_adv_data[offset];
        const uint8_t *field_data = &s_h2_desktop_ble_adv_data[offset + 1u];
        size_t data_len = field_len - 1u;
        size_t uuid_len = 0u;
        switch (type) {
        case H2_DESKTOP_BLE_AD_TYPE_UUID16_INCOMPLETE:
        case H2_DESKTOP_BLE_AD_TYPE_UUID16_COMPLETE:
            uuid_len = 2u;
            break;
        case H2_DESKTOP_BLE_AD_TYPE_UUID32_INCOMPLETE:
        case H2_DESKTOP_BLE_AD_TYPE_UUID32_COMPLETE:
            uuid_len = 4u;
            break;
        case H2_DESKTOP_BLE_AD_TYPE_UUID128_INCOMPLETE:
        case H2_DESKTOP_BLE_AD_TYPE_UUID128_COMPLETE:
            uuid_len = 16u;
            break;
        case H2_DESKTOP_BLE_AD_TYPE_NAME_INCOMPLETE:
        case H2_DESKTOP_BLE_AD_TYPE_NAME_COMPLETE:
            result->local_name = (const char *)field_data;
            result->local_name_len = data_len;
            break;
        case H2_DESKTOP_BLE_AD_TYPE_SERVICE_DATA16:
        case H2_DESKTOP_BLE_AD_TYPE_SERVICE_DATA32:
        case H2_DESKTOP_BLE_AD_TYPE_SERVICE_DATA128:
            result->service_data.data = field_data;
            result->service_data.len = data_len;
            break;
        case H2_DESKTOP_BLE_AD_TYPE_MANUFACTURER:
            result->manufacturer_data.data = field_data;
            result->manufacturer_data.len = data_len;
            break;
        default:
            break;
        }
        if (uuid_len > 0u && data_len % uuid_len == 0u) {
            for (size_t i = 0u; i < data_len && uuid_count < H2_DESKTOP_BLE_MAX_SCAN_UUIDS;
                 i += uuid_len) {
                s_h2_desktop_ble_scan_uuids[uuid_count].data = &field_data[i];
                s_h2_desktop_ble_scan_uuids[uuid_count].len = uuid_len;
                ++uuid_count;
            }
        }
        offset += field_len;
    }
    result->service_uuids = uuid_count > 0u ? s_h2_desktop_ble_scan_uuids : NULL;
    result->service_uuid_count = uuid_count;
}

static h2_pal_result_t h2_desktop_ble_start(h2_pal_ble_t *ble) {
    (void)ble;
    s_h2_desktop_ble_host_started = 1;
    h2_desktop_ble_post(H2_PAL_SYSTEM_EVENT_TYPE_BLE_HOST_STARTED, NULL, 0u);
    return H2_PAL_OK;
}

static h2_pal_result_t h2_desktop_ble_stop(h2_pal_ble_t *ble) {
    (void)ble;
    if (s_h2_desktop_ble_adv_active) {
        s_h2_desktop_ble_adv_active = 0;
        h2_desktop_ble_post(
            H2_PAL_SYSTEM_EVENT_TYPE_BLE_ADVERTISING_STOPPED, NULL, 0u);
    }
    for (size_t i = 0u; i < H2_DESKTOP_BLE_ADV_SET_COUNT; ++i) {
        h2_pal_ble_adv_set_t *set = &s_h2_desktop_ble_adv_sets[i];
        if (set->allocated && set->active) {
            set->active = 0;
            h2_desktop_ble_post_adv(
                H2_PAL_SYSTEM_EVENT_TYPE_BLE_ADVERTISING_STOPPED, set, H2_PAL_OK);
        }
    }
    s_h2_desktop_ble_host_started = 0;
    memset(&s_h2_desktop_ble_pairing, 0,
           sizeof(s_h2_desktop_ble_pairing));
    s_h2_desktop_ble_paired = 0;
    s_h2_desktop_ble_adv_data_staged = 0;
    s_h2_desktop_ble_adv_data_len = 0u;
    memset(s_h2_desktop_ble_adv_sets, 0, sizeof(s_h2_desktop_ble_adv_sets));
    h2_desktop_ble_post(H2_PAL_SYSTEM_EVENT_TYPE_BLE_HOST_STOPPED, NULL, 0u);
    return H2_PAL_OK;
}

static h2_pal_result_t h2_desktop_ble_set_adv_data(
    h2_pal_ble_t *ble,
    const h2_pal_ble_adv_data_t *data) {
    (void)ble;
    if (!s_h2_desktop_ble_host_started) {
        return data == NULL ? H2_PAL_ERR_INVALID_ARG : H2_PAL_ERR_INVALID_STATE;
    }
    uint8_t encoded[H2_PAL_BLE_EXT_ADV_DATA_MAX_LEN];
    size_t encoded_len = 0u;
    h2_pal_result_t rc = h2_desktop_ble_encode_adv_data(data, encoded, &encoded_len);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    memcpy(s_h2_desktop_ble_adv_data, encoded, encoded_len);
    s_h2_desktop_ble_adv_data_len = encoded_len;
    s_h2_desktop_ble_adv_data_staged = 1;
    return H2_PAL_OK;
}

static h2_pal_result_t h2_desktop_ble_start_advertising(
    h2_pal_ble_t *ble,
    const h2_pal_ble_adv_params_t *params) {
    (void)ble;
    if (params == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (!s_h2_desktop_ble_host_started || !s_h2_desktop_ble_adv_data_staged || s_h2_desktop_ble_adv_active) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    if (params->type == H2_PAL_BLE_ADV_TYPE_EXTENDED && !s_h2_desktop_ble_extended_supported) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    if (s_h2_desktop_ble_adv_start_result != H2_PAL_OK) {
        return s_h2_desktop_ble_adv_start_result;
    }
    if (params->type == H2_PAL_BLE_ADV_TYPE_LEGACY &&
        s_h2_desktop_ble_adv_data_len > H2_PAL_BLE_LEGACY_ADV_DATA_MAX_LEN) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    s_h2_desktop_ble_adv_active = 1;
    h2_desktop_ble_post(
        H2_PAL_SYSTEM_EVENT_TYPE_BLE_ADVERTISING_STARTED, NULL, 0u);
    if (params->duration_ms > 0u || params->max_adv_events > 0u) {
        s_h2_desktop_ble_adv_active = 0;
        h2_desktop_ble_post(
            H2_PAL_SYSTEM_EVENT_TYPE_BLE_ADVERTISING_STOPPED, NULL, 0u);
    }
    return H2_PAL_OK;
}

static h2_pal_result_t h2_desktop_ble_stop_advertising(h2_pal_ble_t *ble) {
    (void)ble;
    if (s_h2_desktop_ble_adv_active) {
        s_h2_desktop_ble_adv_active = 0;
        h2_desktop_ble_post(
            H2_PAL_SYSTEM_EVENT_TYPE_BLE_ADVERTISING_STOPPED, NULL, 0u);
    }
    return H2_PAL_OK;
}

static h2_pal_result_t h2_desktop_ble_adv_set_create(
    h2_pal_ble_t *ble,
    const h2_pal_ble_adv_params_t *params,
    h2_pal_ble_adv_set_t **out_set) {
    (void)ble;
    if (params == NULL || out_set == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_set = NULL;
    if (!s_h2_desktop_ble_host_started) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    if (params->type == H2_PAL_BLE_ADV_TYPE_EXTENDED &&
        !s_h2_desktop_ble_extended_supported) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    for (size_t i = 0u; i < H2_DESKTOP_BLE_ADV_SET_COUNT; ++i) {
        h2_pal_ble_adv_set_t *set = &s_h2_desktop_ble_adv_sets[i];
        if (!set->allocated) {
            memset(set, 0, sizeof(*set));
            set->params = *params;
            set->allocated = 1;
            *out_set = set;
            return H2_PAL_OK;
        }
    }
    return H2_PAL_ERR_NO_SPACE;
}

static h2_pal_result_t h2_desktop_ble_adv_set_set_data(
    h2_pal_ble_t *ble,
    h2_pal_ble_adv_set_t *set,
    const h2_pal_ble_adv_data_t *data) {
    (void)ble;
    if (data == NULL || !h2_desktop_ble_adv_set_valid(set)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    uint8_t encoded[H2_PAL_BLE_EXT_ADV_DATA_MAX_LEN];
    size_t encoded_len = 0u;
    h2_pal_result_t rc = h2_desktop_ble_encode_adv_data(data, encoded, &encoded_len);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    if (set->params.type == H2_PAL_BLE_ADV_TYPE_LEGACY &&
        encoded_len > H2_PAL_BLE_LEGACY_ADV_DATA_MAX_LEN) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    memcpy(set->data, encoded, encoded_len);
    set->data_len = encoded_len;
    set->data_staged = 1;
    return H2_PAL_OK;
}

static h2_pal_result_t h2_desktop_ble_adv_set_set_encoded_data(
    h2_pal_ble_t *ble,
    h2_pal_ble_adv_set_t *set,
    const uint8_t *encoded_data,
    size_t encoded_data_len) {
    (void)ble;
    if (!h2_desktop_ble_adv_set_valid(set) ||
        (encoded_data == NULL && encoded_data_len != 0u)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    size_t capacity = set->params.type == H2_PAL_BLE_ADV_TYPE_LEGACY
                          ? H2_PAL_BLE_LEGACY_ADV_DATA_MAX_LEN
                          : H2_PAL_BLE_EXT_ADV_DATA_MAX_LEN;
    if (encoded_data_len > capacity) {
        return H2_PAL_ERR_NO_SPACE;
    }
    if (encoded_data_len > 0u) {
        memcpy(set->data, encoded_data, encoded_data_len);
    }
    set->data_len = encoded_data_len;
    set->data_staged = 1;
    return H2_PAL_OK;
}

static h2_pal_result_t h2_desktop_ble_adv_set_start(
    h2_pal_ble_t *ble,
    h2_pal_ble_adv_set_t *set) {
    (void)ble;
    if (!h2_desktop_ble_adv_set_valid(set)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (!s_h2_desktop_ble_host_started || !set->data_staged || set->active) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    if (s_h2_desktop_ble_adv_start_result != H2_PAL_OK) {
        return s_h2_desktop_ble_adv_start_result;
    }
    set->active = 1;
    h2_desktop_ble_post_adv(
        H2_PAL_SYSTEM_EVENT_TYPE_BLE_ADVERTISING_STARTED, set, H2_PAL_OK);
    if (set->params.duration_ms > 0u || set->params.max_adv_events > 0u) {
        set->active = 0;
        h2_desktop_ble_post_adv(
            H2_PAL_SYSTEM_EVENT_TYPE_BLE_ADVERTISING_STOPPED, set, H2_PAL_OK);
    }
    return H2_PAL_OK;
}

static h2_pal_result_t h2_desktop_ble_adv_set_stop(
    h2_pal_ble_t *ble,
    h2_pal_ble_adv_set_t *set) {
    (void)ble;
    if (!h2_desktop_ble_adv_set_valid(set)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (set->active) {
        set->active = 0;
        h2_desktop_ble_post_adv(
            H2_PAL_SYSTEM_EVENT_TYPE_BLE_ADVERTISING_STOPPED, set, H2_PAL_OK);
    }
    return H2_PAL_OK;
}

static h2_pal_result_t h2_desktop_ble_adv_set_destroy(
    h2_pal_ble_t *ble,
    h2_pal_ble_adv_set_t *set) {
    if (!h2_desktop_ble_adv_set_valid(set)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    h2_pal_result_t rc = h2_desktop_ble_adv_set_stop(ble, set);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    memset(set, 0, sizeof(*set));
    return H2_PAL_OK;
}

int h2_desktop_platform_configure_ble_extended_advertising(int supported) {
    if (s_h2_desktop_ble_host_started || s_h2_desktop_ble_adv_active ||
        h2_desktop_ble_any_adv_set_active()) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    s_h2_desktop_ble_extended_supported = supported != 0;
    return H2_PAL_OK;
}

int h2_desktop_platform_configure_ble_extended_scanning(int supported) {
    if (s_h2_desktop_ble_host_started) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    s_h2_desktop_ble_extended_scan_supported = supported != 0;
    return H2_PAL_OK;
}

int h2_desktop_platform_configure_ble_advertising_start_result(h2_pal_result_t result) {
    if (result > H2_PAL_OK) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    s_h2_desktop_ble_adv_start_result = result;
    return H2_PAL_OK;
}

int h2_desktop_platform_copy_ble_staged_adv_data(
    uint8_t *out,
    size_t out_size,
    size_t *out_len) {
    if (out_len == NULL || (s_h2_desktop_ble_adv_data_len > 0u && out == NULL)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_len = s_h2_desktop_ble_adv_data_len;
    if (out_size < s_h2_desktop_ble_adv_data_len) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    if (s_h2_desktop_ble_adv_data_len > 0u) {
        memcpy(out, s_h2_desktop_ble_adv_data, s_h2_desktop_ble_adv_data_len);
    }
    return H2_PAL_OK;
}

int h2_desktop_platform_copy_ble_adv_set_data(
    h2_pal_ble_adv_set_t *set,
    uint8_t *out,
    size_t out_size,
    size_t *out_len) {
    if (!h2_desktop_ble_adv_set_valid(set) || out_len == NULL ||
        (set->data_len > 0u && out == NULL)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_len = set->data_len;
    if (out_size < set->data_len) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    if (set->data_len > 0u) {
        memcpy(out, set->data, set->data_len);
    }
    return H2_PAL_OK;
}

static h2_pal_result_t h2_desktop_ble_start_scan(
    h2_pal_ble_t *ble,
    const h2_pal_ble_scan_params_t *params,
    h2_pal_ble_scan_result_fn on_result,
    void *user) {
    (void)ble;
    if (params == NULL || on_result == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (params->interval_units_625us != 0u) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    if (!s_h2_desktop_ble_host_started) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    if (params->type == H2_PAL_BLE_SCAN_TYPE_EXTENDED &&
        !s_h2_desktop_ble_extended_scan_supported) {
        return H2_PAL_ERR_UNSUPPORTED;
    }

    h2_pal_ble_scan_result_t result;
    memset(&result, 0, sizeof(result));
    result.addr.type = H2_PAL_BLE_ADDR_TYPE_RANDOM;
    result.addr.value[0] = 0x02u;
    result.addr.value[1] = 0x48u;
    result.addr.value[2] = 0x32u;
    result.rssi = -40;
    result.connectable = true;
    result.adv_type = params->type == H2_PAL_BLE_SCAN_TYPE_EXTENDED
                          ? H2_PAL_BLE_ADV_TYPE_EXTENDED
                          : H2_PAL_BLE_ADV_TYPE_LEGACY;
    result.primary_phy = params->type == H2_PAL_BLE_SCAN_TYPE_EXTENDED &&
                                 (params->phy_mask & H2_PAL_BLE_SCAN_PHY_1M) == 0u &&
                                 (params->phy_mask & H2_PAL_BLE_SCAN_PHY_CODED) != 0u
                             ? H2_PAL_BLE_PHY_CODED
                             : H2_PAL_BLE_PHY_1M;
    result.secondary_phy = params->type == H2_PAL_BLE_SCAN_TYPE_EXTENDED
                              ? H2_PAL_BLE_PHY_2M
                              : H2_PAL_BLE_PHY_UNKNOWN;
    result.sid = params->type == H2_PAL_BLE_SCAN_TYPE_EXTENDED ? 7u : 0u;
    result.data_status = H2_PAL_BLE_ADV_DATA_COMPLETE;
    result.tx_power = params->type == H2_PAL_BLE_SCAN_TYPE_EXTENDED ? 4 : 127;
    result.raw_data.data = s_h2_desktop_ble_adv_data;
    result.raw_data.len = s_h2_desktop_ble_adv_data_len;
    h2_desktop_ble_parse_adv_data(&result);
    (void)on_result(user, &result);
    return H2_PAL_OK;
}

static h2_pal_result_t h2_desktop_ble_stop_scan(h2_pal_ble_t *ble) {
    (void)ble;
    return H2_PAL_OK;
}

static h2_pal_result_t h2_desktop_ble_register_gatt_services(
    h2_pal_ble_t *ble,
    const h2_pal_ble_gatt_service_t *services,
    size_t count) {
    (void)ble;
    if (count > 0u && services == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    for (size_t i = 0u; i < count; ++i) {
        if (services[i].uuid.data == NULL ||
            (services[i].uuid.len != 2u && services[i].uuid.len != 4u &&
             services[i].uuid.len != 16u)) {
            return H2_PAL_ERR_INVALID_ARG;
        }
        memcpy(s_h2_desktop_ble_service_uuid, services[i].uuid.data,
               services[i].uuid.len);
        s_h2_desktop_ble_service_uuid_len = services[i].uuid.len;
        if (services[i].out_service_handle != NULL) {
            *services[i].out_service_handle = (uint16_t)(10u + i);
        }
        for (size_t j = 0u; j < services[i].characteristic_count; ++j) {
            const h2_pal_ble_gatt_characteristic_t *ch = &services[i].characteristics[j];
            if (ch->initial_value_len > sizeof(s_h2_desktop_ble_value) ||
                (ch->initial_value_len > 0u &&
                 ch->initial_value == NULL)) {
                return H2_PAL_ERR_INVALID_ARG;
            }
            s_h2_desktop_ble_value_permissions = ch->permissions;
            s_h2_desktop_ble_value_max_len =
                ch->max_value_len == 0u ||
                        ch->max_value_len > sizeof(s_h2_desktop_ble_value)
                    ? sizeof(s_h2_desktop_ble_value)
                    : ch->max_value_len;
            s_h2_desktop_ble_value_read = ch->read;
            s_h2_desktop_ble_value_write = ch->write;
            s_h2_desktop_ble_value_user = ch->user;
            if (ch->initial_value_len > 0u) {
                memcpy(s_h2_desktop_ble_value, ch->initial_value,
                       ch->initial_value_len);
            }
            s_h2_desktop_ble_value_len = ch->initial_value_len;
            if (ch->out_value_handle != NULL) {
                *ch->out_value_handle = H2_DESKTOP_BLE_VALUE_HANDLE;
            }
            if (ch->out_cccd_handle != NULL) {
                *ch->out_cccd_handle = H2_DESKTOP_BLE_CCCD_HANDLE;
            }
        }
    }
    return H2_PAL_OK;
}

static h2_pal_result_t h2_desktop_ble_unregister_gatt_services(
    h2_pal_ble_t *ble) {
    (void)ble;
    memset(s_h2_desktop_ble_service_uuid, 0,
           sizeof(s_h2_desktop_ble_service_uuid));
    s_h2_desktop_ble_service_uuid_len = 0u;
    memset(s_h2_desktop_ble_value, 0, sizeof(s_h2_desktop_ble_value));
    s_h2_desktop_ble_value_len = 0u;
    s_h2_desktop_ble_value_max_len = sizeof(s_h2_desktop_ble_value);
    s_h2_desktop_ble_value_permissions = 0u;
    s_h2_desktop_ble_value_read = NULL;
    s_h2_desktop_ble_value_write = NULL;
    s_h2_desktop_ble_value_user = NULL;
    return H2_PAL_OK;
}

static h2_pal_result_t h2_desktop_ble_notify(
    h2_pal_ble_t *ble,
    uint16_t conn_handle,
    uint16_t attr_handle,
    const uint8_t *data,
    size_t len) {
    (void)ble;
    (void)data;
    return conn_handle != H2_PAL_BLE_INVALID_CONN_HANDLE &&
                   attr_handle != H2_PAL_BLE_INVALID_ATTR_HANDLE &&
                   (len == 0u || data != NULL)
               ? H2_PAL_OK
               : H2_PAL_ERR_INVALID_ARG;
}

static h2_pal_result_t h2_desktop_ble_indicate(
    h2_pal_ble_t *ble,
    uint16_t conn_handle,
    uint16_t attr_handle,
    const uint8_t *data,
    size_t len,
    uint32_t timeout_ms) {
    (void)ble;
    (void)conn_handle;
    (void)attr_handle;
    (void)data;
    (void)len;
    (void)timeout_ms;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t h2_desktop_ble_connect(
    h2_pal_ble_t *ble,
    const h2_pal_ble_addr_t *addr,
    const h2_pal_ble_connect_params_t *params,
    uint16_t *out_conn_handle) {
    (void)ble;
    (void)addr;
    if (params == NULL || out_conn_handle == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_conn_handle = H2_DESKTOP_BLE_CONN_HANDLE;
    s_h2_desktop_ble_paired = 0;
    return H2_PAL_OK;
}

static h2_pal_result_t h2_desktop_ble_configure_pairing(
    h2_pal_ble_t *ble,
    const h2_pal_ble_pairing_config_t *config) {
    (void)ble;
    if (config == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    s_h2_desktop_ble_pairing = *config;
    if (!config->enabled) {
        memset(&s_h2_desktop_ble_pairing, 0,
               sizeof(s_h2_desktop_ble_pairing));
        s_h2_desktop_ble_paired = 0;
    }
    return H2_PAL_OK;
}

static h2_pal_result_t h2_desktop_ble_pair(
    h2_pal_ble_t *ble,
    uint16_t conn_handle,
    uint32_t timeout_ms) {
    (void)ble;
    if (conn_handle != H2_DESKTOP_BLE_CONN_HANDLE || timeout_ms == 0u) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (!s_h2_desktop_ble_pairing.enabled) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    s_h2_desktop_ble_paired = 1;
    return H2_PAL_OK;
}

static h2_pal_result_t h2_desktop_ble_disconnect(
    h2_pal_ble_t *ble,
    uint16_t conn_handle) {
    (void)ble;
    if (conn_handle != H2_DESKTOP_BLE_CONN_HANDLE) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    s_h2_desktop_ble_paired = 0;
    return H2_PAL_OK;
}

static h2_pal_result_t h2_desktop_ble_gatt_discover(
    h2_pal_ble_t *ble,
    uint16_t conn_handle,
    const h2_pal_ble_gatt_discovery_request_t *request,
    h2_pal_ble_gatt_discovery_entry_t *entries,
    size_t max_entries,
    size_t *out_count,
    uint32_t timeout_ms) {
    (void)ble;
    (void)timeout_ms;
    if (conn_handle == H2_PAL_BLE_INVALID_CONN_HANDLE ||
        request == NULL ||
        out_count == NULL ||
        (max_entries > 0u && entries == NULL)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_count = 0u;
    if (max_entries == 0u) {
        return H2_PAL_OK;
    }
    memset(&entries[0], 0, sizeof(entries[0]));
    entries[0].kind = request->kind;
    entries[0].uuid.data = s_h2_desktop_ble_service_uuid;
    entries[0].uuid.len = s_h2_desktop_ble_service_uuid_len;
    entries[0].start_handle = 1u;
    entries[0].end_handle = 4u;
    entries[0].value_handle = H2_DESKTOP_BLE_VALUE_HANDLE;
    entries[0].properties = H2_PAL_BLE_GATT_PROPERTY_READ |
                             H2_PAL_BLE_GATT_PROPERTY_WRITE |
                             H2_PAL_BLE_GATT_PROPERTY_NOTIFY;
    *out_count = 1u;
    return H2_PAL_OK;
}

static h2_pal_result_t h2_desktop_ble_gatt_read(
    h2_pal_ble_t *ble,
    uint16_t conn_handle,
    uint16_t attr_handle,
    uint16_t offset,
    uint8_t *out,
    size_t out_size,
    size_t *out_len,
    uint32_t timeout_ms) {
    (void)ble;
    (void)timeout_ms;
    if (conn_handle == H2_PAL_BLE_INVALID_CONN_HANDLE ||
        attr_handle == H2_PAL_BLE_INVALID_ATTR_HANDLE ||
        out_len == NULL ||
        (out_size > 0u && out == NULL)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_len = 0u;
    if ((s_h2_desktop_ble_value_permissions &
         (H2_PAL_BLE_GATT_PERMISSION_READ_ENCRYPTED |
          H2_PAL_BLE_GATT_PERMISSION_READ_AUTHENTICATED)) != 0u &&
        !s_h2_desktop_ble_paired) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    if (s_h2_desktop_ble_value_read != NULL) {
        const h2_pal_ble_gatt_access_t access = {
            .conn_handle = conn_handle,
            .attr_handle = attr_handle,
            .offset = offset,
        };
        return s_h2_desktop_ble_value_read(
            s_h2_desktop_ble_value_user, &access, out, out_size, out_len);
    }
    if (offset >= s_h2_desktop_ble_value_len) {
        return H2_PAL_OK;
    }
    size_t len = s_h2_desktop_ble_value_len - offset;
    if (len > out_size) {
        len = out_size;
    }
    memcpy(out, s_h2_desktop_ble_value + offset, len);
    *out_len = len;
    return H2_PAL_OK;
}

static h2_pal_result_t h2_desktop_ble_gatt_write(
    h2_pal_ble_t *ble,
    uint16_t conn_handle,
    uint16_t attr_handle,
    const uint8_t *data,
    size_t len,
    bool with_response,
    uint32_t timeout_ms) {
    (void)ble;
    (void)sizeof(with_response);
    (void)timeout_ms;
    if (conn_handle == H2_PAL_BLE_INVALID_CONN_HANDLE ||
        attr_handle == H2_PAL_BLE_INVALID_ATTR_HANDLE ||
        (len > 0u && data == NULL) ||
        len > s_h2_desktop_ble_value_max_len) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if ((s_h2_desktop_ble_value_permissions &
         (H2_PAL_BLE_GATT_PERMISSION_WRITE_ENCRYPTED |
          H2_PAL_BLE_GATT_PERMISSION_WRITE_AUTHENTICATED)) != 0u &&
        !s_h2_desktop_ble_paired) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    if (s_h2_desktop_ble_value_write != NULL) {
        const h2_pal_ble_gatt_access_t access = {
            .conn_handle = conn_handle,
            .attr_handle = attr_handle,
        };
        return s_h2_desktop_ble_value_write(
            s_h2_desktop_ble_value_user, &access, data, len);
    }
    if (len > 0u) {
        memcpy(s_h2_desktop_ble_value, data, len);
    }
    s_h2_desktop_ble_value_len = len;
    return H2_PAL_OK;
}

static h2_pal_result_t h2_desktop_ble_gatt_subscribe(
    h2_pal_ble_t *ble,
    uint16_t conn_handle,
    const h2_pal_ble_gatt_subscribe_t *subscribe,
    uint32_t timeout_ms) {
    (void)ble;
    (void)timeout_ms;
    return conn_handle != H2_PAL_BLE_INVALID_CONN_HANDLE && subscribe != NULL
               ? H2_PAL_OK
               : H2_PAL_ERR_INVALID_ARG;
}

#define H2_DESKTOP_BLE_API(user) ((h2_pal_ble_t *)(user))

static h2_pal_result_t h2_desktop_ble_start_adapter(void *user) {
    return h2_desktop_ble_start(H2_DESKTOP_BLE_API(user));
}

static h2_pal_result_t h2_desktop_ble_stop_adapter(void *user) {
    return h2_desktop_ble_stop(H2_DESKTOP_BLE_API(user));
}

static h2_pal_result_t h2_desktop_ble_set_adv_data_adapter(void *user, const h2_pal_ble_adv_data_t *data) {
    return h2_desktop_ble_set_adv_data(H2_DESKTOP_BLE_API(user), data);
}

static h2_pal_result_t h2_desktop_ble_start_advertising_adapter(void *user, const h2_pal_ble_adv_params_t *params) {
    return h2_desktop_ble_start_advertising(H2_DESKTOP_BLE_API(user), params);
}

static h2_pal_result_t h2_desktop_ble_stop_advertising_adapter(void *user) {
    return h2_desktop_ble_stop_advertising(H2_DESKTOP_BLE_API(user));
}

static h2_pal_result_t h2_desktop_ble_adv_set_create_adapter(
    void *user,
    const h2_pal_ble_adv_params_t *params,
    h2_pal_ble_adv_set_t **out_set) {
    return h2_desktop_ble_adv_set_create(
        H2_DESKTOP_BLE_API(user), params, out_set);
}

static h2_pal_result_t h2_desktop_ble_adv_set_set_data_adapter(
    void *user,
    h2_pal_ble_adv_set_t *set,
    const h2_pal_ble_adv_data_t *data) {
    return h2_desktop_ble_adv_set_set_data(
        H2_DESKTOP_BLE_API(user), set, data);
}

static h2_pal_result_t h2_desktop_ble_adv_set_set_encoded_data_adapter(
    void *user,
    h2_pal_ble_adv_set_t *set,
    const uint8_t *encoded_data,
    size_t encoded_data_len) {
    return h2_desktop_ble_adv_set_set_encoded_data(
        H2_DESKTOP_BLE_API(user), set, encoded_data, encoded_data_len);
}

static h2_pal_result_t
h2_desktop_ble_adv_set_set_scan_response_data_adapter(
    void *user,
    h2_pal_ble_adv_set_t *set,
    const h2_pal_ble_adv_data_t *data) {
    (void)user;
    (void)set;
    (void)data;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t h2_desktop_ble_adv_set_start_adapter(
    void *user,
    h2_pal_ble_adv_set_t *set) {
    return h2_desktop_ble_adv_set_start(H2_DESKTOP_BLE_API(user), set);
}

static h2_pal_result_t h2_desktop_ble_adv_set_stop_adapter(
    void *user,
    h2_pal_ble_adv_set_t *set) {
    return h2_desktop_ble_adv_set_stop(H2_DESKTOP_BLE_API(user), set);
}

static h2_pal_result_t h2_desktop_ble_adv_set_destroy_adapter(
    void *user,
    h2_pal_ble_adv_set_t *set) {
    return h2_desktop_ble_adv_set_destroy(H2_DESKTOP_BLE_API(user), set);
}

static h2_pal_result_t h2_desktop_ble_start_scan_adapter(
    void *user,
    const h2_pal_ble_scan_params_t *params,
    h2_pal_ble_scan_result_fn on_result,
    void *scan_user) {
    return h2_desktop_ble_start_scan(H2_DESKTOP_BLE_API(user), params, on_result, scan_user);
}

static h2_pal_result_t h2_desktop_ble_stop_scan_adapter(void *user) {
    return h2_desktop_ble_stop_scan(H2_DESKTOP_BLE_API(user));
}

static h2_pal_result_t h2_desktop_ble_register_gatt_services_adapter(
    void *user,
    const h2_pal_ble_gatt_service_t *services,
    size_t count) {
    return h2_desktop_ble_register_gatt_services(H2_DESKTOP_BLE_API(user), services, count);
}

static h2_pal_result_t h2_desktop_ble_unregister_gatt_services_adapter(void *user) {
    return h2_desktop_ble_unregister_gatt_services(H2_DESKTOP_BLE_API(user));
}

static h2_pal_result_t h2_desktop_ble_notify_adapter(
    void *user,
    uint16_t conn_handle,
    uint16_t attr_handle,
    const uint8_t *data,
    size_t len) {
    return h2_desktop_ble_notify(H2_DESKTOP_BLE_API(user), conn_handle, attr_handle, data, len);
}

static h2_pal_result_t h2_desktop_ble_indicate_adapter(
    void *user,
    uint16_t conn_handle,
    uint16_t attr_handle,
    const uint8_t *data,
    size_t len,
    uint32_t timeout_ms) {
    return h2_desktop_ble_indicate(
        H2_DESKTOP_BLE_API(user), conn_handle, attr_handle, data, len,
        timeout_ms);
}

static h2_pal_result_t h2_desktop_ble_connect_adapter(
    void *user,
    const h2_pal_ble_addr_t *addr,
    const h2_pal_ble_connect_params_t *params,
    uint16_t *out_conn_handle) {
    return h2_desktop_ble_connect(H2_DESKTOP_BLE_API(user), addr, params, out_conn_handle);
}

static h2_pal_result_t h2_desktop_ble_configure_pairing_adapter(
    void *user,
    const h2_pal_ble_pairing_config_t *config) {
    return h2_desktop_ble_configure_pairing(
        H2_DESKTOP_BLE_API(user), config);
}

static h2_pal_result_t h2_desktop_ble_pair_adapter(
    void *user,
    uint16_t conn_handle,
    uint32_t timeout_ms) {
    return h2_desktop_ble_pair(
        H2_DESKTOP_BLE_API(user), conn_handle, timeout_ms);
}

static h2_pal_result_t h2_desktop_ble_disconnect_adapter(void *user, uint16_t conn_handle) {
    return h2_desktop_ble_disconnect(H2_DESKTOP_BLE_API(user), conn_handle);
}

static h2_pal_result_t h2_desktop_ble_gatt_discover_adapter(
    void *user,
    uint16_t conn_handle,
    const h2_pal_ble_gatt_discovery_request_t *request,
    h2_pal_ble_gatt_discovery_entry_t *entries,
    size_t max_entries,
    size_t *out_count,
    uint32_t timeout_ms) {
    return h2_desktop_ble_gatt_discover(
        H2_DESKTOP_BLE_API(user),
        conn_handle,
        request,
        entries,
        max_entries,
        out_count,
        timeout_ms);
}

static h2_pal_result_t h2_desktop_ble_gatt_read_adapter(
    void *user,
    uint16_t conn_handle,
    uint16_t attr_handle,
    uint16_t offset,
    uint8_t *out,
    size_t out_size,
    size_t *out_len,
    uint32_t timeout_ms) {
    return h2_desktop_ble_gatt_read(
        H2_DESKTOP_BLE_API(user),
        conn_handle,
        attr_handle,
        offset,
        out,
        out_size,
        out_len,
        timeout_ms);
}

static h2_pal_result_t h2_desktop_ble_gatt_write_adapter(
    void *user,
    uint16_t conn_handle,
    uint16_t attr_handle,
    const uint8_t *data,
    size_t len,
    bool with_response,
    uint32_t timeout_ms) {
    return h2_desktop_ble_gatt_write(
        H2_DESKTOP_BLE_API(user),
        conn_handle,
        attr_handle,
        data,
        len,
        with_response,
        timeout_ms);
}

static h2_pal_result_t h2_desktop_ble_gatt_subscribe_adapter(
    void *user,
    uint16_t conn_handle,
    const h2_pal_ble_gatt_subscribe_t *subscribe,
    uint32_t timeout_ms) {
    return h2_desktop_ble_gatt_subscribe(H2_DESKTOP_BLE_API(user), conn_handle, subscribe, timeout_ms);
}

static const h2_pal_ble_vtable_t s_h2_desktop_ble_vtable = {
    .start = h2_desktop_ble_start_adapter,
    .stop = h2_desktop_ble_stop_adapter,
    .set_adv_data = h2_desktop_ble_set_adv_data_adapter,
    .start_advertising = h2_desktop_ble_start_advertising_adapter,
    .stop_advertising = h2_desktop_ble_stop_advertising_adapter,
    .adv_set_create = h2_desktop_ble_adv_set_create_adapter,
    .adv_set_set_data = h2_desktop_ble_adv_set_set_data_adapter,
    .adv_set_set_encoded_data =
        h2_desktop_ble_adv_set_set_encoded_data_adapter,
    .adv_set_set_scan_response_data =
        h2_desktop_ble_adv_set_set_scan_response_data_adapter,
    .adv_set_start = h2_desktop_ble_adv_set_start_adapter,
    .adv_set_stop = h2_desktop_ble_adv_set_stop_adapter,
    .adv_set_destroy = h2_desktop_ble_adv_set_destroy_adapter,
    .start_scan = h2_desktop_ble_start_scan_adapter,
    .stop_scan = h2_desktop_ble_stop_scan_adapter,
    .register_gatt_services = h2_desktop_ble_register_gatt_services_adapter,
    .unregister_gatt_services = h2_desktop_ble_unregister_gatt_services_adapter,
    .notify = h2_desktop_ble_notify_adapter,
    .indicate = h2_desktop_ble_indicate_adapter,
    .connect = h2_desktop_ble_connect_adapter,
    .configure_pairing =
        h2_desktop_ble_configure_pairing_adapter,
    .pair = h2_desktop_ble_pair_adapter,
    .disconnect = h2_desktop_ble_disconnect_adapter,
    .gatt_discover = h2_desktop_ble_gatt_discover_adapter,
    .gatt_read = h2_desktop_ble_gatt_read_adapter,
    .gatt_write = h2_desktop_ble_gatt_write_adapter,
    .gatt_subscribe = h2_desktop_ble_gatt_subscribe_adapter,
};

static h2_pal_ble_t s_h2_desktop_ble = {
    .user = &s_h2_desktop_ble,
    .vtable = &s_h2_desktop_ble_vtable,
    .allocator = NULL,
};

h2_pal_ble_t *h2_desktop_platform_ble(
    const h2_pal_system_event_api_t *system_event) {
    if (!h2_desktop_ble_system_event_valid(system_event) ||
        (s_h2_desktop_ble_system_event != NULL &&
         s_h2_desktop_ble_system_event != system_event &&
         s_h2_desktop_ble_host_started)) {
        return NULL;
    }
    s_h2_desktop_ble_system_event = system_event;
    s_h2_desktop_ble.allocator = h2_desktop_platform_default_allocator();
    return &s_h2_desktop_ble;
}
