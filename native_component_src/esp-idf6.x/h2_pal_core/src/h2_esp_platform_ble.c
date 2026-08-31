#include "h2_esp_platform_core.h"
#include "h2_esp_platform_safe_call.h"
#include "h2_esp_ble_indication_tracker.h"

#include "sdkconfig.h"

#if CONFIG_BT_NIMBLE_ENABLED
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_sm.h"
#include "host/ble_store.h"
#include "host/ble_uuid.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nvs_flash.h"
#include "os/os_mbuf.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#if CONFIG_ESP_HOSTED_ENABLE_BT_NIMBLE
#include "esp_hosted.h"
#include "esp_hosted_misc.h"
#endif

#include <string.h>

#define H2_ESP_BLE_SYNCED_BIT BIT0
#define H2_ESP_BLE_SCAN_STOPPED_BIT BIT1
#define H2_ESP_BLE_ADV_STOPPED_BIT BIT2
#define H2_ESP_BLE_CONNECT_DONE_BIT BIT3
#define H2_ESP_BLE_GATT_DONE_BIT BIT4
#define H2_ESP_BLE_PHY_DONE_BIT BIT5
#define H2_ESP_BLE_INDICATION_DONE_BIT BIT21
#define H2_ESP_BLE_PAIR_DONE_BIT BIT20
#define H2_ESP_BLE_ADV_SET_STOPPED_BIT(instance) \
    (BIT6 << ((instance) - 1u))
#define H2_ESP_BLE_MAX_VALUE_LEN H2_PAL_BLE_ATT_MAX_VALUE_LEN
#define H2_ESP_BLE_MAX_DISCOVERY_ENTRIES 8u
#define H2_ESP_BLE_MAX_GATT_SERVICES 2u
#define H2_ESP_BLE_MAX_GATT_CHARACTERISTICS_PER_SERVICE 2u
#define H2_ESP_BLE_MAX_GATT_CHARACTERISTICS \
    (H2_ESP_BLE_MAX_GATT_SERVICES * \
     H2_ESP_BLE_MAX_GATT_CHARACTERISTICS_PER_SERVICE)
#define H2_ESP_BLE_SCAN_SOURCE_ID 1u
#define H2_ESP_BLE_GAP_STOP_TIMEOUT_MS 6000u
#define H2_ESP_BLE_LL_DATA_LEN 251u
#define H2_ESP_BLE_LL_DATA_TIME_US 2120u
#define H2_ESP_BLE_EXT_ADV_INSTANCE 0u
#define H2_ESP_BLE_AD_TYPE_FLAGS 0x01u
#define H2_ESP_BLE_AD_TYPE_UUID16_COMPLETE 0x03u
#define H2_ESP_BLE_AD_TYPE_UUID32_COMPLETE 0x05u
#define H2_ESP_BLE_AD_TYPE_UUID128_COMPLETE 0x07u
#define H2_ESP_BLE_AD_TYPE_NAME_COMPLETE 0x09u
#define H2_ESP_BLE_AD_TYPE_SERVICE_DATA16 0x16u
#define H2_ESP_BLE_AD_TYPE_SERVICE_DATA32 0x20u
#define H2_ESP_BLE_AD_TYPE_SERVICE_DATA128 0x21u
#define H2_ESP_BLE_AD_TYPE_MANUFACTURER 0xffu
#define H2_ESP_BLE_MAX_SCAN_UUIDS (127u + 63u + 15u)
#define H2_ESP_BLE_INIT_SAFE_STACK_DEPTH 4096u

#if CONFIG_BT_NIMBLE_EXT_ADV
struct h2_pal_ble_adv_set {
    h2_pal_ble_adv_params_t params;
    uint8_t instance;
    bool allocated;
    bool data_staged;
    bool active;
};
#endif

static const char *TAG = "h2_esp_ble";

static EventGroupHandle_t s_h2_esp_ble_events;
static SemaphoreHandle_t s_h2_esp_ble_gatt_mutex;
#if CONFIG_BT_NIMBLE_EXT_ADV
static SemaphoreHandle_t s_h2_esp_ble_adv_mutex;
#endif
static bool s_h2_esp_ble_started;
static portMUX_TYPE s_h2_esp_ble_indication_lock = portMUX_INITIALIZER_UNLOCKED;
static h2_esp_ble_indication_tracker_t s_h2_esp_ble_indication = {
    .conn_handle = H2_PAL_BLE_INVALID_CONN_HANDLE,
    .attr_handle = H2_PAL_BLE_INVALID_ATTR_HANDLE,
};
#if CONFIG_ESP_HOSTED_ENABLE_BT_NIMBLE
static bool s_h2_esp_ble_hosted_controller_initialized;
static bool s_h2_esp_ble_hosted_controller_enabled;
#endif
static uint8_t s_h2_esp_ble_own_addr_type;
static h2_pal_ble_scan_result_fn s_h2_esp_ble_scan_cb;
static void *s_h2_esp_ble_scan_user;
static bool s_h2_esp_ble_connect_pending;
static h2_pal_result_t s_h2_esp_ble_connect_result;
static uint16_t s_h2_esp_ble_connect_handle = H2_PAL_BLE_INVALID_CONN_HANDLE;
static h2_pal_result_t s_h2_esp_ble_gatt_result;
static uint16_t s_h2_esp_ble_exchange_mtu;
static h2_pal_result_t s_h2_esp_ble_phy_result;
static h2_pal_result_t s_h2_esp_ble_pair_result;
static h2_pal_ble_pairing_config_t s_h2_esp_ble_pairing;
static uint16_t s_h2_esp_ble_pair_conn_handle =
    H2_PAL_BLE_INVALID_CONN_HANDLE;
static struct {
    uint8_t io_cap;
    uint8_t bonding;
    uint8_t mitm;
    uint8_t secure_connections;
    bool valid;
} s_h2_esp_ble_previous_pairing;
static h2_pal_ble_phy_info_t s_h2_esp_ble_phy_info;
static h2_pal_ble_gatt_discovery_entry_t *s_h2_esp_ble_discovery_entries;
static size_t s_h2_esp_ble_discovery_max_entries;
static size_t s_h2_esp_ble_discovery_count;
static uint8_t s_h2_esp_ble_discovery_uuid_data[H2_ESP_BLE_MAX_DISCOVERY_ENTRIES][16];
static h2_pal_ble_uuid_t s_h2_esp_ble_discovery_uuid_filter;
static uint8_t s_h2_esp_ble_discovery_uuid_filter_data[16];
static uint8_t *s_h2_esp_ble_read_out;
static size_t s_h2_esp_ble_read_out_size;
static size_t s_h2_esp_ble_read_out_len;
static uint8_t s_h2_esp_ble_adv_data[H2_PAL_BLE_EXT_ADV_DATA_MAX_LEN];
static size_t s_h2_esp_ble_adv_data_len;
static uint8_t s_h2_esp_ble_legacy_adv_data[
    H2_PAL_BLE_LEGACY_ADV_DATA_MAX_LEN];
static size_t s_h2_esp_ble_legacy_adv_data_len;
static uint8_t s_h2_esp_ble_legacy_scan_response[
    H2_PAL_BLE_LEGACY_ADV_DATA_MAX_LEN];
static size_t s_h2_esp_ble_legacy_scan_response_len;
static bool s_h2_esp_ble_legacy_adv_data_valid;
static bool s_h2_esp_ble_adv_data_staged;
static bool s_h2_esp_ble_adv_active;
static bool s_h2_esp_ble_ext_adv_configured;
static h2_pal_ble_uuid_t s_h2_esp_ble_scan_uuids[H2_ESP_BLE_MAX_SCAN_UUIDS];
#if CONFIG_BT_NIMBLE_EXT_ADV
static h2_pal_ble_adv_set_t
    s_h2_esp_ble_adv_sets[CONFIG_BT_NIMBLE_MAX_EXT_ADV_INSTANCES];
#endif

static ble_uuid_any_t
    s_h2_esp_ble_service_uuid[H2_ESP_BLE_MAX_GATT_SERVICES];
static ble_uuid_any_t s_h2_esp_ble_char_uuid[H2_ESP_BLE_MAX_GATT_CHARACTERISTICS];
static uint8_t s_h2_esp_ble_value[H2_ESP_BLE_MAX_GATT_CHARACTERISTICS][H2_ESP_BLE_MAX_VALUE_LEN];
static size_t s_h2_esp_ble_value_len[H2_ESP_BLE_MAX_GATT_CHARACTERISTICS];
static size_t s_h2_esp_ble_value_max_len[H2_ESP_BLE_MAX_GATT_CHARACTERISTICS];
static h2_pal_ble_gatt_read_fn s_h2_esp_ble_read[H2_ESP_BLE_MAX_GATT_CHARACTERISTICS];
static h2_pal_ble_gatt_write_fn s_h2_esp_ble_write[H2_ESP_BLE_MAX_GATT_CHARACTERISTICS];
static void *s_h2_esp_ble_gatt_user[H2_ESP_BLE_MAX_GATT_CHARACTERISTICS];
static uint16_t s_h2_esp_ble_value_handle[H2_ESP_BLE_MAX_GATT_CHARACTERISTICS];
static uint8_t s_h2_esp_ble_char_index[H2_ESP_BLE_MAX_GATT_CHARACTERISTICS] = {
    0u, 1u, 2u, 3u,
};
static size_t s_h2_esp_ble_service_count;
static size_t
    s_h2_esp_ble_service_characteristic_count[H2_ESP_BLE_MAX_GATT_SERVICES];
static size_t s_h2_esp_ble_characteristic_count;
static uint16_t
    *s_h2_esp_ble_out_service_handle[H2_ESP_BLE_MAX_GATT_SERVICES];
static uint16_t *s_h2_esp_ble_out_value_handle[H2_ESP_BLE_MAX_GATT_CHARACTERISTICS];
static uint16_t *s_h2_esp_ble_out_cccd_handle[H2_ESP_BLE_MAX_GATT_CHARACTERISTICS];
static uint16_t s_h2_esp_ble_last_conn_handle = H2_PAL_BLE_INVALID_CONN_HANDLE;
static bool s_h2_esp_ble_gatt_configured;
static bool s_h2_esp_ble_gatt_attached;

typedef struct h2_esp_ble_init_call {
    esp_err_t result;
} h2_esp_ble_init_call_t;

static int h2_esp_ble_gap_event(struct ble_gap_event *event, void *arg);
static int h2_esp_ble_gatt_access(
    uint16_t conn_handle,
    uint16_t attr_handle,
    struct ble_gatt_access_ctxt *ctxt,
    void *arg);
static int h2_esp_ble_gatt_access_unlocked(
    uint16_t conn_handle,
    uint16_t attr_handle,
    struct ble_gatt_access_ctxt *ctxt,
    void *arg);

static void IRAM_ATTR h2_esp_ble_init_safe_callback(void *context) {
    h2_esp_ble_init_call_t *call = (h2_esp_ble_init_call_t *)context;
    call->result = nvs_flash_init();
    if (call->result == ESP_OK) {
        call->result = nimble_port_init();
    }
}

static bool h2_esp_ble_adv_put(
    uint8_t *out,
    size_t *out_len,
    uint8_t type,
    const uint8_t *data,
    size_t data_len) {
    if (data_len > 254u || (data_len > 0u && data == NULL) ||
        *out_len > H2_PAL_BLE_EXT_ADV_DATA_MAX_LEN - data_len - 2u) {
        return false;
    }
    out[(*out_len)++] = (uint8_t)(data_len + 1u);
    out[(*out_len)++] = type;
    if (data_len > 0u) {
        memcpy(&out[*out_len], data, data_len);
        *out_len += data_len;
    }
    return true;
}

static bool h2_esp_ble_adv_put_uuid_list(
    uint8_t *out,
    size_t *out_len,
    const h2_pal_ble_adv_data_t *data,
    size_t uuid_len,
    uint8_t type) {
    size_t list_len = 0u;
    for (size_t i = 0u; i < data->service_uuid_count; ++i) {
        if (data->service_uuids[i].len == uuid_len) {
            if (list_len > 254u - uuid_len) {
                return false;
            }
            list_len += uuid_len;
        }
    }
    if (list_len == 0u) {
        return true;
    }
    if (*out_len > H2_PAL_BLE_EXT_ADV_DATA_MAX_LEN - list_len - 2u) {
        return false;
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
    return true;
}

static h2_pal_result_t h2_esp_ble_encode_adv_data(
    const h2_pal_ble_adv_data_t *data,
    uint8_t *out,
    size_t *out_len) {
    uint8_t flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    size_t len = 0u;
    if (data == NULL || out == NULL || out_len == NULL ||
        (data->service_uuid_count > 0u && data->service_uuids == NULL) ||
        (data->manufacturer_data.len > 0u && data->manufacturer_data.data == NULL) ||
        (data->service_data_uuid.len > 0u && data->service_data_uuid.data == NULL) ||
        (data->service_data.len > 0u && data->service_data.data == NULL) ||
        !h2_esp_ble_adv_put(out, &len, H2_ESP_BLE_AD_TYPE_FLAGS, &flags, sizeof(flags))) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    for (size_t i = 0u; i < data->service_uuid_count; ++i) {
        const h2_pal_ble_uuid_t *uuid = &data->service_uuids[i];
        if ((uuid->len != 2u && uuid->len != 4u && uuid->len != 16u) || uuid->data == NULL) {
            return H2_PAL_ERR_INVALID_ARG;
        }
    }
    if (!h2_esp_ble_adv_put_uuid_list(
            out, &len, data, 2u, H2_ESP_BLE_AD_TYPE_UUID16_COMPLETE) ||
        !h2_esp_ble_adv_put_uuid_list(
            out, &len, data, 4u, H2_ESP_BLE_AD_TYPE_UUID32_COMPLETE) ||
        !h2_esp_ble_adv_put_uuid_list(
            out, &len, data, 16u, H2_ESP_BLE_AD_TYPE_UUID128_COMPLETE)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (data->manufacturer_data.len > 0u &&
        !h2_esp_ble_adv_put(out, &len, H2_ESP_BLE_AD_TYPE_MANUFACTURER,
            data->manufacturer_data.data, data->manufacturer_data.len)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (data->service_data.len > 0u) {
        uint8_t service_data[254u];
        size_t uuid_len = data->service_data_uuid.len;
        uint8_t type = H2_ESP_BLE_AD_TYPE_SERVICE_DATA16;
        if (uuid_len != 0u && uuid_len != 2u && uuid_len != 4u && uuid_len != 16u) {
            return H2_PAL_ERR_INVALID_ARG;
        }
        if (uuid_len + data->service_data.len > sizeof(service_data)) {
            return H2_PAL_ERR_INVALID_ARG;
        }
        if (uuid_len == 4u) type = H2_ESP_BLE_AD_TYPE_SERVICE_DATA32;
        if (uuid_len == 16u) type = H2_ESP_BLE_AD_TYPE_SERVICE_DATA128;
        if (uuid_len > 0u) memcpy(service_data, data->service_data_uuid.data, uuid_len);
        memcpy(service_data + uuid_len, data->service_data.data, data->service_data.len);
        if (!h2_esp_ble_adv_put(
                out, &len, type, service_data, uuid_len + data->service_data.len)) {
            return H2_PAL_ERR_INVALID_ARG;
        }
    }
    if (data->local_name != NULL) {
        size_t name_len = strlen(data->local_name);
        if (!h2_esp_ble_adv_put(out, &len, H2_ESP_BLE_AD_TYPE_NAME_COMPLETE,
                (const uint8_t *)data->local_name, name_len)) {
            return H2_PAL_ERR_INVALID_ARG;
        }
    }
    *out_len = len;
    return H2_PAL_OK;
}

#if CONFIG_BT_NIMBLE_EXT_ADV
static uint32_t h2_esp_ble_ms_to_ext_units625(uint32_t ms);

static uint8_t h2_esp_ble_ext_phy(h2_pal_ble_phy_t phy) {
    switch (phy) {
    case H2_PAL_BLE_PHY_2M:
        return BLE_HCI_LE_PHY_2M;
    case H2_PAL_BLE_PHY_CODED:
        return BLE_HCI_LE_PHY_CODED;
    default:
        return BLE_HCI_LE_PHY_1M;
    }
}

static void h2_esp_ble_fill_ext_adv_params(
    const h2_pal_ble_adv_params_t *params,
    struct ble_gap_ext_adv_params *out) {
    bool legacy = params->type == H2_PAL_BLE_ADV_TYPE_LEGACY;
    memset(out, 0, sizeof(*out));
    out->connectable = params->mode == H2_PAL_BLE_ADV_MODE_CONNECTABLE;
    out->scannable = legacy;
    out->legacy_pdu = legacy;
    out->itvl_min = h2_esp_ble_ms_to_ext_units625(params->interval_min_ms);
    out->itvl_max = h2_esp_ble_ms_to_ext_units625(params->interval_max_ms);
    out->own_addr_type = s_h2_esp_ble_own_addr_type;
    out->primary_phy = legacy ? BLE_HCI_LE_PHY_1M : h2_esp_ble_ext_phy(params->primary_phy);
    out->secondary_phy = legacy ? BLE_HCI_LE_PHY_1M : h2_esp_ble_ext_phy(params->secondary_phy);
    out->tx_power = 127;
    out->sid = legacy ? 0u : params->sid;
}
#endif

static int h2_esp_ble_gatt_access(
    uint16_t conn_handle,
    uint16_t attr_handle,
    struct ble_gatt_access_ctxt *ctxt,
    void *arg) {
    if (s_h2_esp_ble_gatt_mutex == NULL ||
        xSemaphoreTake(s_h2_esp_ble_gatt_mutex, portMAX_DELAY) != pdTRUE) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    int rc = h2_esp_ble_gatt_access_unlocked(
        conn_handle, attr_handle, ctxt, arg);
    (void)xSemaphoreGive(s_h2_esp_ble_gatt_mutex);
    return rc;
}

static struct ble_gatt_chr_def
    s_h2_esp_ble_chr_defs[H2_ESP_BLE_MAX_GATT_SERVICES]
                            [H2_ESP_BLE_MAX_GATT_CHARACTERISTICS_PER_SERVICE + 1u];

static struct ble_gatt_svc_def
    s_h2_esp_ble_svc_defs[H2_ESP_BLE_MAX_GATT_SERVICES + 1u];

void ble_store_config_init(void);

static uint64_t h2_esp_ble_now_ms(void) {
    return (uint64_t)(esp_timer_get_time() / 1000);
}

static h2_pal_ble_phy_t h2_esp_ble_phy(uint8_t phy) {
    switch (phy) {
    case BLE_GAP_LE_PHY_1M:
        return H2_PAL_BLE_PHY_1M;
    case BLE_GAP_LE_PHY_2M:
        return H2_PAL_BLE_PHY_2M;
    case BLE_GAP_LE_PHY_CODED:
        return H2_PAL_BLE_PHY_CODED;
    default:
        return H2_PAL_BLE_PHY_UNKNOWN;
    }
}

static uint8_t h2_esp_ble_phy_mask(h2_pal_ble_phy_t phy) {
    switch (phy) {
    case H2_PAL_BLE_PHY_1M:
        return BLE_GAP_LE_PHY_1M_MASK;
    case H2_PAL_BLE_PHY_2M:
        return BLE_GAP_LE_PHY_2M_MASK;
    case H2_PAL_BLE_PHY_CODED:
        return BLE_GAP_LE_PHY_CODED_MASK;
    default:
        return 0u;
    }
}

static void h2_esp_ble_post(h2_pal_system_event_type_t type, const void *payload, size_t payload_size) {
    h2_pal_system_event_t event = {
        .type = type,
        .source_id = H2_ESP_BLE_SCAN_SOURCE_ID,
        .timestamp_ms = h2_esp_ble_now_ms(),
        .payload = payload,
        .payload_size = payload_size,
    };
    (void)h2_pal_system_event_post(h2_esp_platform_system_event_api(), &event, 0u);
}

static void h2_esp_ble_finish_indication(
    uint16_t conn_handle,
    uint16_t attr_handle,
    bool match_attr,
    h2_pal_result_t result) {
    bool wake;

    taskENTER_CRITICAL(&s_h2_esp_ble_indication_lock);
    wake = h2_esp_ble_indication_tracker_finish(
        &s_h2_esp_ble_indication, conn_handle, attr_handle, match_attr,
        result);
    taskEXIT_CRITICAL(&s_h2_esp_ble_indication_lock);

    if (wake && s_h2_esp_ble_events != NULL) {
        xEventGroupSetBits(
            s_h2_esp_ble_events, H2_ESP_BLE_INDICATION_DONE_BIT);
    }
}

static h2_pal_result_t h2_esp_ble_begin_indication(
    uint16_t conn_handle,
    uint16_t attr_handle) {
    taskENTER_CRITICAL(&s_h2_esp_ble_indication_lock);
    h2_pal_result_t result = h2_esp_ble_indication_tracker_begin(
        &s_h2_esp_ble_indication, conn_handle, attr_handle);
    taskEXIT_CRITICAL(&s_h2_esp_ble_indication_lock);
    return result;
}

static void h2_esp_ble_cancel_indication_submission(
    uint16_t conn_handle,
    uint16_t attr_handle) {
    taskENTER_CRITICAL(&s_h2_esp_ble_indication_lock);
    h2_esp_ble_indication_tracker_cancel_submission(
        &s_h2_esp_ble_indication, conn_handle, attr_handle);
    taskEXIT_CRITICAL(&s_h2_esp_ble_indication_lock);
}

static void h2_esp_ble_complete_advertising(void) {
    if (!s_h2_esp_ble_adv_active) {
        return;
    }
    s_h2_esp_ble_adv_active = false;
    if (s_h2_esp_ble_events != NULL) {
        xEventGroupSetBits(s_h2_esp_ble_events, H2_ESP_BLE_ADV_STOPPED_BIT);
    }
    h2_esp_ble_post(H2_PAL_SYSTEM_EVENT_TYPE_BLE_ADVERTISING_STOPPED, NULL, 0u);
}

#if CONFIG_BT_NIMBLE_EXT_ADV
static h2_pal_result_t h2_esp_ble_ensure_adv_mutex(void) {
    if (s_h2_esp_ble_adv_mutex == NULL) {
        s_h2_esp_ble_adv_mutex = xSemaphoreCreateMutex();
    }
    return s_h2_esp_ble_adv_mutex != NULL ? H2_PAL_OK : H2_PAL_ERR_NO_MEMORY;
}

static h2_pal_result_t h2_esp_ble_lock_adv(void) {
    return s_h2_esp_ble_adv_mutex != NULL &&
                   xSemaphoreTake(s_h2_esp_ble_adv_mutex, portMAX_DELAY) == pdTRUE
               ? H2_PAL_OK
               : H2_PAL_ERR_INVALID_STATE;
}

static void h2_esp_ble_unlock_adv(void) {
    if (s_h2_esp_ble_adv_mutex != NULL) {
        (void)xSemaphoreGive(s_h2_esp_ble_adv_mutex);
    }
}

static bool h2_esp_ble_adv_set_valid_unlocked(const h2_pal_ble_adv_set_t *set) {
    uintptr_t address = (uintptr_t)set;
    return address >= (uintptr_t)&s_h2_esp_ble_adv_sets[1] &&
           address < (uintptr_t)&s_h2_esp_ble_adv_sets[CONFIG_BT_NIMBLE_MAX_EXT_ADV_INSTANCES] &&
           (address - (uintptr_t)&s_h2_esp_ble_adv_sets[0]) %
                   sizeof(s_h2_esp_ble_adv_sets[0]) ==
               0u &&
           set->allocated;
}

static void h2_esp_ble_complete_adv_set_unlocked(
    h2_pal_ble_adv_set_t *set,
    h2_pal_result_t status) {
    if (!h2_esp_ble_adv_set_valid_unlocked(set) || !set->active) {
        return;
    }
    set->active = false;
    if (s_h2_esp_ble_events != NULL) {
        xEventGroupSetBits(
            s_h2_esp_ble_events,
            H2_ESP_BLE_ADV_SET_STOPPED_BIT(set->instance));
    }
    h2_pal_ble_adv_set_event_t payload = {
        .set = set,
        .status = status,
    };
    h2_esp_ble_post(
        H2_PAL_SYSTEM_EVENT_TYPE_BLE_ADVERTISING_STOPPED,
        &payload,
        sizeof(payload));
}

static void h2_esp_ble_complete_adv_set(
    h2_pal_ble_adv_set_t *set,
    h2_pal_result_t status) {
    if (h2_esp_ble_lock_adv() != H2_PAL_OK) {
        return;
    }
    h2_esp_ble_complete_adv_set_unlocked(set, status);
    h2_esp_ble_unlock_adv();
}
#endif

static void h2_esp_ble_complete_scan(void) {
    if (s_h2_esp_ble_scan_cb == NULL) {
        return;
    }
    s_h2_esp_ble_scan_cb = NULL;
    s_h2_esp_ble_scan_user = NULL;
    if (s_h2_esp_ble_events != NULL) {
        xEventGroupSetBits(s_h2_esp_ble_events, H2_ESP_BLE_SCAN_STOPPED_BIT);
    }
    h2_esp_ble_post(H2_PAL_SYSTEM_EVENT_TYPE_BLE_SCAN_STOPPED, NULL, 0u);
}

static h2_pal_result_t h2_esp_ble_map_rc(int rc) {
    if (rc == 0 || rc == BLE_HS_EALREADY) {
        return H2_PAL_OK;
    }
    if (rc == BLE_HS_EAGAIN || rc == BLE_HS_EBUSY ||
        rc == BLE_HS_HCI_ERR(BLE_ERR_MEM_CAPACITY)) {
        return H2_PAL_ERR_WOULD_BLOCK;
    }
    if (rc == BLE_HS_ETIMEOUT) {
        return H2_PAL_ERR_TIMEOUT;
    }
    if (rc == BLE_HS_ENOMEM) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    if (rc == BLE_HS_EINVAL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (rc == BLE_HS_ENOTSUP) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return H2_PAL_ERR_IO;
}

static h2_pal_result_t h2_esp_ble_map_tx_rc(int rc) {
    if (rc == BLE_HS_EAGAIN || rc == BLE_HS_ENOMEM || rc == BLE_HS_EBUSY) {
        return H2_PAL_ERR_WOULD_BLOCK;
    }
    return h2_esp_ble_map_rc(rc);
}

static uint16_t h2_esp_ble_uuid16_value(const h2_pal_ble_uuid_t *uuid) {
    if (uuid == NULL || uuid->data == NULL || uuid->len != 2u) {
        return 0u;
    }
    return (uint16_t)uuid->data[0] | ((uint16_t)uuid->data[1] << 8);
}

static bool h2_esp_ble_uuid_from_pal(
    const h2_pal_ble_uuid_t *uuid,
    ble_uuid_any_t *out) {
    if (uuid == NULL || out == NULL || uuid->data == NULL ||
        (uuid->len != 2u && uuid->len != 4u && uuid->len != 16u)) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    return ble_uuid_init_from_buf(out, uuid->data, uuid->len) == 0;
}

static void h2_esp_ble_update_out_handles(void) {
    for (size_t service = 0u; service < s_h2_esp_ble_service_count;
         ++service) {
        if (s_h2_esp_ble_out_service_handle[service] == NULL) {
            continue;
        }
        size_t first =
            service * H2_ESP_BLE_MAX_GATT_CHARACTERISTICS_PER_SERVICE;
        uint16_t value_handle = s_h2_esp_ble_value_handle[first];
        *s_h2_esp_ble_out_service_handle[service] =
            value_handle >= 2u ? (uint16_t)(value_handle - 2u) : 0u;
    }
    for (size_t i = 0u; i < s_h2_esp_ble_characteristic_count; ++i) {
        if (s_h2_esp_ble_out_value_handle[i] != NULL) {
            *s_h2_esp_ble_out_value_handle[i] = s_h2_esp_ble_value_handle[i];
        }
        if (s_h2_esp_ble_out_cccd_handle[i] != NULL) {
            *s_h2_esp_ble_out_cccd_handle[i] =
                s_h2_esp_ble_value_handle[i] != 0u
                    ? (uint16_t)(s_h2_esp_ble_value_handle[i] + 1u)
                    : 0u;
        }
    }
}

static void h2_esp_ble_clear_out_handle_ptrs(void) {
    for (size_t i = 0u; i < H2_ESP_BLE_MAX_GATT_SERVICES; ++i) {
        s_h2_esp_ble_out_service_handle[i] = NULL;
    }
    for (size_t i = 0u; i < H2_ESP_BLE_MAX_GATT_CHARACTERISTICS; ++i) {
        s_h2_esp_ble_out_value_handle[i] = NULL;
        s_h2_esp_ble_out_cccd_handle[i] = NULL;
    }
}

static uint16_t h2_esp_ble_ms_to_units1250(uint32_t ms) {
    uint32_t units = (ms * 800u) / 1000u;
    if (units == 0u) {
        units = 0x18u;
    }
    if (units > 0x0c80u) {
        units = 0x0c80u;
    }
    return (uint16_t)units;
}

static uint16_t h2_esp_ble_ms_to_units10(uint32_t ms) {
    uint32_t units = ms / 10u;
    if (units == 0u) {
        units = 200u;
    }
    if (units > 0x0c80u) {
        units = 0x0c80u;
    }
    return (uint16_t)units;
}

static uint8_t h2_esp_ble_to_nimble_addr_type(h2_pal_ble_addr_type_t type) {
    switch (type) {
    case H2_PAL_BLE_ADDR_TYPE_PUBLIC:
        return BLE_ADDR_PUBLIC;
    case H2_PAL_BLE_ADDR_TYPE_RANDOM:
        return BLE_ADDR_RANDOM;
    case H2_PAL_BLE_ADDR_TYPE_PUBLIC_IDENTITY:
        return BLE_ADDR_PUBLIC_ID;
    case H2_PAL_BLE_ADDR_TYPE_RANDOM_IDENTITY:
        return BLE_ADDR_RANDOM_ID;
    default:
        return BLE_ADDR_RANDOM;
    }
}

static void h2_esp_ble_copy_uuid(
    const ble_uuid_any_t *src,
    h2_pal_ble_uuid_t *dst,
    uint8_t *storage) {
    if (dst == NULL) {
        return;
    }
    memset(dst, 0, sizeof(*dst));
    if (src == NULL || storage == NULL) {
        return;
    }
    switch (src->u.type) {
    case BLE_UUID_TYPE_16:
        storage[0] = (uint8_t)(src->u16.value & 0xffu);
        storage[1] = (uint8_t)(src->u16.value >> 8);
        dst->data = storage;
        dst->len = 2u;
        break;
    case BLE_UUID_TYPE_32:
        storage[0] = (uint8_t)(src->u32.value & 0xffu);
        storage[1] = (uint8_t)((src->u32.value >> 8) & 0xffu);
        storage[2] = (uint8_t)((src->u32.value >> 16) & 0xffu);
        storage[3] = (uint8_t)(src->u32.value >> 24);
        dst->data = storage;
        dst->len = 4u;
        break;
    case BLE_UUID_TYPE_128:
        memcpy(storage, src->u128.value, 16u);
        dst->data = storage;
        dst->len = 16u;
        break;
    default:
        break;
    }
}

static bool h2_esp_ble_discovery_uuid_matches(const ble_uuid_any_t *uuid) {
    if (s_h2_esp_ble_discovery_uuid_filter.len == 0u) {
        return true;
    }

    uint8_t uuid_data[16];
    h2_pal_ble_uuid_t discovered_uuid;
    h2_esp_ble_copy_uuid(uuid, &discovered_uuid, uuid_data);
    return discovered_uuid.len == s_h2_esp_ble_discovery_uuid_filter.len &&
           memcmp(discovered_uuid.data,
                  s_h2_esp_ble_discovery_uuid_filter.data,
                  discovered_uuid.len) == 0;
}

static uint16_t h2_esp_ble_ms_to_units625(uint32_t ms) {
    uint32_t units = (ms * 1600u) / 1000u;
    if (units == 0u) {
        units = 0x20u;
    }
    if (units > 0x4000u) {
        units = 0x4000u;
    }
    return (uint16_t)units;
}

#if CONFIG_BT_NIMBLE_EXT_ADV
static uint16_t h2_esp_ble_ext_scan_ms_to_units625(uint32_t ms) {
    uint32_t units = (ms * 1600u) / 1000u;
    return units > UINT16_MAX ? UINT16_MAX : (uint16_t)units;
}

static uint32_t h2_esp_ble_ms_to_ext_units625(uint32_t ms) {
    return (uint32_t)(((uint64_t)ms * 1600u) / 1000u);
}
#endif

static void h2_esp_ble_on_reset(int reason) {
    ESP_LOGW(TAG, "NimBLE reset reason=%d", reason);
}

static void h2_esp_ble_on_sync(void) {
    int rc = ble_hs_id_infer_auto(0, &s_h2_esp_ble_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_id_infer_auto failed rc=%d", rc);
        return;
    }
    if (s_h2_esp_ble_events != NULL) {
        xEventGroupSetBits(s_h2_esp_ble_events, H2_ESP_BLE_SYNCED_BIT);
    }
    h2_esp_ble_post(H2_PAL_SYSTEM_EVENT_TYPE_BLE_HOST_STARTED, NULL, 0u);
}

static h2_pal_result_t h2_esp_ble_wait_bit(EventBits_t bit, uint32_t timeout_ms) {
    if (s_h2_esp_ble_events == NULL) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    EventBits_t bits = xEventGroupWaitBits(
        s_h2_esp_ble_events,
        bit,
        pdTRUE,
        pdTRUE,
        pdMS_TO_TICKS(timeout_ms == 0u ? H2_ESP_BLE_GAP_STOP_TIMEOUT_MS : timeout_ms));
    return (bits & bit) != 0u ? H2_PAL_OK : H2_PAL_ERR_TIMEOUT;
}

static void h2_esp_ble_host_task(void *param) {
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static h2_pal_ble_addr_type_t h2_esp_ble_addr_type(uint8_t type) {
    switch (type) {
    case BLE_ADDR_PUBLIC:
        return H2_PAL_BLE_ADDR_TYPE_PUBLIC;
    case BLE_ADDR_RANDOM:
        return H2_PAL_BLE_ADDR_TYPE_RANDOM;
    case BLE_ADDR_PUBLIC_ID:
        return H2_PAL_BLE_ADDR_TYPE_PUBLIC_IDENTITY;
    case BLE_ADDR_RANDOM_ID:
        return H2_PAL_BLE_ADDR_TYPE_RANDOM_IDENTITY;
    default:
        return H2_PAL_BLE_ADDR_TYPE_UNKNOWN;
    }
}

static void h2_esp_ble_apply_adv_fields(
    h2_pal_ble_scan_result_t *result,
    const struct ble_hs_adv_fields *fields) {
    size_t uuid_count = 0u;
    if (fields->name != NULL && fields->name_len > 0u) {
        result->local_name = (const char *)fields->name;
        result->local_name_len = fields->name_len;
    }
    if (fields->mfg_data != NULL && fields->mfg_data_len > 0u) {
        result->manufacturer_data.data = fields->mfg_data;
        result->manufacturer_data.len = fields->mfg_data_len;
    }
    if (fields->svc_data_uuid16 != NULL && fields->svc_data_uuid16_len > 0u) {
        result->service_data.data = fields->svc_data_uuid16;
        result->service_data.len = fields->svc_data_uuid16_len;
    } else if (fields->svc_data_uuid32 != NULL && fields->svc_data_uuid32_len > 0u) {
        result->service_data.data = fields->svc_data_uuid32;
        result->service_data.len = fields->svc_data_uuid32_len;
    } else if (fields->svc_data_uuid128 != NULL && fields->svc_data_uuid128_len > 0u) {
        result->service_data.data = fields->svc_data_uuid128;
        result->service_data.len = fields->svc_data_uuid128_len;
    }
    for (size_t i = 0u; i < fields->num_uuids16; ++i) {
        s_h2_esp_ble_scan_uuids[uuid_count].data =
            (const uint8_t *)&fields->uuids16[i].value;
        s_h2_esp_ble_scan_uuids[uuid_count].len = 2u;
        ++uuid_count;
    }
    for (size_t i = 0u; i < fields->num_uuids32; ++i) {
        s_h2_esp_ble_scan_uuids[uuid_count].data =
            (const uint8_t *)&fields->uuids32[i].value;
        s_h2_esp_ble_scan_uuids[uuid_count].len = 4u;
        ++uuid_count;
    }
    for (size_t i = 0u; i < fields->num_uuids128; ++i) {
        s_h2_esp_ble_scan_uuids[uuid_count].data = fields->uuids128[i].value;
        s_h2_esp_ble_scan_uuids[uuid_count].len = 16u;
        ++uuid_count;
    }
    result->service_uuids = uuid_count > 0u ? s_h2_esp_ble_scan_uuids : NULL;
    result->service_uuid_count = uuid_count;
}

static int h2_esp_ble_gap_event(struct ble_gap_event *event, void *arg) {
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_DISC: {
        struct ble_hs_adv_fields fields;
        memset(&fields, 0, sizeof(fields));
        (void)ble_hs_adv_parse_fields(&fields, event->disc.data, event->disc.length_data);

        h2_pal_ble_scan_result_t result;
        memset(&result, 0, sizeof(result));
        result.addr.type = h2_esp_ble_addr_type(event->disc.addr.type);
        memcpy(result.addr.value, event->disc.addr.val, sizeof(result.addr.value));
        result.rssi = event->disc.rssi;
        result.connectable = event->disc.event_type == BLE_HCI_ADV_RPT_EVTYPE_ADV_IND ||
                             event->disc.event_type == BLE_HCI_ADV_RPT_EVTYPE_DIR_IND;
        result.scan_response = event->disc.event_type == BLE_HCI_ADV_RPT_EVTYPE_SCAN_RSP;
        result.adv_type = H2_PAL_BLE_ADV_TYPE_LEGACY;
        result.primary_phy = H2_PAL_BLE_PHY_1M;
        result.tx_power = 127;
        result.raw_data.data = event->disc.data;
        result.raw_data.len = event->disc.length_data;
        h2_esp_ble_apply_adv_fields(&result, &fields);

        if (s_h2_esp_ble_scan_cb != NULL &&
            s_h2_esp_ble_scan_cb(s_h2_esp_ble_scan_user, &result)) {
            (void)ble_gap_disc_cancel();
        }
        return 0;
    }
#if CONFIG_BT_NIMBLE_EXT_ADV
    case BLE_GAP_EVENT_EXT_DISC: {
        const struct ble_gap_ext_disc_desc *report = &event->ext_disc;
        struct ble_hs_adv_fields fields;
        memset(&fields, 0, sizeof(fields));
        (void)ble_hs_adv_parse_fields(&fields, report->data, report->length_data);

        h2_pal_ble_scan_result_t result;
        memset(&result, 0, sizeof(result));
        result.addr.type = h2_esp_ble_addr_type(report->addr.type);
        memcpy(result.addr.value, report->addr.val, sizeof(result.addr.value));
        result.rssi = report->rssi;
        result.connectable = (report->props & BLE_HCI_ADV_CONN_MASK) != 0u;
        result.scan_response = (report->props & BLE_HCI_ADV_SCAN_RSP_MASK) != 0u;
        result.adv_type = (report->props & BLE_HCI_ADV_LEGACY_MASK) != 0u
                              ? H2_PAL_BLE_ADV_TYPE_LEGACY
                              : H2_PAL_BLE_ADV_TYPE_EXTENDED;
        result.primary_phy = h2_esp_ble_phy(report->prim_phy);
        result.secondary_phy = h2_esp_ble_phy(report->sec_phy);
        result.sid = report->sid;
        result.data_status = report->data_status == BLE_GAP_EXT_ADV_DATA_STATUS_INCOMPLETE
                                 ? H2_PAL_BLE_ADV_DATA_INCOMPLETE
                             : report->data_status == BLE_GAP_EXT_ADV_DATA_STATUS_TRUNCATED
                                 ? H2_PAL_BLE_ADV_DATA_TRUNCATED
                                 : H2_PAL_BLE_ADV_DATA_COMPLETE;
        result.tx_power = report->tx_power;
        result.raw_data.data = report->data;
        result.raw_data.len = report->length_data;
        h2_esp_ble_apply_adv_fields(&result, &fields);
        if (s_h2_esp_ble_scan_cb != NULL &&
            s_h2_esp_ble_scan_cb(s_h2_esp_ble_scan_user, &result)) {
            (void)ble_gap_disc_cancel();
        }
        return 0;
    }
#endif
    case BLE_GAP_EVENT_DISC_COMPLETE:
        h2_esp_ble_complete_scan();
        return 0;
    case BLE_GAP_EVENT_ADV_COMPLETE:
#if CONFIG_BT_NIMBLE_EXT_ADV
        ESP_LOGI(
            TAG,
            "extended advertising complete instance=%u reason=%d events=%u",
            (unsigned)event->adv_complete.instance,
            event->adv_complete.reason,
            (unsigned)event->adv_complete.num_ext_adv_events);
        if (arg != NULL) {
            h2_esp_ble_complete_adv_set(arg, H2_PAL_OK);
            return 0;
        }
#endif
        h2_esp_ble_complete_advertising();
        return 0;
    case BLE_GAP_EVENT_CONNECT: {
        bool central = s_h2_esp_ble_connect_pending;
        ESP_LOGI(
            TAG,
            "connection event status=%d role=%s handle=%u",
            event->connect.status,
            central ? "central" : "peripheral",
            (unsigned)event->connect.conn_handle);
        if (event->connect.status == 0) {
            s_h2_esp_ble_last_conn_handle = event->connect.conn_handle;
            int data_len_rc = ble_gap_set_data_len(
                event->connect.conn_handle,
                H2_ESP_BLE_LL_DATA_LEN,
                H2_ESP_BLE_LL_DATA_TIME_US);
            if (data_len_rc != 0) {
                ESP_LOGW(TAG, "set data len failed rc=%d", data_len_rc);
            }
            h2_pal_ble_connection_t conn;
            memset(&conn, 0, sizeof(conn));
            conn.conn_handle = event->connect.conn_handle;
            conn.role = central ? H2_PAL_BLE_ROLE_CENTRAL : H2_PAL_BLE_ROLE_PERIPHERAL;
            conn.mtu = ble_att_mtu(event->connect.conn_handle);
            s_h2_esp_ble_connect_result = H2_PAL_OK;
            s_h2_esp_ble_connect_handle = event->connect.conn_handle;
            h2_esp_ble_post(
                H2_PAL_SYSTEM_EVENT_TYPE_BLE_CONNECTED,
                &conn,
                sizeof(conn));
        } else {
            s_h2_esp_ble_connect_result =
                h2_esp_ble_map_rc(event->connect.status);
            s_h2_esp_ble_connect_handle = H2_PAL_BLE_INVALID_CONN_HANDLE;
        }
        if (!central) {
#if CONFIG_BT_NIMBLE_EXT_ADV
            if (arg != NULL) {
                h2_esp_ble_complete_adv_set(arg, H2_PAL_OK);
            } else
#endif
            {
                h2_esp_ble_complete_advertising();
            }
        }
        s_h2_esp_ble_connect_pending = false;
        if (s_h2_esp_ble_events != NULL) {
            xEventGroupSetBits(s_h2_esp_ble_events, H2_ESP_BLE_CONNECT_DONE_BIT);
        }
        return 0;
    }
    case BLE_GAP_EVENT_CONN_UPDATE: {
        struct ble_gap_conn_desc desc;
        int desc_rc = ble_gap_conn_find(event->conn_update.conn_handle, &desc);
        if (desc_rc == 0) {
            const h2_pal_ble_connection_params_t params = {
                .interval_min_ms =
                    (uint16_t)((desc.conn_itvl * 125u) / 100u),
                .interval_max_ms =
                    (uint16_t)((desc.conn_itvl * 125u) / 100u),
                .latency = desc.conn_latency,
                .supervision_timeout_ms =
                    (uint16_t)(desc.supervision_timeout * 10u),
            };
            ESP_LOGI(
                TAG,
                "conn update status=%d handle=%u interval_units=%u interval_ms=%u latency=%u timeout_units=%u",
                event->conn_update.status,
                event->conn_update.conn_handle,
                desc.conn_itvl,
                (unsigned)((desc.conn_itvl * 125u) / 100u),
                desc.conn_latency,
                desc.supervision_timeout);
            h2_esp_ble_post(
                H2_PAL_SYSTEM_EVENT_TYPE_BLE_CONNECTION_UPDATED,
                &params, sizeof(params));
        } else {
            ESP_LOGW(
                TAG,
                "conn update status=%d handle=%u desc_rc=%d",
                event->conn_update.status,
                event->conn_update.conn_handle,
                desc_rc);
        }
        return 0;
    }
    case BLE_GAP_EVENT_DATA_LEN_CHG:
        ESP_LOGI(
            TAG,
            "data len handle=%u tx_octets=%u tx_time=%u rx_octets=%u rx_time=%u",
            event->data_len_chg.conn_handle,
            event->data_len_chg.max_tx_octets,
            event->data_len_chg.max_tx_time,
            event->data_len_chg.max_rx_octets,
            event->data_len_chg.max_rx_time);
        return 0;
    case BLE_GAP_EVENT_PASSKEY_ACTION: {
        uint8_t expected_action =
            s_h2_esp_ble_pairing.io == H2_PAL_BLE_PAIRING_IO_DISPLAY_ONLY
                ? BLE_SM_IOACT_DISP
                : BLE_SM_IOACT_INPUT;
        if (!s_h2_esp_ble_pairing.enabled ||
            event->passkey.params.action != expected_action) {
            return BLE_HS_EAUTHEN;
        }
        struct ble_sm_io io;
        memset(&io, 0, sizeof(io));
        io.action = event->passkey.params.action;
        io.passkey = s_h2_esp_ble_pairing.passkey;
        return ble_sm_inject_io(event->passkey.conn_handle, &io);
    }
    case BLE_GAP_EVENT_ENC_CHANGE: {
        if (event->enc_change.conn_handle !=
            s_h2_esp_ble_pair_conn_handle) {
            return 0;
        }
        struct ble_gap_conn_desc desc;
        int desc_rc =
            ble_gap_conn_find(event->enc_change.conn_handle, &desc);
        s_h2_esp_ble_pair_result =
            event->enc_change.status == 0 && desc_rc == 0 &&
                    desc.sec_state.encrypted &&
                    desc.sec_state.authenticated
                ? H2_PAL_OK
                : H2_PAL_ERR_IO;
        if (s_h2_esp_ble_events != NULL) {
            xEventGroupSetBits(s_h2_esp_ble_events,
                               H2_ESP_BLE_PAIR_DONE_BIT);
        }
        return 0;
    }
    case BLE_GAP_EVENT_PHY_UPDATE_COMPLETE:
        s_h2_esp_ble_phy_info.conn_handle = event->phy_updated.conn_handle;
        s_h2_esp_ble_phy_info.tx_phy = h2_esp_ble_phy(event->phy_updated.tx_phy);
        s_h2_esp_ble_phy_info.rx_phy = h2_esp_ble_phy(event->phy_updated.rx_phy);
        s_h2_esp_ble_phy_result = event->phy_updated.status == 0 ? H2_PAL_OK : H2_PAL_ERR_IO;
        ESP_LOGI(
            TAG,
            "phy update status=%d handle=%u tx_phy=%u rx_phy=%u",
            event->phy_updated.status,
            event->phy_updated.conn_handle,
            (unsigned)event->phy_updated.tx_phy,
            (unsigned)event->phy_updated.rx_phy);
        if (s_h2_esp_ble_events != NULL) {
            xEventGroupSetBits(s_h2_esp_ble_events, H2_ESP_BLE_PHY_DONE_BIT);
        }
        return 0;
    case BLE_GAP_EVENT_DISCONNECT: {
        h2_pal_ble_disconnected_info_t info;
        memset(&info, 0, sizeof(info));
        info.conn_handle = event->disconnect.conn.conn_handle;
        info.peer_addr.type = h2_esp_ble_addr_type(event->disconnect.conn.peer_ota_addr.type);
        memcpy(info.peer_addr.value, event->disconnect.conn.peer_ota_addr.val, sizeof(info.peer_addr.value));
        info.reason = event->disconnect.reason;
        h2_esp_ble_finish_indication(
            event->disconnect.conn.conn_handle,
            H2_PAL_BLE_INVALID_ATTR_HANDLE,
            false,
            H2_PAL_ERR_CLOSED);
        s_h2_esp_ble_last_conn_handle = H2_PAL_BLE_INVALID_CONN_HANDLE;
        h2_esp_ble_post(H2_PAL_SYSTEM_EVENT_TYPE_BLE_DISCONNECTED, &info, sizeof(info));
        return 0;
    }
    case BLE_GAP_EVENT_MTU: {
        h2_pal_ble_mtu_info_t info = {
            .conn_handle = event->mtu.conn_handle,
            .mtu = event->mtu.value,
        };
        h2_esp_ble_post(H2_PAL_SYSTEM_EVENT_TYPE_BLE_MTU_CHANGED, &info, sizeof(info));
        return 0;
    }
    case BLE_GAP_EVENT_SUBSCRIBE: {
        h2_pal_ble_subscription_state_t state = {
            .conn_handle = event->subscribe.conn_handle,
            .value_handle = event->subscribe.attr_handle,
            .mode = event->subscribe.cur_notify ? H2_PAL_BLE_SUBSCRIBE_MODE_NOTIFY
                                                : H2_PAL_BLE_SUBSCRIBE_MODE_INDICATE,
            .enabled = event->subscribe.cur_notify || event->subscribe.cur_indicate,
        };
        h2_esp_ble_post(H2_PAL_SYSTEM_EVENT_TYPE_BLE_SUBSCRIPTION_CHANGED, &state, sizeof(state));
        return 0;
    }
    case BLE_GAP_EVENT_NOTIFY_RX: {
        h2_pal_ble_gatt_client_value_t client_value;
        memset(&client_value, 0, sizeof(client_value));
        client_value.conn_handle = event->notify_rx.conn_handle;
        client_value.attr_handle = event->notify_rx.attr_handle;
        uint16_t len = 0u;
        (void)ble_hs_mbuf_to_flat(
            event->notify_rx.om, client_value.value,
            sizeof(client_value.value), &len);
        client_value.value_len = len;
        h2_esp_ble_post(
            event->notify_rx.indication ? H2_PAL_SYSTEM_EVENT_TYPE_BLE_GATT_CLIENT_INDICATION
                                        : H2_PAL_SYSTEM_EVENT_TYPE_BLE_GATT_CLIENT_NOTIFICATION,
            &client_value,
            sizeof(client_value));
        return 0;
    }
    case BLE_GAP_EVENT_NOTIFY_TX:
        if (event->notify_tx.indication && event->notify_tx.status != 0) {
            h2_esp_ble_finish_indication(
                event->notify_tx.conn_handle,
                event->notify_tx.attr_handle,
                true,
                event->notify_tx.status == BLE_HS_EDONE
                    ? H2_PAL_OK
                    : h2_esp_ble_map_rc(event->notify_tx.status));
        }
        return 0;
    default:
        return 0;
    }
}

static int h2_esp_ble_discover_service_cb(
    uint16_t conn_handle,
    const struct ble_gatt_error *error,
    const struct ble_gatt_svc *service,
    void *arg) {
    (void)conn_handle;
    (void)arg;
    if (error == NULL) {
        s_h2_esp_ble_gatt_result = H2_PAL_ERR_IO;
        xEventGroupSetBits(s_h2_esp_ble_events, H2_ESP_BLE_GATT_DONE_BIT);
        return 0;
    }
    if (error->status == BLE_HS_EDONE) {
        s_h2_esp_ble_gatt_result = H2_PAL_OK;
        xEventGroupSetBits(s_h2_esp_ble_events, H2_ESP_BLE_GATT_DONE_BIT);
        return 0;
    }
    if (error->status != 0) {
        s_h2_esp_ble_gatt_result = h2_esp_ble_map_rc(error->status);
        xEventGroupSetBits(s_h2_esp_ble_events, H2_ESP_BLE_GATT_DONE_BIT);
        return 0;
    }
    if (service != NULL && h2_esp_ble_discovery_uuid_matches(&service->uuid) &&
        s_h2_esp_ble_discovery_count < s_h2_esp_ble_discovery_max_entries) {
        size_t idx = s_h2_esp_ble_discovery_count++;
        h2_pal_ble_gatt_discovery_entry_t *entry = &s_h2_esp_ble_discovery_entries[idx];
        memset(entry, 0, sizeof(*entry));
        entry->kind = H2_PAL_BLE_GATT_DISCOVERY_SERVICE;
        entry->start_handle = service->start_handle;
        entry->end_handle = service->end_handle;
        h2_esp_ble_copy_uuid(&service->uuid, &entry->uuid, s_h2_esp_ble_discovery_uuid_data[idx]);
    }
    return 0;
}

static uint16_t h2_esp_ble_gatt_properties_from_nimble(uint8_t properties) {
    uint16_t result = 0u;
    if ((properties & BLE_GATT_CHR_PROP_READ) != 0u) {
        result |= H2_PAL_BLE_GATT_PROPERTY_READ;
    }
    if ((properties & BLE_GATT_CHR_PROP_WRITE) != 0u) {
        result |= H2_PAL_BLE_GATT_PROPERTY_WRITE;
    }
    if ((properties & BLE_GATT_CHR_PROP_WRITE_NO_RSP) != 0u) {
        result |= H2_PAL_BLE_GATT_PROPERTY_WRITE_NO_RSP;
    }
    if ((properties & BLE_GATT_CHR_PROP_NOTIFY) != 0u) {
        result |= H2_PAL_BLE_GATT_PROPERTY_NOTIFY;
    }
    if ((properties & BLE_GATT_CHR_PROP_INDICATE) != 0u) {
        result |= H2_PAL_BLE_GATT_PROPERTY_INDICATE;
    }
    return result;
}

static int h2_esp_ble_discover_characteristic_cb(
    uint16_t conn_handle,
    const struct ble_gatt_error *error,
    const struct ble_gatt_chr *chr,
    void *arg) {
    (void)conn_handle;
    (void)arg;
    if (error == NULL) {
        s_h2_esp_ble_gatt_result = H2_PAL_ERR_IO;
        xEventGroupSetBits(s_h2_esp_ble_events, H2_ESP_BLE_GATT_DONE_BIT);
        return 0;
    }
    if (error->status == BLE_HS_EDONE) {
        s_h2_esp_ble_gatt_result = H2_PAL_OK;
        xEventGroupSetBits(s_h2_esp_ble_events, H2_ESP_BLE_GATT_DONE_BIT);
        return 0;
    }
    if (error->status != 0) {
        s_h2_esp_ble_gatt_result = h2_esp_ble_map_rc(error->status);
        xEventGroupSetBits(s_h2_esp_ble_events, H2_ESP_BLE_GATT_DONE_BIT);
        return 0;
    }
    if (chr != NULL && h2_esp_ble_discovery_uuid_matches(&chr->uuid) &&
        s_h2_esp_ble_discovery_count < s_h2_esp_ble_discovery_max_entries) {
        size_t idx = s_h2_esp_ble_discovery_count++;
        h2_pal_ble_gatt_discovery_entry_t *entry = &s_h2_esp_ble_discovery_entries[idx];
        memset(entry, 0, sizeof(*entry));
        entry->kind = H2_PAL_BLE_GATT_DISCOVERY_CHARACTERISTIC;
        entry->start_handle = chr->def_handle;
        entry->value_handle = chr->val_handle;
        entry->properties =
            h2_esp_ble_gatt_properties_from_nimble(chr->properties);
        h2_esp_ble_copy_uuid(&chr->uuid, &entry->uuid, s_h2_esp_ble_discovery_uuid_data[idx]);
    }
    return 0;
}

static int h2_esp_ble_discover_descriptor_cb(
    uint16_t conn_handle,
    const struct ble_gatt_error *error,
    uint16_t chr_val_handle,
    const struct ble_gatt_dsc *descriptor,
    void *arg) {
    (void)conn_handle;
    (void)arg;
    if (error == NULL) {
        s_h2_esp_ble_gatt_result = H2_PAL_ERR_IO;
        xEventGroupSetBits(s_h2_esp_ble_events, H2_ESP_BLE_GATT_DONE_BIT);
        return 0;
    }
    if (error->status == BLE_HS_EDONE) {
        s_h2_esp_ble_gatt_result = H2_PAL_OK;
        xEventGroupSetBits(s_h2_esp_ble_events, H2_ESP_BLE_GATT_DONE_BIT);
        return 0;
    }
    if (error->status != 0) {
        s_h2_esp_ble_gatt_result = h2_esp_ble_map_rc(error->status);
        xEventGroupSetBits(s_h2_esp_ble_events, H2_ESP_BLE_GATT_DONE_BIT);
        return 0;
    }
    if (descriptor != NULL &&
        h2_esp_ble_discovery_uuid_matches(&descriptor->uuid) &&
        s_h2_esp_ble_discovery_count < s_h2_esp_ble_discovery_max_entries) {
        size_t idx = s_h2_esp_ble_discovery_count++;
        h2_pal_ble_gatt_discovery_entry_t *entry =
            &s_h2_esp_ble_discovery_entries[idx];
        memset(entry, 0, sizeof(*entry));
        entry->kind = H2_PAL_BLE_GATT_DISCOVERY_DESCRIPTOR;
        entry->start_handle = chr_val_handle;
        entry->value_handle = descriptor->handle;
        h2_esp_ble_copy_uuid(
            &descriptor->uuid, &entry->uuid,
            s_h2_esp_ble_discovery_uuid_data[idx]);
    }
    return 0;
}

static int h2_esp_ble_gatt_attr_cb(
    uint16_t conn_handle,
    const struct ble_gatt_error *error,
    struct ble_gatt_attr *attr,
    void *arg) {
    (void)conn_handle;
    (void)arg;
    if (error == NULL) {
        s_h2_esp_ble_gatt_result = H2_PAL_ERR_IO;
    } else if (error->status == BLE_HS_EDONE || error->status == 0) {
        s_h2_esp_ble_gatt_result = H2_PAL_OK;
        if (attr != NULL && attr->om != NULL && s_h2_esp_ble_read_out != NULL) {
            uint16_t len = 0u;
            int rc = ble_hs_mbuf_to_flat(
                attr->om,
                s_h2_esp_ble_read_out,
                s_h2_esp_ble_read_out_size,
                &len);
            if (rc == 0) {
                s_h2_esp_ble_read_out_len = len;
            } else {
                s_h2_esp_ble_gatt_result = h2_esp_ble_map_rc(rc);
            }
        }
    } else {
        s_h2_esp_ble_gatt_result = h2_esp_ble_map_rc(error->status);
    }
    xEventGroupSetBits(s_h2_esp_ble_events, H2_ESP_BLE_GATT_DONE_BIT);
    return 0;
}

static int h2_esp_ble_mtu_cb(
    uint16_t conn_handle,
    const struct ble_gatt_error *error,
    uint16_t mtu,
    void *arg) {
    (void)conn_handle;
    (void)arg;
    if (error == NULL) {
        s_h2_esp_ble_gatt_result = H2_PAL_ERR_IO;
    } else if (error->status == 0) {
        s_h2_esp_ble_exchange_mtu = mtu;
        s_h2_esp_ble_gatt_result = H2_PAL_OK;
    } else {
        s_h2_esp_ble_gatt_result = h2_esp_ble_map_rc(error->status);
    }
    xEventGroupSetBits(s_h2_esp_ble_events, H2_ESP_BLE_GATT_DONE_BIT);
    return 0;
}

static int h2_esp_ble_gatt_access_unlocked(
    uint16_t conn_handle,
    uint16_t attr_handle,
    struct ble_gatt_access_ctxt *ctxt,
    void *arg) {
    if (!s_h2_esp_ble_gatt_attached || arg == NULL) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    size_t index = *(const uint8_t *)arg;
    if (index >= s_h2_esp_ble_characteristic_count) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    h2_pal_ble_gatt_access_t access = {
        .conn_handle = conn_handle,
        .attr_handle = attr_handle,
        .offset = 0u,
    };

    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        uint8_t out[H2_ESP_BLE_MAX_VALUE_LEN];
        size_t out_len = 0u;
        h2_pal_result_t rc = H2_PAL_OK;
        if (s_h2_esp_ble_read[index] != NULL) {
            rc = s_h2_esp_ble_read[index](s_h2_esp_ble_gatt_user[index], &access, out, sizeof(out), &out_len);
        } else {
            out_len = s_h2_esp_ble_value_len[index];
            memcpy(out, s_h2_esp_ble_value[index], out_len);
        }
        if (rc != H2_PAL_OK) {
            return BLE_ATT_ERR_UNLIKELY;
        }
        return os_mbuf_append(ctxt->om, out, out_len) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }

    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        uint16_t len = 0u;
        int rc = ble_hs_mbuf_to_flat(
            ctxt->om, s_h2_esp_ble_value[index],
            s_h2_esp_ble_value_max_len[index], &len);
        if (rc != 0) {
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }
        s_h2_esp_ble_value_len[index] = len;
        if (s_h2_esp_ble_write[index] != NULL) {
            h2_pal_result_t write_rc =
                s_h2_esp_ble_write[index](
                    s_h2_esp_ble_gatt_user[index], &access,
                    s_h2_esp_ble_value[index], s_h2_esp_ble_value_len[index]);
            if (write_rc != H2_PAL_OK) {
                return BLE_ATT_ERR_UNLIKELY;
            }
        }
        return 0;
    }

    return BLE_ATT_ERR_UNLIKELY;
}

static h2_pal_result_t h2_esp_ble_start(h2_pal_ble_t *ble) {
    (void)ble;
    if (s_h2_esp_ble_started) {
        return H2_PAL_OK;
    }
#if CONFIG_BT_NIMBLE_EXT_ADV
    h2_pal_result_t adv_mutex_rc = h2_esp_ble_ensure_adv_mutex();
    if (adv_mutex_rc != H2_PAL_OK) {
        return adv_mutex_rc;
    }
#endif
    esp_log_level_set("NimBLE", ESP_LOG_WARN);
    if (s_h2_esp_ble_events == NULL) {
        s_h2_esp_ble_events = xEventGroupCreate();
        if (s_h2_esp_ble_events == NULL) {
            return H2_PAL_ERR_NO_MEMORY;
        }
    }
    xEventGroupClearBits(s_h2_esp_ble_events, H2_ESP_BLE_SYNCED_BIT);

#if CONFIG_ESP_HOSTED_ENABLE_BT_NIMBLE
    esp_err_t hosted_err = esp_hosted_connect_to_slave();
    if (hosted_err != ESP_OK) {
        ESP_LOGW(TAG, "ESP-Hosted slave connect failed err=%d", (int)hosted_err);
        return H2_PAL_ERR_IO;
    }
    if (!s_h2_esp_ble_hosted_controller_initialized) {
        hosted_err = esp_hosted_bt_controller_init();
        if (hosted_err != ESP_OK) {
            ESP_LOGW(TAG, "ESP-Hosted BT controller init failed err=%d", (int)hosted_err);
            return H2_PAL_ERR_IO;
        }
        s_h2_esp_ble_hosted_controller_initialized = true;
    }
    if (!s_h2_esp_ble_hosted_controller_enabled) {
        hosted_err = esp_hosted_bt_controller_enable();
        if (hosted_err != ESP_OK) {
            ESP_LOGW(TAG, "ESP-Hosted BT controller enable failed err=%d", (int)hosted_err);
            return H2_PAL_ERR_IO;
        }
        s_h2_esp_ble_hosted_controller_enabled = true;
    }
#endif

    h2_esp_ble_init_call_t init_call = { .result = ESP_FAIL };
    h2_pal_result_t safe_rc = h2_esp_platform_safe_call(
        h2_esp_ble_init_safe_callback,
        &init_call,
        sizeof(init_call),
        H2_ESP_BLE_INIT_SAFE_STACK_DEPTH);
    if (safe_rc != H2_PAL_OK) {
        ESP_LOGE(TAG, "nimble_port_init safe call failed rc=%d", safe_rc);
        return safe_rc;
    }
    esp_err_t err = init_call.result;
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed err=%d", (int)err);
        return H2_PAL_ERR_IO;
    }

    ble_hs_cfg.reset_cb = h2_esp_ble_on_reset;
    ble_hs_cfg.sync_cb = h2_esp_ble_on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    int rc = ble_svc_gap_device_name_set("h2-esp-ble");
    if (rc != 0) {
        return h2_esp_ble_map_rc(rc);
    }
    if (s_h2_esp_ble_gatt_configured) {
        rc = ble_gatts_count_cfg(s_h2_esp_ble_svc_defs);
        if (rc == 0) {
            rc = ble_gatts_add_svcs(s_h2_esp_ble_svc_defs);
        }
        if (rc != 0) {
            return h2_esp_ble_map_rc(rc);
        }
    }
    ble_store_config_init();
    nimble_port_freertos_init(h2_esp_ble_host_task);

    EventBits_t bits = xEventGroupWaitBits(
        s_h2_esp_ble_events,
        H2_ESP_BLE_SYNCED_BIT,
        pdFALSE,
        pdTRUE,
        pdMS_TO_TICKS(3000));
    if ((bits & H2_ESP_BLE_SYNCED_BIT) == 0u) {
        return H2_PAL_ERR_TIMEOUT;
    }
    h2_esp_ble_update_out_handles();
    h2_esp_ble_clear_out_handle_ptrs();
    s_h2_esp_ble_started = true;
    return H2_PAL_OK;
}

static h2_pal_result_t h2_esp_ble_stop(h2_pal_ble_t *ble) {
    (void)ble;
    if (!s_h2_esp_ble_started) {
        return H2_PAL_OK;
    }
    uint16_t indication_conn_handle = H2_PAL_BLE_INVALID_CONN_HANDLE;
    taskENTER_CRITICAL(&s_h2_esp_ble_indication_lock);
    indication_conn_handle = s_h2_esp_ble_indication.conn_handle;
    taskEXIT_CRITICAL(&s_h2_esp_ble_indication_lock);
    if (indication_conn_handle != H2_PAL_BLE_INVALID_CONN_HANDLE) {
        h2_esp_ble_finish_indication(
            indication_conn_handle,
            H2_PAL_BLE_INVALID_ATTR_HANDLE,
            false,
            H2_PAL_ERR_CLOSED);
    }
    (void)ble_gap_disc_cancel();
#if CONFIG_BT_NIMBLE_EXT_ADV
    h2_pal_result_t adv_lock_rc = h2_esp_ble_lock_adv();
    if (adv_lock_rc != H2_PAL_OK) {
        return adv_lock_rc;
    }
    for (size_t i = 1u; i < CONFIG_BT_NIMBLE_MAX_EXT_ADV_INSTANCES; ++i) {
        h2_pal_ble_adv_set_t *set = &s_h2_esp_ble_adv_sets[i];
        if (!set->allocated) {
            continue;
        }
        if (set->active) {
            (void)ble_gap_ext_adv_stop(set->instance);
            h2_esp_ble_complete_adv_set_unlocked(set, H2_PAL_OK);
        }
        (void)ble_gap_ext_adv_remove(set->instance);
        memset(set, 0, sizeof(*set));
    }
    h2_esp_ble_unlock_adv();
#endif
    if (s_h2_esp_ble_adv_active) {
#if CONFIG_BT_NIMBLE_EXT_ADV
        if (s_h2_esp_ble_ext_adv_configured) {
            (void)ble_gap_ext_adv_stop(H2_ESP_BLE_EXT_ADV_INSTANCE);
            h2_esp_ble_complete_advertising();
        } else
#endif
        {
            (void)ble_gap_adv_stop();
        }
    }
#if CONFIG_BT_NIMBLE_EXT_ADV
    if (s_h2_esp_ble_ext_adv_configured) {
        (void)ble_gap_ext_adv_remove(H2_ESP_BLE_EXT_ADV_INSTANCE);
    }
#endif
    int rc = nimble_port_stop();
    if (rc != 0) {
        return h2_esp_ble_map_rc(rc);
    }
    esp_err_t err = nimble_port_deinit();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_deinit failed err=%d", (int)err);
        return H2_PAL_ERR_IO;
    }
#if CONFIG_ESP_HOSTED_ENABLE_BT_NIMBLE
    h2_pal_result_t hosted_stop_result = H2_PAL_OK;
    if (s_h2_esp_ble_hosted_controller_enabled) {
        err = esp_hosted_bt_controller_disable();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "ESP-Hosted BT controller disable failed err=%d", (int)err);
            hosted_stop_result = H2_PAL_ERR_IO;
        } else {
            s_h2_esp_ble_hosted_controller_enabled = false;
        }
    }
    if (s_h2_esp_ble_hosted_controller_initialized &&
        !s_h2_esp_ble_hosted_controller_enabled) {
        err = esp_hosted_bt_controller_deinit(false);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "ESP-Hosted BT controller deinit failed err=%d", (int)err);
            hosted_stop_result = H2_PAL_ERR_IO;
        } else {
            s_h2_esp_ble_hosted_controller_initialized = false;
        }
    }
#endif
    s_h2_esp_ble_scan_cb = NULL;
    s_h2_esp_ble_scan_user = NULL;
    s_h2_esp_ble_connect_pending = false;
    s_h2_esp_ble_connect_handle = H2_PAL_BLE_INVALID_CONN_HANDLE;
    s_h2_esp_ble_last_conn_handle = H2_PAL_BLE_INVALID_CONN_HANDLE;
    s_h2_esp_ble_adv_data_len = 0u;
    s_h2_esp_ble_legacy_adv_data_len = 0u;
    s_h2_esp_ble_legacy_scan_response_len = 0u;
    s_h2_esp_ble_legacy_adv_data_valid = false;
    s_h2_esp_ble_adv_data_staged = false;
    s_h2_esp_ble_adv_active = false;
    s_h2_esp_ble_ext_adv_configured = false;
    s_h2_esp_ble_started = false;
    h2_esp_ble_post(H2_PAL_SYSTEM_EVENT_TYPE_BLE_HOST_STOPPED, NULL, 0u);
#if CONFIG_ESP_HOSTED_ENABLE_BT_NIMBLE
    return hosted_stop_result;
#else
    return H2_PAL_OK;
#endif
}

static h2_pal_result_t h2_esp_ble_set_adv_data(
    h2_pal_ble_t *ble,
    const h2_pal_ble_adv_data_t *data) {
    (void)ble;
    if (!s_h2_esp_ble_started || data == NULL) {
        return data == NULL ? H2_PAL_ERR_INVALID_ARG : H2_PAL_ERR_INVALID_STATE;
    }

    uint8_t encoded[H2_PAL_BLE_EXT_ADV_DATA_MAX_LEN];
    size_t encoded_len = 0u;
    h2_pal_result_t rc = h2_esp_ble_encode_adv_data(data, encoded, &encoded_len);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    memcpy(s_h2_esp_ble_adv_data, encoded, encoded_len);
    s_h2_esp_ble_adv_data_len = encoded_len;
    h2_pal_ble_adv_data_t legacy_primary = *data;
    legacy_primary.local_name = NULL;
    legacy_primary.manufacturer_data = (h2_pal_ble_bytes_t){ 0 };
    uint8_t legacy_primary_encoded[H2_PAL_BLE_EXT_ADV_DATA_MAX_LEN];
    size_t legacy_primary_len = 0u;
    rc = h2_esp_ble_encode_adv_data(
        &legacy_primary,
        legacy_primary_encoded,
        &legacy_primary_len);
    size_t legacy_scan_response_len = 0u;
    bool legacy_valid = rc == H2_PAL_OK &&
        legacy_primary_len <= H2_PAL_BLE_LEGACY_ADV_DATA_MAX_LEN;
    if (legacy_valid) {
        memcpy(s_h2_esp_ble_legacy_adv_data,
            legacy_primary_encoded, legacy_primary_len);
    }
    if (legacy_valid && data->manufacturer_data.len > 0u) {
        legacy_valid = h2_esp_ble_adv_put(
            s_h2_esp_ble_legacy_scan_response,
            &legacy_scan_response_len,
            H2_ESP_BLE_AD_TYPE_MANUFACTURER,
            data->manufacturer_data.data,
            data->manufacturer_data.len);
    }
    if (legacy_valid && data->local_name != NULL) {
        legacy_valid = h2_esp_ble_adv_put(
            s_h2_esp_ble_legacy_scan_response,
            &legacy_scan_response_len,
            H2_ESP_BLE_AD_TYPE_NAME_COMPLETE,
            (const uint8_t *)data->local_name,
            strlen(data->local_name));
    }
    s_h2_esp_ble_legacy_adv_data_len = legacy_primary_len;
    s_h2_esp_ble_legacy_scan_response_len = legacy_scan_response_len;
    s_h2_esp_ble_legacy_adv_data_valid = legacy_valid;
    s_h2_esp_ble_adv_data_staged = true;
    return H2_PAL_OK;
}

static h2_pal_result_t h2_esp_ble_start_advertising(
    h2_pal_ble_t *ble,
    const h2_pal_ble_adv_params_t *params) {
    (void)ble;
    if (!s_h2_esp_ble_started || !s_h2_esp_ble_adv_data_staged || s_h2_esp_ble_adv_active) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    if (params->type == H2_PAL_BLE_ADV_TYPE_LEGACY &&
        !s_h2_esp_ble_legacy_adv_data_valid) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (s_h2_esp_ble_events != NULL) {
        xEventGroupClearBits(s_h2_esp_ble_events, H2_ESP_BLE_ADV_STOPPED_BIT);
    }
    h2_pal_result_t rc;

#if CONFIG_BT_NIMBLE_EXT_ADV
    if (s_h2_esp_ble_ext_adv_configured) {
        int remove_rc = ble_gap_ext_adv_remove(H2_ESP_BLE_EXT_ADV_INSTANCE);
        if (remove_rc != 0) {
            return h2_esp_ble_map_rc(remove_rc);
        }
        s_h2_esp_ble_ext_adv_configured = false;
    }
#endif

#if CONFIG_BT_NIMBLE_EXT_ADV
    /*
     * NimBLE compiles ble_gap_adv_* out when BLE_EXT_ADV is enabled. Legacy
     * advertising must therefore use an extended advertising set with the
     * legacy-PDU property in this configuration.
     */
    bool legacy = params->type == H2_PAL_BLE_ADV_TYPE_LEGACY;
    struct ble_gap_ext_adv_params ext_params;
    h2_esp_ble_fill_ext_adv_params(params, &ext_params);
    int ext_rc = ble_gap_ext_adv_configure(
        H2_ESP_BLE_EXT_ADV_INSTANCE, &ext_params, NULL, h2_esp_ble_gap_event, NULL);
    if (ext_rc != 0) {
        return h2_esp_ble_map_rc(ext_rc);
    }
    s_h2_esp_ble_ext_adv_configured = true;
    struct os_mbuf *mbuf = os_msys_get_pkthdr(0u, 0u);
    if (mbuf == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    const uint8_t *advertising_data = legacy
        ? s_h2_esp_ble_legacy_adv_data
        : s_h2_esp_ble_adv_data;
    size_t advertising_data_len = legacy
        ? s_h2_esp_ble_legacy_adv_data_len
        : s_h2_esp_ble_adv_data_len;
    ext_rc = os_mbuf_append(mbuf, advertising_data, advertising_data_len);
    if (ext_rc != 0) {
        os_mbuf_free_chain(mbuf);
        return h2_esp_ble_map_rc(ext_rc);
    }
    ext_rc = ble_gap_ext_adv_set_data(H2_ESP_BLE_EXT_ADV_INSTANCE, mbuf);
    if (ext_rc != 0) {
        return h2_esp_ble_map_rc(ext_rc);
    }
    if (legacy) {
        mbuf = os_msys_get_pkthdr(0u, 0u);
        if (mbuf == NULL) {
            return H2_PAL_ERR_NO_MEMORY;
        }
        if (s_h2_esp_ble_legacy_scan_response_len > 0u) {
            ext_rc = os_mbuf_append(
                mbuf,
                s_h2_esp_ble_legacy_scan_response,
                s_h2_esp_ble_legacy_scan_response_len);
            if (ext_rc != 0) {
                os_mbuf_free_chain(mbuf);
                return h2_esp_ble_map_rc(ext_rc);
            }
        }
        ext_rc = ble_gap_ext_adv_rsp_set_data(
            H2_ESP_BLE_EXT_ADV_INSTANCE, mbuf);
        if (ext_rc != 0) {
            return h2_esp_ble_map_rc(ext_rc);
        }
    }
    uint32_t duration_units = legacy ? 0u : (params->duration_ms + 9u) / 10u;
    uint8_t max_adv_events = legacy ? 0u : params->max_adv_events;
    rc = h2_esp_ble_map_rc(
        ble_gap_ext_adv_start(H2_ESP_BLE_EXT_ADV_INSTANCE, (int)duration_units, max_adv_events));
#else
    if (params->type == H2_PAL_BLE_ADV_TYPE_EXTENDED) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    {
        int data_rc = ble_gap_adv_set_data(
            s_h2_esp_ble_legacy_adv_data,
            s_h2_esp_ble_legacy_adv_data_len);
        if (data_rc != 0) {
            return h2_esp_ble_map_rc(data_rc);
        }
        data_rc = ble_gap_adv_rsp_set_data(
            s_h2_esp_ble_legacy_scan_response,
            s_h2_esp_ble_legacy_scan_response_len);
        if (data_rc != 0) {
            return h2_esp_ble_map_rc(data_rc);
        }
        struct ble_gap_adv_params adv_params;
        memset(&adv_params, 0, sizeof(adv_params));
        adv_params.conn_mode = params->mode == H2_PAL_BLE_ADV_MODE_CONNECTABLE
                                   ? BLE_GAP_CONN_MODE_UND
                                   : BLE_GAP_CONN_MODE_NON;
        adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
        adv_params.itvl_min = h2_esp_ble_ms_to_units625(params->interval_min_ms);
        adv_params.itvl_max = h2_esp_ble_ms_to_units625(params->interval_max_ms);

        rc = h2_esp_ble_map_rc(
            ble_gap_adv_start(
                s_h2_esp_ble_own_addr_type,
                NULL,
                BLE_HS_FOREVER,
                &adv_params,
                h2_esp_ble_gap_event,
                NULL));
    }
#endif
    if (rc == H2_PAL_OK) {
        s_h2_esp_ble_adv_active = true;
        h2_esp_ble_post(H2_PAL_SYSTEM_EVENT_TYPE_BLE_ADVERTISING_STARTED, NULL, 0u);
    }
    return rc;
}

static h2_pal_result_t h2_esp_ble_stop_advertising(h2_pal_ble_t *ble) {
    (void)ble;
    if (!s_h2_esp_ble_adv_active) {
        return H2_PAL_OK;
    }
    int rc;
#if CONFIG_BT_NIMBLE_EXT_ADV
    if (s_h2_esp_ble_ext_adv_configured) {
        rc = ble_gap_ext_adv_stop(H2_ESP_BLE_EXT_ADV_INSTANCE);
        if (rc == 0 || rc == BLE_HS_EALREADY) {
            h2_esp_ble_complete_advertising();
            return H2_PAL_OK;
        }
    } else
#endif
    {
        rc = ble_gap_adv_stop();
        if (rc == BLE_HS_EALREADY) {
            h2_esp_ble_complete_advertising();
            return H2_PAL_OK;
        }
    }
    if (rc == 0 && s_h2_esp_ble_events != NULL) {
        EventBits_t bits = xEventGroupWaitBits(
            s_h2_esp_ble_events,
            H2_ESP_BLE_ADV_STOPPED_BIT,
            pdTRUE,
            pdTRUE,
            pdMS_TO_TICKS(H2_ESP_BLE_GAP_STOP_TIMEOUT_MS));
        if ((bits & H2_ESP_BLE_ADV_STOPPED_BIT) == 0u && s_h2_esp_ble_adv_active) {
            return H2_PAL_ERR_TIMEOUT;
        }
    }
    if (rc == 0) {
        return H2_PAL_OK;
    }
    return h2_esp_ble_map_rc(rc);
}

static h2_pal_result_t h2_esp_ble_adv_set_create(
    h2_pal_ble_t *ble,
    const h2_pal_ble_adv_params_t *params,
    h2_pal_ble_adv_set_t **out_set) {
    (void)ble;
    if (params == NULL || out_set == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_set = NULL;
    if (!s_h2_esp_ble_started) {
        return H2_PAL_ERR_INVALID_STATE;
    }
#if !CONFIG_BT_NIMBLE_EXT_ADV
    return H2_PAL_ERR_UNSUPPORTED;
#else
    h2_pal_result_t lock_rc = h2_esp_ble_lock_adv();
    if (lock_rc != H2_PAL_OK) {
        return lock_rc;
    }
    if (!s_h2_esp_ble_started) {
        h2_esp_ble_unlock_adv();
        return H2_PAL_ERR_INVALID_STATE;
    }
    /*
     * A finite-duration/default advertising operation can complete without a
     * subsequent stop call.  Its controller instance is then inactive but
     * still configured.  Reclaim it before allocating an independent set so
     * the permanent H2Loader set and a dynamic product beacon only consume the
     * two controller instances they actually need.
     */
    if (s_h2_esp_ble_ext_adv_configured && !s_h2_esp_ble_adv_active) {
        int remove_rc = ble_gap_ext_adv_remove(H2_ESP_BLE_EXT_ADV_INSTANCE);
        if (remove_rc != 0) {
            h2_esp_ble_unlock_adv();
            return h2_esp_ble_map_rc(remove_rc);
        }
        s_h2_esp_ble_ext_adv_configured = false;
    }
    for (size_t i = 1u; i < CONFIG_BT_NIMBLE_MAX_EXT_ADV_INSTANCES; ++i) {
        h2_pal_ble_adv_set_t *set = &s_h2_esp_ble_adv_sets[i];
        if (set->allocated) {
            continue;
        }
        struct ble_gap_ext_adv_params ext_params;
        h2_esp_ble_fill_ext_adv_params(params, &ext_params);
        memset(set, 0, sizeof(*set));
        set->params = *params;
        set->instance = (uint8_t)i;
        int rc = ble_gap_ext_adv_configure(
            set->instance, &ext_params, NULL, h2_esp_ble_gap_event, set);
        if (rc != 0) {
            memset(set, 0, sizeof(*set));
            h2_esp_ble_unlock_adv();
            return h2_esp_ble_map_rc(rc);
        }
        set->allocated = true;
        *out_set = set;
        h2_esp_ble_unlock_adv();
        return H2_PAL_OK;
    }
    h2_esp_ble_unlock_adv();
    return H2_PAL_ERR_NO_SPACE;
#endif
}

static h2_pal_result_t h2_esp_ble_adv_set_set_data(
    h2_pal_ble_t *ble,
    h2_pal_ble_adv_set_t *set,
    const h2_pal_ble_adv_data_t *data) {
    (void)ble;
#if !CONFIG_BT_NIMBLE_EXT_ADV
    (void)set;
    (void)data;
    return H2_PAL_ERR_UNSUPPORTED;
#else
    if (data == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    h2_pal_result_t lock_rc = h2_esp_ble_lock_adv();
    if (lock_rc != H2_PAL_OK) {
        return lock_rc;
    }
    if (!h2_esp_ble_adv_set_valid_unlocked(set)) {
        h2_esp_ble_unlock_adv();
        return H2_PAL_ERR_INVALID_ARG;
    }
    const bool legacy = set->params.type == H2_PAL_BLE_ADV_TYPE_LEGACY;
    h2_pal_ble_adv_data_t primary_data = *data;
    if (legacy) {
        primary_data.local_name = NULL;
        primary_data.manufacturer_data = (h2_pal_ble_bytes_t){ 0 };
    }
    uint8_t encoded[H2_PAL_BLE_EXT_ADV_DATA_MAX_LEN];
    size_t encoded_len = 0u;
    h2_pal_result_t rc = h2_esp_ble_encode_adv_data(
        &primary_data, encoded, &encoded_len);
    if (rc != H2_PAL_OK) {
        h2_esp_ble_unlock_adv();
        return rc;
    }
    if (legacy &&
        encoded_len > H2_PAL_BLE_LEGACY_ADV_DATA_MAX_LEN) {
        h2_esp_ble_unlock_adv();
        return H2_PAL_ERR_INVALID_ARG;
    }
    uint8_t scan_response[H2_PAL_BLE_LEGACY_ADV_DATA_MAX_LEN];
    size_t scan_response_len = 0u;
    if (legacy && data->manufacturer_data.len > 0u &&
        !h2_esp_ble_adv_put(
            scan_response,
            &scan_response_len,
            H2_ESP_BLE_AD_TYPE_MANUFACTURER,
            data->manufacturer_data.data,
            data->manufacturer_data.len)) {
        h2_esp_ble_unlock_adv();
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (legacy && data->local_name != NULL) {
        size_t name_len = strlen(data->local_name);
        if (name_len + 2u > sizeof(scan_response) ||
            !h2_esp_ble_adv_put(
                scan_response,
                &scan_response_len,
                H2_ESP_BLE_AD_TYPE_NAME_COMPLETE,
                (const uint8_t *)data->local_name,
                name_len)) {
            h2_esp_ble_unlock_adv();
            return H2_PAL_ERR_INVALID_ARG;
        }
    }
    struct os_mbuf *mbuf = os_msys_get_pkthdr(0u, 0u);
    if (mbuf == NULL) {
        h2_esp_ble_unlock_adv();
        return H2_PAL_ERR_NO_MEMORY;
    }
    int nimble_rc = os_mbuf_append(mbuf, encoded, encoded_len);
    if (nimble_rc != 0) {
        os_mbuf_free_chain(mbuf);
        h2_esp_ble_unlock_adv();
        return h2_esp_ble_map_rc(nimble_rc);
    }
    nimble_rc = ble_gap_ext_adv_set_data(set->instance, mbuf);
    if (nimble_rc != 0) {
        h2_esp_ble_unlock_adv();
        return h2_esp_ble_map_rc(nimble_rc);
    }
    if (scan_response_len > 0u) {
        mbuf = os_msys_get_pkthdr(0u, 0u);
        if (mbuf == NULL) {
            h2_esp_ble_unlock_adv();
            return H2_PAL_ERR_NO_MEMORY;
        }
        nimble_rc = os_mbuf_append(
            mbuf, scan_response, scan_response_len);
        if (nimble_rc != 0) {
            os_mbuf_free_chain(mbuf);
            h2_esp_ble_unlock_adv();
            return h2_esp_ble_map_rc(nimble_rc);
        }
        nimble_rc = ble_gap_ext_adv_rsp_set_data(set->instance, mbuf);
        if (nimble_rc != 0) {
            h2_esp_ble_unlock_adv();
            return h2_esp_ble_map_rc(nimble_rc);
        }
    }
    set->data_staged = true;
    h2_esp_ble_unlock_adv();
    return H2_PAL_OK;
#endif
}

static h2_pal_result_t h2_esp_ble_adv_set_start(
    h2_pal_ble_t *ble,
    h2_pal_ble_adv_set_t *set) {
    (void)ble;
#if !CONFIG_BT_NIMBLE_EXT_ADV
    (void)set;
    return H2_PAL_ERR_UNSUPPORTED;
#else
    h2_pal_result_t lock_rc = h2_esp_ble_lock_adv();
    if (lock_rc != H2_PAL_OK) {
        return lock_rc;
    }
    if (!h2_esp_ble_adv_set_valid_unlocked(set)) {
        h2_esp_ble_unlock_adv();
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (!set->data_staged || set->active) {
        h2_esp_ble_unlock_adv();
        return H2_PAL_ERR_INVALID_STATE;
    }
    uint32_t duration_units = set->params.type == H2_PAL_BLE_ADV_TYPE_LEGACY
                                  ? 0u
                                  : (set->params.duration_ms + 9u) / 10u;
    uint8_t max_events = set->params.type == H2_PAL_BLE_ADV_TYPE_LEGACY
                             ? 0u
                             : set->params.max_adv_events;
    int nimble_rc = ble_gap_ext_adv_start(
        set->instance, (int)duration_units, max_events);
    h2_pal_result_t rc = h2_esp_ble_map_rc(nimble_rc);
    if (rc != H2_PAL_OK) {
        ESP_LOGW(
            TAG,
            "extended advertising start failed instance=%u rc=%d mapped=%d",
            (unsigned)set->instance,
            nimble_rc,
            rc);
        h2_esp_ble_unlock_adv();
        return rc;
    }
    set->active = true;
    ESP_LOGI(
        TAG,
        "extended advertising started instance=%u connectable=%u duration=%u max_events=%u",
        (unsigned)set->instance,
        set->params.mode == H2_PAL_BLE_ADV_MODE_CONNECTABLE ? 1u : 0u,
        (unsigned)set->params.duration_ms,
        (unsigned)set->params.max_adv_events);
    h2_pal_ble_adv_set_event_t payload = {
        .set = set,
        .status = H2_PAL_OK,
    };
    h2_esp_ble_post(
        H2_PAL_SYSTEM_EVENT_TYPE_BLE_ADVERTISING_STARTED,
        &payload,
        sizeof(payload));
    h2_esp_ble_unlock_adv();
    return H2_PAL_OK;
#endif
}

#if CONFIG_BT_NIMBLE_EXT_ADV
static h2_pal_result_t h2_esp_ble_adv_set_stop_unlocked(
    h2_pal_ble_adv_set_t *set) {
    if (!h2_esp_ble_adv_set_valid_unlocked(set)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (!set->active) {
        return H2_PAL_OK;
    }
    int nimble_rc = ble_gap_ext_adv_stop(set->instance);
    if (nimble_rc == 0 || nimble_rc == BLE_HS_EALREADY) {
        /* NimBLE's explicit extended-advertising stop completes
         * synchronously and clears the procedure without emitting
         * BLE_GAP_EVENT_ADV_COMPLETE.  Complete the PAL set here; the GAP
         * event remains responsible for duration/max-event termination. */
        h2_esp_ble_complete_adv_set_unlocked(set, H2_PAL_OK);
        return H2_PAL_OK;
    }
    return h2_esp_ble_map_rc(nimble_rc);
}
#endif

static h2_pal_result_t h2_esp_ble_adv_set_stop(
    h2_pal_ble_t *ble,
    h2_pal_ble_adv_set_t *set) {
    (void)ble;
#if !CONFIG_BT_NIMBLE_EXT_ADV
    (void)set;
    return H2_PAL_ERR_UNSUPPORTED;
#else
    h2_pal_result_t rc = h2_esp_ble_lock_adv();
    if (rc != H2_PAL_OK) {
        return rc;
    }
    rc = h2_esp_ble_adv_set_stop_unlocked(set);
    h2_esp_ble_unlock_adv();
    return rc;
#endif
}

static h2_pal_result_t h2_esp_ble_adv_set_destroy(
    h2_pal_ble_t *ble,
    h2_pal_ble_adv_set_t *set) {
#if !CONFIG_BT_NIMBLE_EXT_ADV
    (void)ble;
    (void)set;
    return H2_PAL_ERR_UNSUPPORTED;
#else
    (void)ble;
    h2_pal_result_t rc = h2_esp_ble_lock_adv();
    if (rc != H2_PAL_OK) {
        return rc;
    }
    rc = h2_esp_ble_adv_set_stop_unlocked(set);
    if (rc != H2_PAL_OK) {
        h2_esp_ble_unlock_adv();
        return rc;
    }
    if (!h2_esp_ble_adv_set_valid_unlocked(set)) {
        h2_esp_ble_unlock_adv();
        return H2_PAL_ERR_INVALID_ARG;
    }
    rc = h2_esp_ble_map_rc(ble_gap_ext_adv_remove(set->instance));
    if (rc != H2_PAL_OK) {
        h2_esp_ble_unlock_adv();
        return rc;
    }
    memset(set, 0, sizeof(*set));
    h2_esp_ble_unlock_adv();
    return H2_PAL_OK;
#endif
}

static h2_pal_result_t h2_esp_ble_start_scan(
    h2_pal_ble_t *ble,
    const h2_pal_ble_scan_params_t *params,
    h2_pal_ble_scan_result_fn on_result,
    void *user) {
    (void)ble;
    if (!s_h2_esp_ble_started) {
        return H2_PAL_ERR_INVALID_STATE;
    }

    s_h2_esp_ble_scan_cb = on_result;
    s_h2_esp_ble_scan_user = user;
    if (s_h2_esp_ble_events != NULL) {
        xEventGroupClearBits(s_h2_esp_ble_events, H2_ESP_BLE_SCAN_STOPPED_BIT);
    }
    h2_pal_result_t rc;
    if (params->type == H2_PAL_BLE_SCAN_TYPE_EXTENDED) {
#if CONFIG_BT_NIMBLE_EXT_ADV
        struct ble_gap_ext_disc_params ext_params;
        memset(&ext_params, 0, sizeof(ext_params));
        ext_params.passive = params->mode == H2_PAL_BLE_SCAN_MODE_PASSIVE ? 1 : 0;
        ext_params.itvl = h2_esp_ble_ext_scan_ms_to_units625(params->interval_ms);
        ext_params.window = h2_esp_ble_ext_scan_ms_to_units625(params->window_ms);
        h2_pal_ble_scan_phy_mask_t mask = params->phy_mask == 0u
                                             ? H2_PAL_BLE_SCAN_PHY_1M
                                             : params->phy_mask;
        uint16_t duration = params->timeout_ms == 0u
                                ? 0u
                                : (uint16_t)((params->timeout_ms + 9u) / 10u);
        int nimble_rc = ble_gap_ext_disc(
            s_h2_esp_ble_own_addr_type,
            duration,
            0u,
            1u,
            0u,
            0u,
            (mask & H2_PAL_BLE_SCAN_PHY_1M) != 0u ? &ext_params : NULL,
            (mask & H2_PAL_BLE_SCAN_PHY_CODED) != 0u ? &ext_params : NULL,
            h2_esp_ble_gap_event,
            NULL);
        if (nimble_rc != 0 && nimble_rc != BLE_HS_EALREADY) {
            ESP_LOGE(TAG, "extended scan start failed rc=%d", nimble_rc);
        }
        rc = h2_esp_ble_map_rc(nimble_rc);
#else
        rc = H2_PAL_ERR_UNSUPPORTED;
#endif
    } else {
        struct ble_gap_disc_params disc_params;
        memset(&disc_params, 0, sizeof(disc_params));
        disc_params.filter_duplicates = 1;
        disc_params.passive = params->mode == H2_PAL_BLE_SCAN_MODE_PASSIVE ? 1 : 0;
        disc_params.itvl = h2_esp_ble_ms_to_units625(params->interval_ms);
        disc_params.window = h2_esp_ble_ms_to_units625(params->window_ms);
        int32_t duration_ms = params->timeout_ms == 0u ? BLE_HS_FOREVER : (int32_t)params->timeout_ms;
        rc = h2_esp_ble_map_rc(ble_gap_disc(
            s_h2_esp_ble_own_addr_type,
            duration_ms,
            &disc_params,
            h2_esp_ble_gap_event,
            NULL));
    }
    if (rc == H2_PAL_OK) {
        h2_esp_ble_post(H2_PAL_SYSTEM_EVENT_TYPE_BLE_SCAN_STARTED, NULL, 0u);
    } else {
        s_h2_esp_ble_scan_cb = NULL;
        s_h2_esp_ble_scan_user = NULL;
    }
    return rc;
}

static h2_pal_result_t h2_esp_ble_stop_scan(h2_pal_ble_t *ble) {
    (void)ble;
    int rc = ble_gap_disc_cancel();
    if (rc == 0 && !ble_gap_disc_active()) {
        h2_esp_ble_complete_scan();
        return H2_PAL_OK;
    }
    if (rc == 0 && s_h2_esp_ble_events != NULL) {
        EventBits_t bits = xEventGroupWaitBits(
            s_h2_esp_ble_events,
            H2_ESP_BLE_SCAN_STOPPED_BIT,
            pdTRUE,
            pdTRUE,
            pdMS_TO_TICKS(H2_ESP_BLE_GAP_STOP_TIMEOUT_MS));
        if ((bits & H2_ESP_BLE_SCAN_STOPPED_BIT) == 0u && ble_gap_disc_active()) {
            return H2_PAL_ERR_TIMEOUT;
        }
    }
    return h2_esp_ble_map_rc(rc);
}

static uint16_t h2_esp_ble_gatt_flags(const h2_pal_ble_gatt_characteristic_t *ch) {
    uint16_t flags = 0u;
    if ((ch->properties & H2_PAL_BLE_GATT_PROPERTY_READ) != 0u) {
        flags |= BLE_GATT_CHR_F_READ;
    }
    if ((ch->properties & H2_PAL_BLE_GATT_PROPERTY_WRITE) != 0u) {
        flags |= BLE_GATT_CHR_F_WRITE;
    }
    if ((ch->properties & H2_PAL_BLE_GATT_PROPERTY_WRITE_NO_RSP) != 0u) {
        flags |= BLE_GATT_CHR_F_WRITE_NO_RSP;
    }
    if ((ch->properties & H2_PAL_BLE_GATT_PROPERTY_NOTIFY) != 0u) {
        flags |= BLE_GATT_CHR_F_NOTIFY;
    }
    if ((ch->properties & H2_PAL_BLE_GATT_PROPERTY_INDICATE) != 0u) {
        flags |= BLE_GATT_CHR_F_INDICATE;
    }
    if ((ch->permissions &
         H2_PAL_BLE_GATT_PERMISSION_READ_ENCRYPTED) != 0u) {
        flags |= BLE_GATT_CHR_F_READ_ENC;
    }
    if ((ch->permissions &
         H2_PAL_BLE_GATT_PERMISSION_WRITE_ENCRYPTED) != 0u) {
        flags |= BLE_GATT_CHR_F_WRITE_ENC;
    }
    if ((ch->permissions &
         H2_PAL_BLE_GATT_PERMISSION_READ_AUTHENTICATED) != 0u) {
        flags |= BLE_GATT_CHR_F_READ_AUTHEN;
    }
    if ((ch->permissions &
         H2_PAL_BLE_GATT_PERMISSION_WRITE_AUTHENTICATED) != 0u) {
        flags |= BLE_GATT_CHR_F_WRITE_AUTHEN;
    }
    return flags;
}

static h2_pal_result_t h2_esp_ble_register_gatt_services(
    h2_pal_ble_t *ble,
    const h2_pal_ble_gatt_service_t *services,
    size_t count) {
    (void)ble;
    if (count == 0u) {
        return H2_PAL_OK;
    }
    if (services == NULL || count != 1u || services[0].characteristics == NULL ||
        services[0].characteristic_count == 0u ||
        services[0].characteristic_count >
            H2_ESP_BLE_MAX_GATT_CHARACTERISTICS_PER_SERVICE) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    ble_uuid_any_t service_uuid;
    if (!h2_esp_ble_uuid_from_pal(&services[0].uuid, &service_uuid)) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    if (s_h2_esp_ble_gatt_mutex == NULL) {
        s_h2_esp_ble_gatt_mutex = xSemaphoreCreateMutex();
        if (s_h2_esp_ble_gatt_mutex == NULL) return H2_PAL_ERR_NO_MEMORY;
    }
    ble_uuid_any_t
        char_uuids[H2_ESP_BLE_MAX_GATT_CHARACTERISTICS_PER_SERVICE];
    memset(char_uuids, 0, sizeof(char_uuids));
    for (size_t i = 0u; i < services[0].characteristic_count; ++i) {
        if (!h2_esp_ble_uuid_from_pal(
                &services[0].characteristics[i].uuid, &char_uuids[i])) {
            return H2_PAL_ERR_UNSUPPORTED;
        }
    }
#if !CONFIG_BT_NIMBLE_DYNAMIC_SERVICE
    if (s_h2_esp_ble_started && !s_h2_esp_ble_gatt_configured) {
        return H2_PAL_ERR_INVALID_STATE;
    }
#endif
    if (xSemaphoreTake(s_h2_esp_ble_gatt_mutex, portMAX_DELAY) != pdTRUE) {
        return H2_PAL_ERR_IO;
    }
    size_t service_index = s_h2_esp_ble_service_count;
    for (size_t i = 0u; i < s_h2_esp_ble_service_count; ++i) {
        if (ble_uuid_cmp(
                &s_h2_esp_ble_service_uuid[i].u, &service_uuid.u) == 0) {
            service_index = i;
            break;
        }
    }
    bool add_service = service_index == s_h2_esp_ble_service_count;
    if (add_service &&
        s_h2_esp_ble_service_count >= H2_ESP_BLE_MAX_GATT_SERVICES) {
        (void)xSemaphoreGive(s_h2_esp_ble_gatt_mutex);
        return H2_PAL_ERR_NO_MEMORY;
    }
    if (!add_service &&
        s_h2_esp_ble_service_characteristic_count[service_index] !=
            services[0].characteristic_count) {
        (void)xSemaphoreGive(s_h2_esp_ble_gatt_mutex);
        return H2_PAL_ERR_INVALID_STATE;
    }
    size_t first =
        service_index * H2_ESP_BLE_MAX_GATT_CHARACTERISTICS_PER_SERVICE;
    for (size_t i = 0u; i < services[0].characteristic_count; ++i) {
        if (!add_service &&
            ble_uuid_cmp(
                &s_h2_esp_ble_char_uuid[first + i].u,
                &char_uuids[i].u) != 0) {
            (void)xSemaphoreGive(s_h2_esp_ble_gatt_mutex);
            return H2_PAL_ERR_INVALID_STATE;
        }
    }
    if (add_service) {
        s_h2_esp_ble_service_uuid[service_index] = service_uuid;
        s_h2_esp_ble_service_characteristic_count[service_index] =
            services[0].characteristic_count;
        ++s_h2_esp_ble_service_count;
        s_h2_esp_ble_characteristic_count =
            s_h2_esp_ble_service_count *
            H2_ESP_BLE_MAX_GATT_CHARACTERISTICS_PER_SERVICE;
        s_h2_esp_ble_svc_defs[service_index] = (struct ble_gatt_svc_def){
            .type = BLE_GATT_SVC_TYPE_PRIMARY,
            .uuid = &s_h2_esp_ble_service_uuid[service_index].u,
            .characteristics = s_h2_esp_ble_chr_defs[service_index],
        };
    }
    for (size_t i = 0u; i < services[0].characteristic_count; ++i) {
        size_t index = first + i;
        const h2_pal_ble_gatt_characteristic_t *ch = &services[0].characteristics[i];
        s_h2_esp_ble_char_uuid[index] = char_uuids[i];
        s_h2_esp_ble_chr_defs[service_index][i] =
            (struct ble_gatt_chr_def){
                .uuid = &s_h2_esp_ble_char_uuid[index].u,
                .access_cb = h2_esp_ble_gatt_access,
                .arg = &s_h2_esp_ble_char_index[index],
                .flags = h2_esp_ble_gatt_flags(ch),
                .val_handle = &s_h2_esp_ble_value_handle[index],
            };
        if (add_service) s_h2_esp_ble_value_handle[index] = 0u;
        s_h2_esp_ble_value_len[index] = ch->initial_value_len;
        if (s_h2_esp_ble_value_len[index] > sizeof(s_h2_esp_ble_value[index])) {
            s_h2_esp_ble_value_len[index] = sizeof(s_h2_esp_ble_value[index]);
        }
        memset(s_h2_esp_ble_value[index], 0, sizeof(s_h2_esp_ble_value[index]));
        if (ch->initial_value != NULL && s_h2_esp_ble_value_len[index] > 0u) {
            memcpy(s_h2_esp_ble_value[index], ch->initial_value,
                s_h2_esp_ble_value_len[index]);
        }
        s_h2_esp_ble_value_max_len[index] = ch->max_value_len;
        if (s_h2_esp_ble_value_max_len[index] == 0u ||
            s_h2_esp_ble_value_max_len[index] >
                sizeof(s_h2_esp_ble_value[index])) {
            s_h2_esp_ble_value_max_len[index] = sizeof(s_h2_esp_ble_value[index]);
        }
        s_h2_esp_ble_read[index] = ch->read;
        s_h2_esp_ble_write[index] = ch->write;
        s_h2_esp_ble_gatt_user[index] = ch->user;
        s_h2_esp_ble_out_value_handle[index] = ch->out_value_handle;
        s_h2_esp_ble_out_cccd_handle[index] = ch->out_cccd_handle;
    }
    memset(
        &s_h2_esp_ble_chr_defs[service_index]
                                 [services[0].characteristic_count],
        0,
        sizeof(s_h2_esp_ble_chr_defs[service_index]
                                            [services[0].characteristic_count]));
    s_h2_esp_ble_out_service_handle[service_index] =
        services[0].out_service_handle;
    s_h2_esp_ble_gatt_configured = true;
    s_h2_esp_ble_gatt_attached = true;

    if (s_h2_esp_ble_started && add_service) {
#if CONFIG_BT_NIMBLE_DYNAMIC_SERVICE
        int rc = ble_gatts_add_dynamic_svcs(
            &s_h2_esp_ble_svc_defs[service_index]);
        if (rc != 0) {
            memset(&s_h2_esp_ble_svc_defs[service_index], 0,
                sizeof(s_h2_esp_ble_svc_defs[service_index]));
            memset(&s_h2_esp_ble_chr_defs[service_index], 0,
                sizeof(s_h2_esp_ble_chr_defs[service_index]));
            --s_h2_esp_ble_service_count;
            s_h2_esp_ble_characteristic_count =
                s_h2_esp_ble_service_count *
                H2_ESP_BLE_MAX_GATT_CHARACTERISTICS_PER_SERVICE;
            s_h2_esp_ble_gatt_configured =
                s_h2_esp_ble_service_count != 0u;
            (void)xSemaphoreGive(s_h2_esp_ble_gatt_mutex);
            return h2_esp_ble_map_rc(rc);
        }
        h2_esp_ble_update_out_handles();
        h2_esp_ble_clear_out_handle_ptrs();
#endif
    }

    h2_esp_ble_update_out_handles();
    (void)xSemaphoreGive(s_h2_esp_ble_gatt_mutex);
    return H2_PAL_OK;
}

static h2_pal_result_t h2_esp_ble_unregister_gatt_services(
    h2_pal_ble_t *ble) {
    (void)ble;
    if (s_h2_esp_ble_gatt_mutex != NULL) {
        if (xSemaphoreTake(s_h2_esp_ble_gatt_mutex, portMAX_DELAY) != pdTRUE) {
            return H2_PAL_ERR_IO;
        }
    }
    s_h2_esp_ble_gatt_attached = false;
    for (size_t i = 0u; i < H2_ESP_BLE_MAX_GATT_CHARACTERISTICS; ++i) {
        s_h2_esp_ble_read[i] = NULL;
        s_h2_esp_ble_write[i] = NULL;
        s_h2_esp_ble_gatt_user[i] = NULL;
    }
    h2_esp_ble_clear_out_handle_ptrs();
    if (s_h2_esp_ble_gatt_mutex != NULL) {
        (void)xSemaphoreGive(s_h2_esp_ble_gatt_mutex);
    }
    return H2_PAL_OK;
}

static h2_pal_result_t h2_esp_ble_notify(
    h2_pal_ble_t *ble,
    uint16_t conn_handle,
    uint16_t attr_handle,
    const uint8_t *data,
    size_t len) {
    (void)ble;
    if (len > UINT16_MAX) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    struct os_mbuf *om = ble_hs_mbuf_from_flat(data, (uint16_t)len);
    if (om == NULL) {
        return H2_PAL_ERR_WOULD_BLOCK;
    }
    return h2_esp_ble_map_tx_rc(
        ble_gatts_notify_custom(conn_handle, attr_handle, om));
}

static h2_pal_result_t h2_esp_ble_indicate(
    h2_pal_ble_t *ble,
    uint16_t conn_handle,
    uint16_t attr_handle,
    const uint8_t *data,
    size_t len,
    uint32_t timeout_ms) {
    (void)ble;
    if (len > UINT16_MAX) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (timeout_ms == 0u) {
        return H2_PAL_ERR_WOULD_BLOCK;
    }
    if (s_h2_esp_ble_events == NULL) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    h2_pal_result_t result = h2_esp_ble_begin_indication(
        conn_handle, attr_handle);
    if (result != H2_PAL_OK) {
        return result;
    }
    xEventGroupClearBits(
        s_h2_esp_ble_events, H2_ESP_BLE_INDICATION_DONE_BIT);
    struct os_mbuf *om = ble_hs_mbuf_from_flat(data, (uint16_t)len);
    if (om == NULL) {
        h2_esp_ble_cancel_indication_submission(conn_handle, attr_handle);
        return H2_PAL_ERR_WOULD_BLOCK;
    }
    result = h2_esp_ble_map_tx_rc(
        ble_gatts_indicate_custom(conn_handle, attr_handle, om));
    if (result != H2_PAL_OK) {
        h2_esp_ble_cancel_indication_submission(conn_handle, attr_handle);
    }
    if (result != H2_PAL_OK) {
        return result;
    }
    TickType_t wait_ticks = timeout_ms == UINT32_MAX ? portMAX_DELAY :
        pdMS_TO_TICKS(timeout_ms);
    if (wait_ticks == 0u) {
        wait_ticks = 1u;
    }
    EventBits_t bits = xEventGroupWaitBits(
        s_h2_esp_ble_events,
        H2_ESP_BLE_INDICATION_DONE_BIT,
        pdTRUE,
        pdTRUE,
        wait_ticks);
    taskENTER_CRITICAL(&s_h2_esp_ble_indication_lock);
    result = (bits & H2_ESP_BLE_INDICATION_DONE_BIT) != 0u ?
        h2_esp_ble_indication_tracker_take(&s_h2_esp_ble_indication) :
        h2_esp_ble_indication_tracker_timeout(&s_h2_esp_ble_indication);
    taskEXIT_CRITICAL(&s_h2_esp_ble_indication_lock);
    return result;
}

static h2_pal_result_t h2_esp_ble_disconnect(h2_pal_ble_t *ble, uint16_t conn_handle) {
    (void)ble;
    return h2_esp_ble_map_rc(ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM));
}

static h2_pal_result_t h2_esp_ble_configure_pairing(
    h2_pal_ble_t *ble,
    const h2_pal_ble_pairing_config_t *config) {
    (void)ble;
    if (config == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    s_h2_esp_ble_pairing = *config;
    if (!config->enabled) {
        memset(&s_h2_esp_ble_pairing, 0,
               sizeof(s_h2_esp_ble_pairing));
        if (s_h2_esp_ble_previous_pairing.valid) {
            ble_hs_cfg.sm_io_cap =
                s_h2_esp_ble_previous_pairing.io_cap;
            ble_hs_cfg.sm_bonding =
                s_h2_esp_ble_previous_pairing.bonding;
            ble_hs_cfg.sm_mitm =
                s_h2_esp_ble_previous_pairing.mitm;
            ble_hs_cfg.sm_sc =
                s_h2_esp_ble_previous_pairing.secure_connections;
            memset(&s_h2_esp_ble_previous_pairing, 0,
                   sizeof(s_h2_esp_ble_previous_pairing));
        }
        return H2_PAL_OK;
    }
    if (!s_h2_esp_ble_previous_pairing.valid) {
        s_h2_esp_ble_previous_pairing.io_cap = ble_hs_cfg.sm_io_cap;
        s_h2_esp_ble_previous_pairing.bonding = ble_hs_cfg.sm_bonding;
        s_h2_esp_ble_previous_pairing.mitm = ble_hs_cfg.sm_mitm;
        s_h2_esp_ble_previous_pairing.secure_connections =
            ble_hs_cfg.sm_sc;
        s_h2_esp_ble_previous_pairing.valid = true;
    }
    ble_hs_cfg.sm_io_cap =
        config->io == H2_PAL_BLE_PAIRING_IO_DISPLAY_ONLY
            ? BLE_HS_IO_DISPLAY_ONLY
            : BLE_HS_IO_KEYBOARD_ONLY;
    ble_hs_cfg.sm_bonding = 0;
    ble_hs_cfg.sm_mitm = 1;
    ble_hs_cfg.sm_sc = 1;
    return H2_PAL_OK;
}

static h2_pal_result_t h2_esp_ble_pair(
    h2_pal_ble_t *ble,
    uint16_t conn_handle,
    uint32_t timeout_ms) {
    (void)ble;
    if (!s_h2_esp_ble_started || !s_h2_esp_ble_pairing.enabled) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    struct ble_gap_conn_desc desc;
    if (ble_gap_conn_find(conn_handle, &desc) != 0) {
        return H2_PAL_ERR_NOT_FOUND;
    }
    if (desc.sec_state.encrypted && desc.sec_state.authenticated) {
        return H2_PAL_OK;
    }
    xEventGroupClearBits(s_h2_esp_ble_events,
                         H2_ESP_BLE_PAIR_DONE_BIT);
    s_h2_esp_ble_pair_result = H2_PAL_ERR_IO;
    s_h2_esp_ble_pair_conn_handle = conn_handle;
    int rc = ble_gap_security_initiate(conn_handle);
    if (rc != 0) {
        s_h2_esp_ble_pair_conn_handle =
            H2_PAL_BLE_INVALID_CONN_HANDLE;
        return h2_esp_ble_map_rc(rc);
    }
    EventBits_t bits = xEventGroupWaitBits(
        s_h2_esp_ble_events,
        H2_ESP_BLE_PAIR_DONE_BIT,
        pdTRUE,
        pdTRUE,
        pdMS_TO_TICKS(timeout_ms));
    s_h2_esp_ble_pair_conn_handle = H2_PAL_BLE_INVALID_CONN_HANDLE;
    return (bits & H2_ESP_BLE_PAIR_DONE_BIT) != 0u
               ? s_h2_esp_ble_pair_result
               : H2_PAL_ERR_TIMEOUT;
}

static h2_pal_result_t h2_esp_ble_update_connection(
    h2_pal_ble_t *ble,
    uint16_t conn_handle,
    const h2_pal_ble_connection_params_t *params) {
    (void)ble;
    if (!s_h2_esp_ble_started || params == NULL) {
        return params == NULL ? H2_PAL_ERR_INVALID_ARG : H2_PAL_ERR_INVALID_STATE;
    }

    struct ble_gap_upd_params gap_params;
    memset(&gap_params, 0, sizeof(gap_params));
    gap_params.itvl_min = h2_esp_ble_ms_to_units1250(params->interval_min_ms);
    gap_params.itvl_max = h2_esp_ble_ms_to_units1250(
        params->interval_max_ms != 0u ? params->interval_max_ms : params->interval_min_ms);
    gap_params.latency = params->latency;
    gap_params.supervision_timeout = h2_esp_ble_ms_to_units10(params->supervision_timeout_ms);
    return h2_esp_ble_map_rc(ble_gap_update_params(conn_handle, &gap_params));
}

static h2_pal_result_t h2_esp_ble_exchange_mtu(
    h2_pal_ble_t *ble,
    uint16_t conn_handle,
    uint16_t *out_mtu,
    uint32_t timeout_ms) {
    (void)ble;
    if (!s_h2_esp_ble_started || out_mtu == NULL) {
        return out_mtu == NULL ? H2_PAL_ERR_INVALID_ARG : H2_PAL_ERR_INVALID_STATE;
    }

    s_h2_esp_ble_exchange_mtu = 0u;
    s_h2_esp_ble_gatt_result = H2_PAL_ERR_TIMEOUT;
    xEventGroupClearBits(s_h2_esp_ble_events, H2_ESP_BLE_GATT_DONE_BIT);
    h2_pal_result_t rc = h2_esp_ble_map_rc(ble_gattc_exchange_mtu(conn_handle, h2_esp_ble_mtu_cb, NULL));
    if (rc != H2_PAL_OK) {
        return rc;
    }
    rc = h2_esp_ble_wait_bit(H2_ESP_BLE_GATT_DONE_BIT, timeout_ms);
    if (rc == H2_PAL_OK) {
        rc = s_h2_esp_ble_gatt_result;
    }
    if (rc == H2_PAL_OK) {
        *out_mtu = s_h2_esp_ble_exchange_mtu;
    }
    return rc;
}

static h2_pal_result_t h2_esp_ble_set_preferred_phy(
    h2_pal_ble_t *ble,
    uint16_t conn_handle,
    h2_pal_ble_phy_t tx_phy,
    h2_pal_ble_phy_t rx_phy,
    uint32_t timeout_ms) {
    (void)ble;
    if (!s_h2_esp_ble_started) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    uint8_t tx_mask = h2_esp_ble_phy_mask(tx_phy);
    uint8_t rx_mask = h2_esp_ble_phy_mask(rx_phy);
    if (tx_mask == 0u || rx_mask == 0u) {
        return H2_PAL_ERR_INVALID_ARG;
    }

    memset(&s_h2_esp_ble_phy_info, 0, sizeof(s_h2_esp_ble_phy_info));
    s_h2_esp_ble_phy_result = H2_PAL_ERR_TIMEOUT;
    xEventGroupClearBits(s_h2_esp_ble_events, H2_ESP_BLE_PHY_DONE_BIT);
    h2_pal_result_t rc = h2_esp_ble_map_rc(
        ble_gap_set_prefered_le_phy(conn_handle, tx_mask, rx_mask, BLE_GAP_LE_PHY_CODED_ANY));
    if (rc != H2_PAL_OK) {
        return rc;
    }
    rc = h2_esp_ble_wait_bit(H2_ESP_BLE_PHY_DONE_BIT, timeout_ms);
    if (rc == H2_PAL_OK) {
        rc = s_h2_esp_ble_phy_result;
    }
    return rc;
}

static h2_pal_result_t h2_esp_ble_read_phy(
    h2_pal_ble_t *ble,
    uint16_t conn_handle,
    h2_pal_ble_phy_info_t *out_phy,
    uint32_t timeout_ms) {
    (void)ble;
    (void)timeout_ms;
    if (!s_h2_esp_ble_started || out_phy == NULL) {
        return out_phy == NULL ? H2_PAL_ERR_INVALID_ARG : H2_PAL_ERR_INVALID_STATE;
    }

    uint8_t tx_phy = 0u;
    uint8_t rx_phy = 0u;
    h2_pal_result_t rc = h2_esp_ble_map_rc(ble_gap_read_le_phy(conn_handle, &tx_phy, &rx_phy));
    if (rc != H2_PAL_OK) {
        return rc;
    }
    out_phy->conn_handle = conn_handle;
    out_phy->tx_phy = h2_esp_ble_phy(tx_phy);
    out_phy->rx_phy = h2_esp_ble_phy(rx_phy);
    return H2_PAL_OK;
}

static h2_pal_result_t h2_esp_ble_connect(
    h2_pal_ble_t *ble,
    const h2_pal_ble_addr_t *addr,
    const h2_pal_ble_connect_params_t *params,
    uint16_t *out_conn_handle) {
    (void)ble;
    if (!s_h2_esp_ble_started || addr == NULL || params == NULL || out_conn_handle == NULL) {
        return addr == NULL || params == NULL || out_conn_handle == NULL
                   ? H2_PAL_ERR_INVALID_ARG
                   : H2_PAL_ERR_INVALID_STATE;
    }
    ble_addr_t peer_addr;
    memset(&peer_addr, 0, sizeof(peer_addr));
    peer_addr.type = h2_esp_ble_to_nimble_addr_type(addr->type);
    memcpy(peer_addr.val, addr->value, sizeof(peer_addr.val));

    struct ble_gap_conn_params conn_params;
    memset(&conn_params, 0, sizeof(conn_params));
    conn_params.scan_itvl = h2_esp_ble_ms_to_units625(100u);
    conn_params.scan_window = h2_esp_ble_ms_to_units625(50u);
    conn_params.itvl_min = h2_esp_ble_ms_to_units1250(params->interval_min_ms);
    conn_params.itvl_max = h2_esp_ble_ms_to_units1250(
        params->interval_max_ms != 0u ? params->interval_max_ms : params->interval_min_ms);
    conn_params.latency = params->latency;
    conn_params.supervision_timeout = h2_esp_ble_ms_to_units10(params->supervision_timeout_ms);

    s_h2_esp_ble_connect_result = H2_PAL_ERR_TIMEOUT;
    s_h2_esp_ble_connect_handle = H2_PAL_BLE_INVALID_CONN_HANDLE;
    s_h2_esp_ble_connect_pending = true;
    xEventGroupClearBits(s_h2_esp_ble_events, H2_ESP_BLE_CONNECT_DONE_BIT);
    int32_t timeout_ms = params->timeout_ms == 0u ? 10000 : (int32_t)params->timeout_ms;
    int nimble_rc = ble_gap_connect(
        s_h2_esp_ble_own_addr_type,
        &peer_addr,
        timeout_ms,
        &conn_params,
        h2_esp_ble_gap_event,
        NULL);
    h2_pal_result_t rc = h2_esp_ble_map_rc(nimble_rc);
    if (rc != H2_PAL_OK) {
        ESP_LOGE(
            TAG,
            "connect start failed rc=%d peer_type=%u scan_active=%u",
            nimble_rc,
            (unsigned)peer_addr.type,
            ble_gap_disc_active() ? 1u : 0u);
        s_h2_esp_ble_connect_pending = false;
        return rc;
    }
    rc = h2_esp_ble_wait_bit(H2_ESP_BLE_CONNECT_DONE_BIT, params->timeout_ms);
    if (rc != H2_PAL_OK) {
        (void)ble_gap_conn_cancel();
        s_h2_esp_ble_connect_pending = false;
        return rc;
    }
    if (s_h2_esp_ble_connect_result != H2_PAL_OK) {
        return s_h2_esp_ble_connect_result;
    }
    *out_conn_handle = s_h2_esp_ble_connect_handle;
    return H2_PAL_OK;
}

static h2_pal_result_t h2_esp_ble_gatt_discover(
    h2_pal_ble_t *ble,
    uint16_t conn_handle,
    const h2_pal_ble_gatt_discovery_request_t *request,
    h2_pal_ble_gatt_discovery_entry_t *entries,
    size_t max_entries,
    size_t *out_count,
    uint32_t timeout_ms) {
    (void)ble;
    if (request == NULL || entries == NULL || out_count == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (request->uuid_filter.len > sizeof(s_h2_esp_ble_discovery_uuid_filter_data) ||
        (request->uuid_filter.len > 0u && request->uuid_filter.data == NULL)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    s_h2_esp_ble_discovery_entries = entries;
    s_h2_esp_ble_discovery_max_entries = max_entries < H2_ESP_BLE_MAX_DISCOVERY_ENTRIES
                                             ? max_entries
                                             : H2_ESP_BLE_MAX_DISCOVERY_ENTRIES;
    s_h2_esp_ble_discovery_count = 0u;
    s_h2_esp_ble_gatt_result = H2_PAL_ERR_TIMEOUT;
    memset(s_h2_esp_ble_discovery_uuid_data, 0, sizeof(s_h2_esp_ble_discovery_uuid_data));
    memset(s_h2_esp_ble_discovery_uuid_filter_data, 0,
           sizeof(s_h2_esp_ble_discovery_uuid_filter_data));
    s_h2_esp_ble_discovery_uuid_filter = (h2_pal_ble_uuid_t){0};
    if (request->uuid_filter.len > 0u) {
        memcpy(s_h2_esp_ble_discovery_uuid_filter_data,
               request->uuid_filter.data, request->uuid_filter.len);
        s_h2_esp_ble_discovery_uuid_filter.data =
            s_h2_esp_ble_discovery_uuid_filter_data;
        s_h2_esp_ble_discovery_uuid_filter.len = request->uuid_filter.len;
    }
    xEventGroupClearBits(s_h2_esp_ble_events, H2_ESP_BLE_GATT_DONE_BIT);

    uint16_t start_handle = request->start_handle != 0u ? request->start_handle : 1u;
    uint16_t end_handle = request->end_handle != 0u ? request->end_handle : 0xffffu;
    int rc = 0;
    uint16_t uuid16 = h2_esp_ble_uuid16_value(&request->uuid_filter);
    if (request->kind == H2_PAL_BLE_GATT_DISCOVERY_SERVICE) {
        if (uuid16 != 0u) {
            ble_uuid16_t uuid = BLE_UUID16_INIT(uuid16);
            rc = ble_gattc_disc_svc_by_uuid(conn_handle, &uuid.u, h2_esp_ble_discover_service_cb, NULL);
        } else {
            rc = ble_gattc_disc_all_svcs(conn_handle, h2_esp_ble_discover_service_cb, NULL);
        }
    } else if (request->kind == H2_PAL_BLE_GATT_DISCOVERY_CHARACTERISTIC) {
        if (uuid16 != 0u) {
            ble_uuid16_t uuid = BLE_UUID16_INIT(uuid16);
            rc = ble_gattc_disc_chrs_by_uuid(
                conn_handle,
                start_handle,
                end_handle,
                &uuid.u,
                h2_esp_ble_discover_characteristic_cb,
                NULL);
        } else {
            rc = ble_gattc_disc_all_chrs(
                conn_handle,
                start_handle,
                end_handle,
                h2_esp_ble_discover_characteristic_cb,
                NULL);
        }
    } else if (request->kind == H2_PAL_BLE_GATT_DISCOVERY_DESCRIPTOR) {
        /* PAL descriptor ranges are inclusive; NimBLE starts after the
         * characteristic value handle supplied here. */
        uint16_t characteristic_value_handle =
            start_handle > 1u ? (uint16_t)(start_handle - 1u) : start_handle;
        rc = ble_gattc_disc_all_dscs(
            conn_handle,
            characteristic_value_handle,
            end_handle,
            h2_esp_ble_discover_descriptor_cb,
            NULL);
    } else {
        return H2_PAL_ERR_INVALID_ARG;
    }
    h2_pal_result_t result = h2_esp_ble_map_rc(rc);
    if (result != H2_PAL_OK) {
        return result;
    }
    result = h2_esp_ble_wait_bit(H2_ESP_BLE_GATT_DONE_BIT, timeout_ms);
    if (result == H2_PAL_OK) {
        result = s_h2_esp_ble_gatt_result;
    }
    *out_count = s_h2_esp_ble_discovery_count;
    return result;
}

static h2_pal_result_t h2_esp_ble_gatt_read(
    h2_pal_ble_t *ble,
    uint16_t conn_handle,
    uint16_t attr_handle,
    uint16_t offset,
    uint8_t *out,
    size_t out_size,
    size_t *out_len,
    uint32_t timeout_ms) {
    (void)ble;
    if (offset != 0u) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    s_h2_esp_ble_read_out = out;
    s_h2_esp_ble_read_out_size = out_size;
    s_h2_esp_ble_read_out_len = 0u;
    s_h2_esp_ble_gatt_result = H2_PAL_ERR_TIMEOUT;
    xEventGroupClearBits(s_h2_esp_ble_events, H2_ESP_BLE_GATT_DONE_BIT);
    h2_pal_result_t rc =
        h2_esp_ble_map_rc(ble_gattc_read(conn_handle, attr_handle, h2_esp_ble_gatt_attr_cb, NULL));
    if (rc != H2_PAL_OK) {
        return rc;
    }
    rc = h2_esp_ble_wait_bit(H2_ESP_BLE_GATT_DONE_BIT, timeout_ms);
    if (rc == H2_PAL_OK) {
        rc = s_h2_esp_ble_gatt_result;
    }
    *out_len = s_h2_esp_ble_read_out_len;
    s_h2_esp_ble_read_out = NULL;
    s_h2_esp_ble_read_out_size = 0u;
    return rc;
}

static h2_pal_result_t h2_esp_ble_gatt_write(
    h2_pal_ble_t *ble,
    uint16_t conn_handle,
    uint16_t attr_handle,
    const uint8_t *data,
    size_t len,
    bool with_response,
    uint32_t timeout_ms) {
    (void)ble;
    if (len > UINT16_MAX) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (!with_response) {
        return h2_esp_ble_map_tx_rc(
            ble_gattc_write_no_rsp_flat(
                conn_handle, attr_handle, data, (uint16_t)len));
    }
    s_h2_esp_ble_read_out = NULL;
    s_h2_esp_ble_gatt_result = H2_PAL_ERR_TIMEOUT;
    xEventGroupClearBits(s_h2_esp_ble_events, H2_ESP_BLE_GATT_DONE_BIT);
    h2_pal_result_t rc =
        h2_esp_ble_map_rc(ble_gattc_write_flat(conn_handle, attr_handle, data, (uint16_t)len, h2_esp_ble_gatt_attr_cb, NULL));
    if (rc != H2_PAL_OK) {
        return rc;
    }
    rc = h2_esp_ble_wait_bit(H2_ESP_BLE_GATT_DONE_BIT, timeout_ms);
    return rc == H2_PAL_OK ? s_h2_esp_ble_gatt_result : rc;
}

static h2_pal_result_t h2_esp_ble_gatt_subscribe(
    h2_pal_ble_t *ble,
    uint16_t conn_handle,
    const h2_pal_ble_gatt_subscribe_t *subscribe,
    uint32_t timeout_ms) {
    if (subscribe == NULL || subscribe->cccd_handle == H2_PAL_BLE_INVALID_ATTR_HANDLE) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    uint16_t value = 0u;
    if (subscribe->enable) {
        value = subscribe->mode == H2_PAL_BLE_SUBSCRIBE_MODE_INDICATE ? 0x0002u : 0x0001u;
    }
    uint8_t cccd[2] = { (uint8_t)(value & 0xffu), (uint8_t)(value >> 8) };
    return h2_esp_ble_gatt_write(ble, conn_handle, subscribe->cccd_handle, cccd, sizeof(cccd), true, timeout_ms);
}
#endif

#if !CONFIG_BT_NIMBLE_ENABLED
static h2_pal_result_t h2_esp_ble_unsupported_start(h2_pal_ble_t *ble) {
    (void)ble;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t h2_esp_ble_unsupported_stop(h2_pal_ble_t *ble) {
    (void)ble;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t h2_esp_ble_unsupported_connect(
    h2_pal_ble_t *ble,
    const h2_pal_ble_addr_t *addr,
    const h2_pal_ble_connect_params_t *params,
    uint16_t *out_conn_handle) {
    (void)ble;
    (void)addr;
    (void)params;
    if (out_conn_handle != NULL) {
        *out_conn_handle = H2_PAL_BLE_INVALID_CONN_HANDLE;
    }
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t h2_esp_ble_unsupported_update_connection(
    h2_pal_ble_t *ble,
    uint16_t conn_handle,
    const h2_pal_ble_connection_params_t *params) {
    (void)ble;
    (void)conn_handle;
    (void)params;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t h2_esp_ble_unsupported_exchange_mtu(
    h2_pal_ble_t *ble,
    uint16_t conn_handle,
    uint16_t *out_mtu,
    uint32_t timeout_ms) {
    (void)ble;
    (void)conn_handle;
    (void)timeout_ms;
    if (out_mtu != NULL) {
        *out_mtu = 0u;
    }
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t h2_esp_ble_unsupported_set_preferred_phy(
    h2_pal_ble_t *ble,
    uint16_t conn_handle,
    h2_pal_ble_phy_t tx_phy,
    h2_pal_ble_phy_t rx_phy,
    uint32_t timeout_ms) {
    (void)ble;
    (void)conn_handle;
    (void)tx_phy;
    (void)rx_phy;
    (void)timeout_ms;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t h2_esp_ble_unsupported_read_phy(
    h2_pal_ble_t *ble,
    uint16_t conn_handle,
    h2_pal_ble_phy_info_t *out_phy,
    uint32_t timeout_ms) {
    (void)ble;
    (void)conn_handle;
    (void)timeout_ms;
    if (out_phy != NULL) {
        out_phy->conn_handle = H2_PAL_BLE_INVALID_CONN_HANDLE;
        out_phy->tx_phy = H2_PAL_BLE_PHY_UNKNOWN;
        out_phy->rx_phy = H2_PAL_BLE_PHY_UNKNOWN;
    }
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t h2_esp_ble_unsupported_gatt_discover(
    h2_pal_ble_t *ble,
    uint16_t conn_handle,
    const h2_pal_ble_gatt_discovery_request_t *request,
    h2_pal_ble_gatt_discovery_entry_t *entries,
    size_t max_entries,
    size_t *out_count,
    uint32_t timeout_ms) {
    (void)ble;
    (void)conn_handle;
    (void)request;
    (void)entries;
    (void)max_entries;
    (void)timeout_ms;
    if (out_count != NULL) {
        *out_count = 0u;
    }
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t h2_esp_ble_unsupported_gatt_read(
    h2_pal_ble_t *ble,
    uint16_t conn_handle,
    uint16_t attr_handle,
    uint16_t offset,
    uint8_t *out,
    size_t out_size,
    size_t *out_len,
    uint32_t timeout_ms) {
    (void)ble;
    (void)conn_handle;
    (void)attr_handle;
    (void)offset;
    (void)out;
    (void)out_size;
    (void)timeout_ms;
    if (out_len != NULL) {
        *out_len = 0u;
    }
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t h2_esp_ble_unsupported_gatt_write(
    h2_pal_ble_t *ble,
    uint16_t conn_handle,
    uint16_t attr_handle,
    const uint8_t *data,
    size_t len,
    bool with_response,
    uint32_t timeout_ms) {
    (void)ble;
    (void)conn_handle;
    (void)attr_handle;
    (void)data;
    (void)len;
    (void)with_response;
    (void)timeout_ms;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t h2_esp_ble_unsupported_gatt_subscribe(
    h2_pal_ble_t *ble,
    uint16_t conn_handle,
    const h2_pal_ble_gatt_subscribe_t *subscribe,
    uint32_t timeout_ms) {
    (void)ble;
    (void)conn_handle;
    (void)subscribe;
    (void)timeout_ms;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t h2_esp_ble_unsupported_indicate(
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
#endif

static h2_pal_result_t
h2_esp_ble_adv_set_set_scan_response_data_unsupported(
    h2_pal_ble_t *ble,
    h2_pal_ble_adv_set_t *set,
    const h2_pal_ble_adv_data_t *data) {
    (void)ble;
    (void)set;
    (void)data;
    return H2_PAL_ERR_UNSUPPORTED;
}

static const h2_pal_ble_vtable_t s_h2_esp_ble_vtable = {
#if CONFIG_BT_NIMBLE_ENABLED
    .start = (h2_pal_result_t (*)(void *))h2_esp_ble_start,
    .stop = (h2_pal_result_t (*)(void *))h2_esp_ble_stop,
    .set_adv_data = (h2_pal_result_t (*)(void *, const h2_pal_ble_adv_data_t *))h2_esp_ble_set_adv_data,
    .start_advertising = (h2_pal_result_t (*)(void *, const h2_pal_ble_adv_params_t *))h2_esp_ble_start_advertising,
    .stop_advertising = (h2_pal_result_t (*)(void *))h2_esp_ble_stop_advertising,
    .adv_set_create = (h2_pal_result_t (*)(void *, const h2_pal_ble_adv_params_t *, h2_pal_ble_adv_set_t **))h2_esp_ble_adv_set_create,
    .adv_set_set_data = (h2_pal_result_t (*)(void *, h2_pal_ble_adv_set_t *, const h2_pal_ble_adv_data_t *))h2_esp_ble_adv_set_set_data,
    .adv_set_set_scan_response_data = (h2_pal_result_t (*)(void *, h2_pal_ble_adv_set_t *, const h2_pal_ble_adv_data_t *))h2_esp_ble_adv_set_set_scan_response_data_unsupported,
    .adv_set_start = (h2_pal_result_t (*)(void *, h2_pal_ble_adv_set_t *))h2_esp_ble_adv_set_start,
    .adv_set_stop = (h2_pal_result_t (*)(void *, h2_pal_ble_adv_set_t *))h2_esp_ble_adv_set_stop,
    .adv_set_destroy = (h2_pal_result_t (*)(void *, h2_pal_ble_adv_set_t *))h2_esp_ble_adv_set_destroy,
    .start_scan = (h2_pal_result_t (*)(void *, const h2_pal_ble_scan_params_t *, h2_pal_ble_scan_result_fn, void *))h2_esp_ble_start_scan,
    .stop_scan = (h2_pal_result_t (*)(void *))h2_esp_ble_stop_scan,
    .register_gatt_services = (h2_pal_result_t (*)(void *, const h2_pal_ble_gatt_service_t *, size_t))h2_esp_ble_register_gatt_services,
    .unregister_gatt_services = (h2_pal_result_t (*)(void *))h2_esp_ble_unregister_gatt_services,
    .notify = (h2_pal_result_t (*)(void *, uint16_t, uint16_t, const uint8_t *, size_t))h2_esp_ble_notify,
    .indicate = (h2_pal_result_t (*)(void *, uint16_t, uint16_t, const uint8_t *, size_t, uint32_t))h2_esp_ble_indicate,
    .connect = (h2_pal_result_t (*)(void *, const h2_pal_ble_addr_t *, const h2_pal_ble_connect_params_t *, uint16_t *))h2_esp_ble_connect,
    .configure_pairing = (h2_pal_result_t (*)(void *, const h2_pal_ble_pairing_config_t *))h2_esp_ble_configure_pairing,
    .pair = (h2_pal_result_t (*)(void *, uint16_t, uint32_t))h2_esp_ble_pair,
    .disconnect = (h2_pal_result_t (*)(void *, uint16_t))h2_esp_ble_disconnect,
    .update_connection = (h2_pal_result_t (*)(void *, uint16_t, const h2_pal_ble_connection_params_t *))h2_esp_ble_update_connection,
    .exchange_mtu = (h2_pal_result_t (*)(void *, uint16_t, uint16_t *, uint32_t))h2_esp_ble_exchange_mtu,
    .set_preferred_phy = (h2_pal_result_t (*)(void *, uint16_t, h2_pal_ble_phy_t, h2_pal_ble_phy_t, uint32_t))h2_esp_ble_set_preferred_phy,
    .read_phy = (h2_pal_result_t (*)(void *, uint16_t, h2_pal_ble_phy_info_t *, uint32_t))h2_esp_ble_read_phy,
    .gatt_discover = (h2_pal_result_t (*)(void *, uint16_t, const h2_pal_ble_gatt_discovery_request_t *, h2_pal_ble_gatt_discovery_entry_t *, size_t, size_t *, uint32_t))h2_esp_ble_gatt_discover,
    .gatt_read = (h2_pal_result_t (*)(void *, uint16_t, uint16_t, uint16_t, uint8_t *, size_t, size_t *, uint32_t))h2_esp_ble_gatt_read,
    .gatt_write = (h2_pal_result_t (*)(void *, uint16_t, uint16_t, const uint8_t *, size_t, bool, uint32_t))h2_esp_ble_gatt_write,
    .gatt_subscribe = (h2_pal_result_t (*)(void *, uint16_t, const h2_pal_ble_gatt_subscribe_t *, uint32_t))h2_esp_ble_gatt_subscribe,
#else
    .start = (h2_pal_result_t (*)(void *))h2_esp_ble_unsupported_start,
    .stop = (h2_pal_result_t (*)(void *))h2_esp_ble_unsupported_stop,
    .adv_set_set_scan_response_data = (h2_pal_result_t (*)(void *, h2_pal_ble_adv_set_t *, const h2_pal_ble_adv_data_t *))h2_esp_ble_adv_set_set_scan_response_data_unsupported,
    .connect = (h2_pal_result_t (*)(void *, const h2_pal_ble_addr_t *, const h2_pal_ble_connect_params_t *, uint16_t *))h2_esp_ble_unsupported_connect,
    .update_connection = (h2_pal_result_t (*)(void *, uint16_t, const h2_pal_ble_connection_params_t *))h2_esp_ble_unsupported_update_connection,
    .exchange_mtu = (h2_pal_result_t (*)(void *, uint16_t, uint16_t *, uint32_t))h2_esp_ble_unsupported_exchange_mtu,
    .set_preferred_phy = (h2_pal_result_t (*)(void *, uint16_t, h2_pal_ble_phy_t, h2_pal_ble_phy_t, uint32_t))h2_esp_ble_unsupported_set_preferred_phy,
    .read_phy = (h2_pal_result_t (*)(void *, uint16_t, h2_pal_ble_phy_info_t *, uint32_t))h2_esp_ble_unsupported_read_phy,
    .gatt_discover = (h2_pal_result_t (*)(void *, uint16_t, const h2_pal_ble_gatt_discovery_request_t *, h2_pal_ble_gatt_discovery_entry_t *, size_t, size_t *, uint32_t))h2_esp_ble_unsupported_gatt_discover,
    .gatt_read = (h2_pal_result_t (*)(void *, uint16_t, uint16_t, uint16_t, uint8_t *, size_t, size_t *, uint32_t))h2_esp_ble_unsupported_gatt_read,
    .gatt_write = (h2_pal_result_t (*)(void *, uint16_t, uint16_t, const uint8_t *, size_t, bool, uint32_t))h2_esp_ble_unsupported_gatt_write,
    .gatt_subscribe = (h2_pal_result_t (*)(void *, uint16_t, const h2_pal_ble_gatt_subscribe_t *, uint32_t))h2_esp_ble_unsupported_gatt_subscribe,
    .indicate = (h2_pal_result_t (*)(void *, uint16_t, uint16_t, const uint8_t *, size_t, uint32_t))h2_esp_ble_unsupported_indicate,
#endif
};

static h2_pal_ble_t s_h2_esp_ble = {
    .user = &s_h2_esp_ble,
    .vtable = &s_h2_esp_ble_vtable,
    .allocator = NULL,
};

h2_pal_ble_t *h2_esp_platform_ble(void) {
    s_h2_esp_ble.allocator = h2_esp_platform_default_allocator();
    return &s_h2_esp_ble;
}
