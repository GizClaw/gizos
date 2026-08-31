#include "h2_bk_platform_core.h"
#include "h2_bk_ble_gatts_tx_tracker.h"

#include <common/bk_err.h>
#include <common/sys_config.h>

#if CONFIG_BLE
#include <components/bluetooth/bk_dm_bluetooth_types.h>
#include <components/bluetooth/bk_dm_gap_ble.h>
#include <components/bluetooth/bk_dm_gap_ble_types.h>
#include <components/bluetooth/bk_dm_gatt_types.h>
#include <components/bluetooth/bk_dm_gatt_common.h>
#include <components/bluetooth/bk_dm_gattc.h>
#include <components/bluetooth/bk_dm_gatts.h>
#include <components/bluetooth/bk_ble.h>
#include <private/dm_ble_gap_task.h>
#include <private/dm_bluetooth_task.h>
#include <os/os.h>
#include <string.h>

#define H2_BK_BLE_INVALID_ACTIVITY 0xffu
#define H2_BK_BLE_TIMEOUT_MS 5000u
#define H2_BK_BLE_ADV_INSTANCE 2u
#define H2_BK_BLE_ADV_SET_FIRST_INSTANCE 0u
#define H2_BK_BLE_ADV_SET_COUNT 2u
#define H2_BK_BLE_EXT_ADV_FRAGMENT_MAX_LEN 251u
#define H2_BK_BLE_EXT_ADV_DATA_INTERMEDIATE 0x00u
#define H2_BK_BLE_EXT_ADV_DATA_FIRST 0x01u
#define H2_BK_BLE_EXT_ADV_DATA_LAST 0x02u
#define H2_BK_BLE_EXT_ADV_DATA_COMPLETE 0x03u
#define H2_BK_BLE_AD_TYPE_SERVICE_DATA32 0x20u
#define H2_BK_BLE_AD_TYPE_SERVICE_DATA128 0x21u
/* Armino's public coded-scan mask is bit 1, but its HCI adapter consumes the
 * LE PHY bit layout where LE Coded is bit 2. Bit 1 selects the invalid LE 2M
 * scanning PHY and the controller rejects the complete parameter command. */
#define H2_BK_BLE_EXT_SCAN_CODED_MASK ((bk_ble_ext_scan_cfg_mask_t)(1u << 2))
#define H2_BK_BLE_MAX_SCAN_SERVICE_UUIDS 16u
#define H2_BK_BLE_LOCAL_MAX_MTU H2_PAL_BLE_ATT_MAX_MTU
#define H2_BK_BLE_MAX_VALUE_LEN H2_PAL_BLE_ATT_MAX_VALUE_LEN
#define H2_BK_BLE_MAX_DISCOVERY_ENTRIES 8u
#define H2_BK_BLE_MAX_GATT_SERVICES 4u
#define H2_BK_BLE_MAX_GATT_CHARACTERISTICS_PER_SERVICE 2u
#define H2_BK_BLE_MAX_GATT_CHARACTERISTICS \
    (H2_BK_BLE_MAX_GATT_SERVICES * H2_BK_BLE_MAX_GATT_CHARACTERISTICS_PER_SERVICE)
#define H2_BK_BLE_ATTR_COUNT \
    (1u + 2u * H2_BK_BLE_MAX_GATT_CHARACTERISTICS_PER_SERVICE)
#define H2_BK_BLE_NOTIFY_TIMEOUT_MS 1000u
#define H2_BK_BLE_LEGACY_NOTIFY_WINDOW 4u
#define H2_BK_BLE_GATTS_APP_ID 0x4832u
#define H2_BK_BLE_GATTC_APP_ID 0x4833u
#define H2_BK_BLE_LEGACY_PRF_ID 5u

struct h2_pal_ble_adv_set {
    h2_pal_ble_adv_params_t params;
    uint8_t instance;
    int allocated;
    int data_staged;
    int active;
};

#define H2_BK_GATT_ATTR_TYPE(iuuid) \
    ((bk_bt_uuid_t){ .len = BK_UUID_LEN_16, .uuid = { .uuid16 = (iuuid) } })
#define H2_BK_GATT_ATTR_VALUE(ilen, ivalue) \
    ((bk_attr_value_t){ .attr_max_len = (ilen), .attr_len = (ilen), .attr_value = (ivalue) })

static beken_semaphore_t s_h2_bk_ble_sem;
static beken_semaphore_t s_h2_bk_ble_notify_sem;
static beken_semaphore_t s_h2_bk_ble_indication_sem;
static beken_mutex_t s_h2_bk_ble_gatt_mutex;
static beken_mutex_t s_h2_bk_ble_adv_mutex;
static h2_pal_result_t s_h2_bk_ble_async_result;
static h2_pal_result_t s_h2_bk_ble_notify_result;
static h2_bk_ble_gatts_tx_tracker_t s_h2_bk_ble_gatts_tx = {
    .conn_handle = H2_PAL_BLE_INVALID_CONN_HANDLE,
    .attr_handle = H2_PAL_BLE_INVALID_ATTR_HANDLE,
};
static h2_pal_ble_pairing_config_t s_h2_bk_ble_pairing;
static volatile uint8_t s_h2_bk_ble_legacy_notify_in_flight;
static int s_h2_bk_ble_started;
static int s_h2_bk_ble_gap_registered;
static int s_h2_bk_ble_gatts_registered;
static int s_h2_bk_ble_gattc_registered;
static bk_gatt_if_t s_h2_bk_ble_gatts_if;
static bk_gatt_if_t s_h2_bk_ble_gattc_if;

static uint8_t s_h2_bk_ble_adv_data[H2_PAL_BLE_EXT_ADV_DATA_MAX_LEN];
static size_t s_h2_bk_ble_adv_data_len;
static int s_h2_bk_ble_adv_data_staged;
static uint8_t s_h2_bk_ble_legacy_adv_data[H2_PAL_BLE_LEGACY_ADV_DATA_MAX_LEN];
static size_t s_h2_bk_ble_legacy_adv_data_len;
static uint8_t s_h2_bk_ble_legacy_scan_rsp_data[H2_PAL_BLE_LEGACY_ADV_DATA_MAX_LEN];
static size_t s_h2_bk_ble_legacy_scan_rsp_data_len;
static int s_h2_bk_ble_legacy_adv_data_valid;
static uint8_t s_h2_bk_ble_adv_activity = H2_BK_BLE_INVALID_ACTIVITY;
static int s_h2_bk_ble_adv_created;
static int s_h2_bk_ble_adv_started;
static h2_pal_ble_adv_set_t s_h2_bk_ble_adv_sets[H2_BK_BLE_ADV_SET_COUNT];

static h2_pal_ble_scan_result_fn s_h2_bk_ble_scan_cb;
static void *s_h2_bk_ble_scan_user;
static uint8_t s_h2_bk_ble_scan_activity = H2_BK_BLE_INVALID_ACTIVITY;
static int s_h2_bk_ble_scan_created;
static int s_h2_bk_ble_scan_started;
static int s_h2_bk_ble_scan_extended;

static uint8_t s_h2_bk_ble_init_activity = H2_BK_BLE_INVALID_ACTIVITY;
static int s_h2_bk_ble_init_created;
static int s_h2_bk_ble_init_started;
static int s_h2_bk_ble_init_connected;

static bk_bt_uuid_t s_h2_bk_ble_service_uuid[H2_BK_BLE_MAX_GATT_SERVICES];
static bk_bt_uuid_t s_h2_bk_ble_char_uuid[H2_BK_BLE_MAX_GATT_CHARACTERISTICS];
static uint16_t s_h2_bk_ble_service_handle[H2_BK_BLE_MAX_GATT_SERVICES];
static uint16_t s_h2_bk_ble_value_handle[H2_BK_BLE_MAX_GATT_CHARACTERISTICS];
static uint16_t s_h2_bk_ble_cccd_handle[H2_BK_BLE_MAX_GATT_CHARACTERISTICS];
static uint16_t *s_h2_bk_ble_out_service_handle[H2_BK_BLE_MAX_GATT_SERVICES];
static uint16_t *s_h2_bk_ble_out_value_handle[H2_BK_BLE_MAX_GATT_CHARACTERISTICS];
static uint16_t *s_h2_bk_ble_out_cccd_handle[H2_BK_BLE_MAX_GATT_CHARACTERISTICS];
static uint8_t s_h2_bk_ble_value[H2_BK_BLE_MAX_GATT_CHARACTERISTICS][H2_BK_BLE_MAX_VALUE_LEN];
static uint8_t s_h2_bk_ble_read_scratch[H2_BK_BLE_MAX_VALUE_LEN];
static uint16_t s_h2_bk_ble_value_len[H2_BK_BLE_MAX_GATT_CHARACTERISTICS];
static uint16_t s_h2_bk_ble_value_max_len[H2_BK_BLE_MAX_GATT_CHARACTERISTICS];
static uint32_t s_h2_bk_ble_properties[H2_BK_BLE_MAX_GATT_CHARACTERISTICS];
static uint32_t s_h2_bk_ble_permissions[H2_BK_BLE_MAX_GATT_CHARACTERISTICS];
static h2_pal_ble_gatt_read_fn s_h2_bk_ble_read[H2_BK_BLE_MAX_GATT_CHARACTERISTICS];
static h2_pal_ble_gatt_write_fn s_h2_bk_ble_write[H2_BK_BLE_MAX_GATT_CHARACTERISTICS];
static void *s_h2_bk_ble_gatt_user[H2_BK_BLE_MAX_GATT_CHARACTERISTICS];
static size_t s_h2_bk_ble_service_characteristic_count[H2_BK_BLE_MAX_GATT_SERVICES];
static size_t s_h2_bk_ble_service_count;
static int s_h2_bk_ble_service_started[H2_BK_BLE_MAX_GATT_SERVICES];
static int s_h2_bk_ble_gatt_attached;

static h2_pal_ble_gatt_discovery_request_t s_h2_bk_ble_discovery_request;
static h2_pal_ble_gatt_discovery_entry_t *s_h2_bk_ble_discovery_entries;
static size_t s_h2_bk_ble_discovery_max_entries;
static size_t s_h2_bk_ble_discovery_count;
static uint8_t s_h2_bk_ble_discovery_uuid_data[H2_BK_BLE_MAX_DISCOVERY_ENTRIES][16];

static uint8_t *s_h2_bk_ble_read_out;
static size_t s_h2_bk_ble_read_out_size;
static size_t s_h2_bk_ble_read_out_len;
static uint16_t s_h2_bk_ble_pending_handle;
static int s_h2_bk_ble_waiting_write;

static uint16_t s_h2_bk_ble_conn_handle = H2_PAL_BLE_INVALID_CONN_HANDLE;
static uint16_t s_h2_bk_ble_peripheral_conn_handle =
    H2_PAL_BLE_INVALID_CONN_HANDLE;
static uint16_t s_h2_bk_ble_pair_conn_handle =
    H2_PAL_BLE_INVALID_CONN_HANDLE;
static uint16_t s_h2_bk_ble_hci_handle = H2_PAL_BLE_INVALID_CONN_HANDLE;
static uint16_t s_h2_bk_ble_peripheral_hci_handle =
    H2_PAL_BLE_INVALID_CONN_HANDLE;
static bk_bd_addr_t s_h2_bk_ble_peer_addr;
static bk_bd_addr_t s_h2_bk_ble_peripheral_peer_addr;
static h2_pal_ble_addr_type_t s_h2_bk_ble_peer_addr_type = H2_PAL_BLE_ADDR_TYPE_PUBLIC;
static h2_pal_ble_addr_type_t s_h2_bk_ble_peripheral_peer_addr_type =
    H2_PAL_BLE_ADDR_TYPE_PUBLIC;
static h2_pal_result_t s_h2_bk_ble_connect_result;
static int s_h2_bk_ble_gattc_connected;
static uint16_t s_h2_bk_ble_last_mtu;
static h2_pal_ble_phy_info_t s_h2_bk_ble_last_phy;

static uint8_t s_h2_bk_ble_cccd_value[H2_BK_BLE_MAX_GATT_CHARACTERISTICS][2];
static uint16_t s_h2_bk_ble_attr_handles[H2_BK_BLE_MAX_GATT_SERVICES][H2_BK_BLE_ATTR_COUNT];
static bk_gatts_attr_db_t s_h2_bk_ble_attr_db[H2_BK_BLE_MAX_GATT_SERVICES][H2_BK_BLE_ATTR_COUNT];
static uint8_t s_h2_bk_ble_value_attr_index[H2_BK_BLE_MAX_GATT_SERVICES][H2_BK_BLE_MAX_GATT_CHARACTERISTICS_PER_SERVICE];
static uint8_t s_h2_bk_ble_cccd_attr_index[H2_BK_BLE_MAX_GATT_SERVICES][H2_BK_BLE_MAX_GATT_CHARACTERISTICS_PER_SERVICE];
static uint8_t s_h2_bk_ble_attr_db_count[H2_BK_BLE_MAX_GATT_SERVICES];
static size_t s_h2_bk_ble_pending_service_index = H2_BK_BLE_MAX_GATT_SERVICES;

typedef enum h2_bk_ble_legacy_attr_index {
    H2_BK_BLE_LEGACY_IDX_SVC = 0,
    H2_BK_BLE_LEGACY_IDX_CHAR0_DECL = 1,
    H2_BK_BLE_LEGACY_IDX_CHAR0_VALUE = 2,
    H2_BK_BLE_LEGACY_IDX_CHAR0_CCCD = 3,
    H2_BK_BLE_LEGACY_IDX_CHAR1_DECL = 4,
    H2_BK_BLE_LEGACY_IDX_CHAR1_VALUE = 5,
    H2_BK_BLE_LEGACY_IDX_CHAR1_CCCD = 6,
    H2_BK_BLE_LEGACY_IDX_COUNT = 7,
} h2_bk_ble_legacy_attr_index_t;

static ble_attm_desc_t s_h2_bk_ble_legacy_attr_db[H2_BK_BLE_LEGACY_IDX_COUNT];
static struct bk_ble_db_cfg s_h2_bk_ble_legacy_db_cfg;
static int s_h2_bk_ble_legacy_service_created;

static h2_pal_result_t h2_bk_ble_map_error(int err) {
    switch (err) {
    case BK_OK:
        return H2_PAL_OK;
    case BK_ERR_PARAM:
    case BK_ERR_NULL_PARAM:
        return H2_PAL_ERR_INVALID_ARG;
    case BK_ERR_NO_MEM:
        return H2_PAL_ERR_NO_MEMORY;
    case BK_ERR_TIMEOUT:
        return H2_PAL_ERR_TIMEOUT;
    case BK_ERR_NOT_SUPPORT:
        return H2_PAL_ERR_UNSUPPORTED;
    case BK_ERR_BUSY:
    case BK_ERR_IN_PROGRESS:
    case BK_ERR_STATE:
        return H2_PAL_ERR_INVALID_STATE;
    default:
        return H2_PAL_ERR_IO;
    }
}

static h2_pal_result_t h2_bk_ble_map_gatt_write_error(
    int err,
    bool with_response) {
    if (!with_response &&
        (err == BK_ERR_BLE_BLE_STATUS || err == BK_ERR_NO_MEM ||
         err == BK_ERR_BUSY || err == BK_ERR_IN_PROGRESS)) {
        /* The legacy stack reports BLE_STATUS until the connected GATT path
         * is ready and NO_MEM while its command mailbox is full.  A write
         * command has not been queued in either case, so the stream may retry
         * it as normal backpressure. */
        return H2_PAL_ERR_WOULD_BLOCK;
    }
    return h2_bk_ble_map_error(err);
}

static h2_pal_result_t h2_bk_ble_ensure_gatt_mutex(void);
static void h2_bk_ble_signal_notify(h2_pal_result_t result);
static void h2_bk_ble_signal_indication(void);

static void h2_bk_ble_post(h2_pal_system_event_type_t type, const void *payload, size_t payload_size) {
    h2_pal_system_event_t event;
    memset(&event, 0, sizeof(event));
    event.type = type;
    event.payload = payload;
    event.payload_size = payload_size;
    (void)h2_pal_system_event_post(h2_bk_platform_system_event_api(), &event, 0u);
}

static void h2_bk_ble_finish_gatts_tx_unlocked(
    uint16_t conn_handle,
    h2_pal_result_t result) {
    h2_bk_ble_gatts_tx_kind_t kind = h2_bk_ble_gatts_tx_tracker_finish(
        &s_h2_bk_ble_gatts_tx, conn_handle, result);

    if (kind == H2_BK_BLE_GATTS_TX_NOTIFY) {
        h2_bk_ble_signal_notify(result);
    } else if (kind == H2_BK_BLE_GATTS_TX_INDICATE_WAITING) {
        h2_bk_ble_signal_indication();
    }
}

static h2_pal_result_t h2_bk_ble_begin_gatts_tx(
    h2_bk_ble_gatts_tx_kind_t kind,
    uint16_t conn_handle,
    uint16_t attr_handle) {
    h2_pal_result_t result = h2_bk_ble_ensure_gatt_mutex();
    if (result != H2_PAL_OK) {
        return result;
    }
    if (rtos_lock_mutex(&s_h2_bk_ble_gatt_mutex) != kNoErr) {
        return H2_PAL_ERR_IO;
    }
    result = h2_bk_ble_gatts_tx_tracker_begin(
        &s_h2_bk_ble_gatts_tx, kind, conn_handle, attr_handle);
    (void)rtos_unlock_mutex(&s_h2_bk_ble_gatt_mutex);
    return result;
}

static void h2_bk_ble_cancel_gatts_tx_submission(
    uint16_t conn_handle,
    uint16_t attr_handle) {
    if (s_h2_bk_ble_gatt_mutex == NULL ||
        rtos_lock_mutex(&s_h2_bk_ble_gatt_mutex) != kNoErr) {
        return;
    }
    h2_bk_ble_gatts_tx_tracker_cancel_submission(
        &s_h2_bk_ble_gatts_tx, conn_handle, attr_handle);
    (void)rtos_unlock_mutex(&s_h2_bk_ble_gatt_mutex);
}

static void h2_bk_ble_post_adv_set(
    h2_pal_system_event_type_t type,
    h2_pal_ble_adv_set_t *set,
    h2_pal_result_t status) {
    h2_pal_ble_adv_set_event_t payload = {
        .set = set,
        .status = status,
    };
    h2_bk_ble_post(type, &payload, sizeof(payload));
}

static void h2_bk_ble_mark_connectable_advertising_stopped(void) {
    for (size_t i = 0u; i < H2_BK_BLE_ADV_SET_COUNT; ++i) {
        h2_pal_ble_adv_set_t *set = &s_h2_bk_ble_adv_sets[i];
        if (set->allocated && set->active &&
            set->params.mode == H2_PAL_BLE_ADV_MODE_CONNECTABLE) {
            set->active = 0;
            h2_bk_ble_post_adv_set(
                H2_PAL_SYSTEM_EVENT_TYPE_BLE_ADVERTISING_STOPPED,
                set,
                H2_PAL_OK);
        }
    }
}

static int h2_bk_ble_adv_set_valid(const h2_pal_ble_adv_set_t *set) {
    uintptr_t address = (uintptr_t)set;
    return address >= (uintptr_t)&s_h2_bk_ble_adv_sets[0] &&
           address < (uintptr_t)&s_h2_bk_ble_adv_sets[H2_BK_BLE_ADV_SET_COUNT] &&
           (address - (uintptr_t)&s_h2_bk_ble_adv_sets[0]) %
                   sizeof(s_h2_bk_ble_adv_sets[0]) ==
               0u &&
           set->allocated;
}

static h2_pal_ble_adv_set_t *h2_bk_ble_adv_set_for_instance(uint8_t instance) {
    for (size_t i = 0u; i < H2_BK_BLE_ADV_SET_COUNT; ++i) {
        h2_pal_ble_adv_set_t *set = &s_h2_bk_ble_adv_sets[i];
        if (set->allocated && set->instance == instance) {
            return set;
        }
    }
    return NULL;
}

static void h2_bk_ble_post_mtu_changed(uint16_t conn_handle, uint16_t mtu) {
    s_h2_bk_ble_last_mtu = mtu;
    os_printf("H2_BK_BLE_MTU_CHANGED conn=%u mtu=%u\n", (unsigned)conn_handle, (unsigned)mtu);

    h2_pal_ble_mtu_info_t info;
    memset(&info, 0, sizeof(info));
    info.conn_handle = conn_handle;
    info.mtu = mtu;
    h2_bk_ble_post(H2_PAL_SYSTEM_EVENT_TYPE_BLE_MTU_CHANGED, &info, sizeof(info));
}

static void h2_bk_ble_post_subscription_changed(
    uint16_t conn_handle,
    size_t index,
    const uint8_t *cccd,
    size_t len) {
    uint16_t value = 0u;
    if (cccd != NULL && len > 0u) {
        value = cccd[0];
        if (len > 1u) {
            value |= (uint16_t)cccd[1] << 8;
        }
    }

    h2_pal_ble_subscription_state_t state;
    memset(&state, 0, sizeof(state));
    state.conn_handle = conn_handle;
    state.value_handle = s_h2_bk_ble_value_handle[index];
    state.enabled = (value & 0x0003u) != 0u;
    state.mode = (value & 0x0002u) != 0u
                     ? H2_PAL_BLE_SUBSCRIBE_MODE_INDICATE
                     : H2_PAL_BLE_SUBSCRIBE_MODE_NOTIFY;
    h2_bk_ble_post(H2_PAL_SYSTEM_EVENT_TYPE_BLE_SUBSCRIPTION_CHANGED, &state, sizeof(state));
}

static size_t h2_bk_ble_service_characteristic_first(size_t service_index) {
    return service_index * H2_BK_BLE_MAX_GATT_CHARACTERISTICS_PER_SERVICE;
}

static void h2_bk_ble_update_out_handles(void) {
    for (size_t service_index = 0u;
         service_index < s_h2_bk_ble_service_count;
         ++service_index) {
        if (s_h2_bk_ble_out_service_handle[service_index] != NULL) {
            *s_h2_bk_ble_out_service_handle[service_index] =
                s_h2_bk_ble_service_handle[service_index];
        }
    }
    for (size_t service_index = 0u;
         service_index < s_h2_bk_ble_service_count;
         ++service_index) {
        size_t first = h2_bk_ble_service_characteristic_first(service_index);
        for (size_t i = 0u;
             i < s_h2_bk_ble_service_characteristic_count[service_index];
             ++i) {
            size_t index = first + i;
            if (s_h2_bk_ble_out_value_handle[index] != NULL) {
                *s_h2_bk_ble_out_value_handle[index] =
                    s_h2_bk_ble_value_handle[index];
            }
            if (s_h2_bk_ble_out_cccd_handle[index] != NULL) {
                *s_h2_bk_ble_out_cccd_handle[index] =
                    s_h2_bk_ble_cccd_handle[index];
            }
        }
    }
}

static void h2_bk_ble_clear_out_handle_ptrs(void) {
    for (size_t i = 0u; i < H2_BK_BLE_MAX_GATT_SERVICES; ++i) {
        s_h2_bk_ble_out_service_handle[i] = NULL;
    }
    for (size_t i = 0u; i < H2_BK_BLE_MAX_GATT_CHARACTERISTICS; ++i) {
        s_h2_bk_ble_out_value_handle[i] = NULL;
        s_h2_bk_ble_out_cccd_handle[i] = NULL;
    }
}

static void h2_bk_ble_rollback_service_slot(size_t service_index) {
    if (service_index >= s_h2_bk_ble_service_count ||
        service_index + 1u != s_h2_bk_ble_service_count) {
        return;
    }
    if (s_h2_bk_ble_out_service_handle[service_index] != NULL) {
        *s_h2_bk_ble_out_service_handle[service_index] =
            H2_PAL_BLE_INVALID_ATTR_HANDLE;
    }
    size_t first = h2_bk_ble_service_characteristic_first(service_index);
    for (size_t i = 0u;
         i < s_h2_bk_ble_service_characteristic_count[service_index];
         ++i) {
        size_t index = first + i;
        if (s_h2_bk_ble_out_value_handle[index] != NULL) {
            *s_h2_bk_ble_out_value_handle[index] =
                H2_PAL_BLE_INVALID_ATTR_HANDLE;
        }
        if (s_h2_bk_ble_out_cccd_handle[index] != NULL) {
            *s_h2_bk_ble_out_cccd_handle[index] =
                H2_PAL_BLE_INVALID_ATTR_HANDLE;
        }
    }
    memset(&s_h2_bk_ble_service_uuid[service_index], 0,
        sizeof(s_h2_bk_ble_service_uuid[service_index]));
    s_h2_bk_ble_service_handle[service_index] =
        H2_PAL_BLE_INVALID_ATTR_HANDLE;
    s_h2_bk_ble_out_service_handle[service_index] = NULL;
    s_h2_bk_ble_service_started[service_index] = 0;
    s_h2_bk_ble_service_characteristic_count[service_index] = 0u;
    memset(s_h2_bk_ble_attr_handles[service_index], 0,
        sizeof(s_h2_bk_ble_attr_handles[service_index]));
    memset(s_h2_bk_ble_attr_db[service_index], 0,
        sizeof(s_h2_bk_ble_attr_db[service_index]));
    memset(s_h2_bk_ble_value_attr_index[service_index], 0,
        sizeof(s_h2_bk_ble_value_attr_index[service_index]));
    memset(s_h2_bk_ble_cccd_attr_index[service_index], 0,
        sizeof(s_h2_bk_ble_cccd_attr_index[service_index]));
    s_h2_bk_ble_attr_db_count[service_index] = 0u;
    for (size_t i = 0u;
         i < H2_BK_BLE_MAX_GATT_CHARACTERISTICS_PER_SERVICE;
         ++i) {
        size_t index = first + i;
        memset(&s_h2_bk_ble_char_uuid[index], 0,
            sizeof(s_h2_bk_ble_char_uuid[index]));
        s_h2_bk_ble_value_handle[index] = H2_PAL_BLE_INVALID_ATTR_HANDLE;
        s_h2_bk_ble_cccd_handle[index] = H2_PAL_BLE_INVALID_ATTR_HANDLE;
        s_h2_bk_ble_out_value_handle[index] = NULL;
        s_h2_bk_ble_out_cccd_handle[index] = NULL;
        memset(s_h2_bk_ble_value[index], 0,
            sizeof(s_h2_bk_ble_value[index]));
        memset(s_h2_bk_ble_cccd_value[index], 0,
            sizeof(s_h2_bk_ble_cccd_value[index]));
        s_h2_bk_ble_value_len[index] = 0u;
        s_h2_bk_ble_value_max_len[index] = 0u;
        s_h2_bk_ble_properties[index] = 0u;
        s_h2_bk_ble_permissions[index] = 0u;
        s_h2_bk_ble_read[index] = NULL;
        s_h2_bk_ble_write[index] = NULL;
        s_h2_bk_ble_gatt_user[index] = NULL;
    }
    --s_h2_bk_ble_service_count;
    if (s_h2_bk_ble_service_count == 0u) {
        s_h2_bk_ble_gatt_attached = 0;
    }
}

static int h2_bk_ble_value_index(uint16_t handle) {
    for (size_t service_index = 0u;
         service_index < s_h2_bk_ble_service_count;
         ++service_index) {
        size_t first = h2_bk_ble_service_characteristic_first(service_index);
        for (size_t i = 0u;
             i < s_h2_bk_ble_service_characteristic_count[service_index];
             ++i) {
            size_t index = first + i;
            if (s_h2_bk_ble_value_handle[index] == handle) return (int)index;
        }
    }
    return -1;
}

static int h2_bk_ble_cccd_index(uint16_t handle) {
    for (size_t service_index = 0u;
         service_index < s_h2_bk_ble_service_count;
         ++service_index) {
        size_t first = h2_bk_ble_service_characteristic_first(service_index);
        for (size_t i = 0u;
             i < s_h2_bk_ble_service_characteristic_count[service_index];
             ++i) {
            size_t index = first + i;
            if (s_h2_bk_ble_cccd_handle[index] == handle) return (int)index;
        }
    }
    return -1;
}

static int h2_bk_ble_legacy_value_index(uint16_t att_index) {
    if (att_index == H2_BK_BLE_LEGACY_IDX_CHAR0_VALUE) return 0;
    if (att_index == H2_BK_BLE_LEGACY_IDX_CHAR1_VALUE &&
        s_h2_bk_ble_service_characteristic_count[0] > 1u) return 1;
    return -1;
}

static int h2_bk_ble_legacy_cccd_index(uint16_t att_index) {
    if (att_index == H2_BK_BLE_LEGACY_IDX_CHAR0_CCCD) return 0;
    if (att_index == H2_BK_BLE_LEGACY_IDX_CHAR1_CCCD &&
        s_h2_bk_ble_service_characteristic_count[0] > 1u) return 1;
    return -1;
}

static h2_pal_result_t h2_bk_ble_ensure_sem(void) {
    if (s_h2_bk_ble_sem != NULL) {
        return H2_PAL_OK;
    }
    return rtos_init_semaphore(&s_h2_bk_ble_sem, 1) == kNoErr ? H2_PAL_OK : H2_PAL_ERR_NO_MEMORY;
}

static h2_pal_result_t h2_bk_ble_ensure_gatt_mutex(void) {
    if (s_h2_bk_ble_gatt_mutex != NULL) return H2_PAL_OK;
    return rtos_init_mutex(&s_h2_bk_ble_gatt_mutex) == kNoErr
               ? H2_PAL_OK
               : H2_PAL_ERR_NO_MEMORY;
}

static h2_pal_result_t h2_bk_ble_ensure_adv_mutex(void) {
    if (s_h2_bk_ble_adv_mutex != NULL) return H2_PAL_OK;
    return rtos_init_mutex(&s_h2_bk_ble_adv_mutex) == kNoErr
               ? H2_PAL_OK
               : H2_PAL_ERR_NO_MEMORY;
}

static h2_pal_result_t h2_bk_ble_lock_adv(void) {
    return s_h2_bk_ble_adv_mutex != NULL &&
                   rtos_lock_mutex(&s_h2_bk_ble_adv_mutex) == kNoErr
               ? H2_PAL_OK
               : H2_PAL_ERR_INVALID_STATE;
}

static void h2_bk_ble_unlock_adv(void) {
    if (s_h2_bk_ble_adv_mutex != NULL) {
        (void)rtos_unlock_mutex(&s_h2_bk_ble_adv_mutex);
    }
}

static void h2_bk_ble_signal(h2_pal_result_t result) {
    s_h2_bk_ble_async_result = result;
    if (s_h2_bk_ble_sem != NULL) {
        (void)rtos_set_semaphore(&s_h2_bk_ble_sem);
    }
}

static h2_pal_result_t h2_bk_ble_wait(uint32_t timeout_ms) {
    if (s_h2_bk_ble_sem == NULL) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    uint32_t wait_ms = timeout_ms == 0u ? H2_BK_BLE_TIMEOUT_MS : timeout_ms;
    if (rtos_get_semaphore(&s_h2_bk_ble_sem, wait_ms) != kNoErr) {
        return H2_PAL_ERR_TIMEOUT;
    }
    return s_h2_bk_ble_async_result;
}

static void h2_bk_ble_drain_signals(void) {
    if (s_h2_bk_ble_sem == NULL) {
        return;
    }
    while (rtos_get_semaphore(&s_h2_bk_ble_sem, 0) == kNoErr) {
    }
}

static h2_pal_result_t h2_bk_ble_ensure_notify_sem(void) {
    if (s_h2_bk_ble_notify_sem != NULL) {
        return H2_PAL_OK;
    }
    return rtos_init_semaphore(&s_h2_bk_ble_notify_sem, 1) == kNoErr ? H2_PAL_OK : H2_PAL_ERR_NO_MEMORY;
}

static h2_pal_result_t h2_bk_ble_ensure_indication_sem(void) {
    if (s_h2_bk_ble_indication_sem != NULL) {
        return H2_PAL_OK;
    }
    return rtos_init_semaphore(&s_h2_bk_ble_indication_sem, 1) == kNoErr
               ? H2_PAL_OK
               : H2_PAL_ERR_NO_MEMORY;
}

static void h2_bk_ble_drain_indication_signals(void) {
    if (s_h2_bk_ble_indication_sem == NULL) {
        return;
    }
    while (rtos_get_semaphore(&s_h2_bk_ble_indication_sem, 0) == kNoErr) {
    }
}

static void h2_bk_ble_signal_indication(void) {
    if (s_h2_bk_ble_indication_sem != NULL) {
        (void)rtos_set_semaphore(&s_h2_bk_ble_indication_sem);
    }
}

static void h2_bk_ble_drain_notify_signals(void) {
    if (s_h2_bk_ble_notify_sem == NULL) {
        return;
    }
    while (rtos_get_semaphore(&s_h2_bk_ble_notify_sem, 0) == kNoErr) {
    }
}

static void h2_bk_ble_signal_notify(h2_pal_result_t result) {
    s_h2_bk_ble_notify_result = result;
    if (s_h2_bk_ble_notify_sem != NULL) {
        (void)rtos_set_semaphore(&s_h2_bk_ble_notify_sem);
    }
}

static h2_pal_result_t h2_bk_ble_wait_notify(uint32_t timeout_ms) {
    if (s_h2_bk_ble_notify_sem == NULL) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    uint32_t wait_ms = timeout_ms == 0u ? H2_BK_BLE_NOTIFY_TIMEOUT_MS : timeout_ms;
    if (rtos_get_semaphore(&s_h2_bk_ble_notify_sem, wait_ms) != kNoErr) {
        return H2_PAL_ERR_TIMEOUT;
    }
    return s_h2_bk_ble_notify_result;
}

static h2_pal_result_t h2_bk_ble_wait_indication(uint32_t timeout_ms) {
    if (s_h2_bk_ble_indication_sem == NULL) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    uint32_t wait_ms = timeout_ms == UINT32_MAX ?
        BEKEN_WAIT_FOREVER : timeout_ms;
    bool signaled =
        rtos_get_semaphore(&s_h2_bk_ble_indication_sem, wait_ms) == kNoErr;
    if (rtos_lock_mutex(&s_h2_bk_ble_gatt_mutex) != kNoErr) {
        return H2_PAL_ERR_IO;
    }
    h2_pal_result_t result = signaled ?
        h2_bk_ble_gatts_tx_tracker_take(&s_h2_bk_ble_gatts_tx) :
        h2_bk_ble_gatts_tx_tracker_timeout(&s_h2_bk_ble_gatts_tx);
    (void)rtos_unlock_mutex(&s_h2_bk_ble_gatt_mutex);
    return result;
}

static h2_pal_result_t h2_bk_ble_wait_legacy_notify_slot(void) {
    h2_pal_result_t rc = h2_bk_ble_ensure_notify_sem();
    if (rc != H2_PAL_OK) {
        return rc;
    }
    while (s_h2_bk_ble_legacy_notify_in_flight >= H2_BK_BLE_LEGACY_NOTIFY_WINDOW) {
        rc = h2_bk_ble_wait_notify(H2_BK_BLE_NOTIFY_TIMEOUT_MS);
        if (rc != H2_PAL_OK) {
            return rc;
        }
    }
    return H2_PAL_OK;
}

static void h2_bk_ble_signal_legacy_notify_done(h2_pal_result_t result) {
    if (s_h2_bk_ble_legacy_notify_in_flight > 0u) {
        s_h2_bk_ble_legacy_notify_in_flight--;
    }
    h2_bk_ble_signal_notify(result);
}

static uint16_t h2_bk_ble_ms_to_units625(uint32_t ms) {
    uint32_t units = (ms * 1600u) / 1000u;
    if (units < 4u) {
        units = 4u;
    }
    if (units > 0x4000u) {
        units = 0x4000u;
    }
    return (uint16_t)units;
}

static uint32_t h2_bk_ble_ms_to_ext_units625(uint32_t ms) {
    return (uint32_t)(((uint64_t)ms * 1600u) / 1000u);
}

static uint16_t h2_bk_ble_ms_to_units1250(uint32_t ms) {
    uint32_t units = (ms * 800u) / 1000u;
    if (units < 6u) {
        units = 6u;
    }
    if (units > 0x0c80u) {
        units = 0x0c80u;
    }
    return (uint16_t)units;
}

static uint16_t h2_bk_ble_ms_to_units10(uint32_t ms) {
    uint32_t units = ms / 10u;
    if (units < 10u) {
        units = 10u;
    }
    if (units > 0x0c80u) {
        units = 0x0c80u;
    }
    return (uint16_t)units;
}

static uint16_t h2_bk_ble_uuid16_value(const h2_pal_ble_uuid_t *uuid) {
    if (uuid == NULL || uuid->data == NULL || uuid->len != 2u) {
        return 0u;
    }
    return (uint16_t)uuid->data[0] | ((uint16_t)uuid->data[1] << 8);
}

static bool h2_bk_ble_uuid_from_pal(
    const h2_pal_ble_uuid_t *uuid,
    bk_bt_uuid_t *out) {
    if (uuid == NULL || out == NULL || uuid->data == NULL ||
        (uuid->len != BK_UUID_LEN_16 && uuid->len != BK_UUID_LEN_32 &&
         uuid->len != BK_UUID_LEN_128)) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->len = (uint16_t)uuid->len;
    /* PAL and Armino both represent 128-bit UUIDs in ATT wire order. */
    memcpy(out->uuid.uuid128, uuid->data, uuid->len);
    return true;
}

static bool h2_bk_ble_uuid_equal(
    const bk_bt_uuid_t *lhs,
    const bk_bt_uuid_t *rhs) {
    return lhs->len == rhs->len &&
           memcmp(lhs->uuid.uuid128, rhs->uuid.uuid128, lhs->len) == 0;
}

static void h2_bk_ble_copy_uuid16(uint16_t value, h2_pal_ble_uuid_t *uuid, uint8_t *storage) {
    storage[0] = (uint8_t)(value & 0xffu);
    storage[1] = (uint8_t)(value >> 8);
    uuid->data = storage;
    uuid->len = 2u;
}

static uint16_t h2_bk_ble_raw_uuid16(const uint8_t *uuid, size_t len) {
    if (uuid == NULL || len != 2u) {
        return 0u;
    }
    return (uint16_t)uuid[0] | ((uint16_t)uuid[1] << 8);
}

static uint32_t h2_bk_ble_gatt_properties_from_bk(uint32_t properties) {
    uint32_t result = 0u;
    if ((properties & BK_GATT_CHAR_PROP_BIT_READ) != 0u) {
        result |= H2_PAL_BLE_GATT_PROPERTY_READ;
    }
    if ((properties & BK_GATT_CHAR_PROP_BIT_WRITE) != 0u) {
        result |= H2_PAL_BLE_GATT_PROPERTY_WRITE;
    }
    if ((properties & BK_GATT_CHAR_PROP_BIT_WRITE_NR) != 0u) {
        result |= H2_PAL_BLE_GATT_PROPERTY_WRITE_NO_RSP;
    }
    if ((properties & BK_GATT_CHAR_PROP_BIT_NOTIFY) != 0u) {
        result |= H2_PAL_BLE_GATT_PROPERTY_NOTIFY;
    }
    if ((properties & BK_GATT_CHAR_PROP_BIT_INDICATE) != 0u) {
        result |= H2_PAL_BLE_GATT_PROPERTY_INDICATE;
    }
    return result;
}

static bk_ble_addr_type_t h2_bk_ble_to_bk_addr_type(h2_pal_ble_addr_type_t type) {
    return type == H2_PAL_BLE_ADDR_TYPE_RANDOM ? BLE_ADDR_TYPE_RANDOM : BLE_ADDR_TYPE_PUBLIC;
}

static h2_pal_ble_addr_type_t h2_bk_ble_addr_type(uint8_t type) {
    switch (type) {
    case BLE_ADDR_TYPE_PUBLIC:
        return H2_PAL_BLE_ADDR_TYPE_PUBLIC;
    case BLE_ADDR_TYPE_RANDOM:
        return H2_PAL_BLE_ADDR_TYPE_RANDOM;
    default:
        return H2_PAL_BLE_ADDR_TYPE_UNKNOWN;
    }
}

static int h2_bk_ble_adv_put(
    uint8_t *out,
    size_t *len,
    size_t capacity,
    uint8_t type,
    const uint8_t *data,
    size_t data_len) {
    if (data_len > 254u || (data_len > 0u && data == NULL) ||
        capacity < data_len + 2u || *len > capacity - data_len - 2u) {
        return 0;
    }
    out[(*len)++] = (uint8_t)(data_len + 1u);
    out[(*len)++] = type;
    if (data_len > 0u) {
        memcpy(&out[*len], data, data_len);
        *len += data_len;
    }
    return 1;
}

static int h2_bk_ble_adv_put_uuid_list(
    uint8_t *out,
    size_t *out_len,
    size_t capacity,
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
    if (capacity < list_len + 2u || *out_len > capacity - list_len - 2u) {
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

static h2_pal_result_t h2_bk_ble_encode_adv_data(
    const h2_pal_ble_adv_data_t *data,
    uint8_t *out,
    size_t capacity,
    size_t *out_len,
    uint8_t *scan_rsp,
    size_t scan_rsp_capacity,
    size_t *scan_rsp_len) {
    uint8_t flags = 0x06u;
    size_t len = 0u;
    size_t response_len = 0u;
    if (data == NULL || out == NULL || out_len == NULL ||
        (data->service_uuid_count > 0u && data->service_uuids == NULL) ||
        (data->manufacturer_data.len > 0u && data->manufacturer_data.data == NULL) ||
        (data->service_data_uuid.len > 0u && data->service_data_uuid.data == NULL) ||
        (data->service_data.len > 0u && data->service_data.data == NULL)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (!h2_bk_ble_adv_put(out, &len, capacity, BK_BLE_AD_TYPE_FLAG, &flags, sizeof(flags))) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    for (size_t i = 0u; i < data->service_uuid_count; ++i) {
        const h2_pal_ble_uuid_t *uuid = &data->service_uuids[i];
        if ((uuid->len != 2u && uuid->len != 4u && uuid->len != 16u) || uuid->data == NULL) {
            return H2_PAL_ERR_INVALID_ARG;
        }
    }
    if (!h2_bk_ble_adv_put_uuid_list(
            out, &len, capacity, data, 2u, BK_BLE_AD_TYPE_16SRV_CMPL) ||
        !h2_bk_ble_adv_put_uuid_list(
            out, &len, capacity, data, 4u, BK_BLE_AD_TYPE_32SRV_CMPL) ||
        !h2_bk_ble_adv_put_uuid_list(
            out, &len, capacity, data, 16u, BK_BLE_AD_TYPE_128SRV_CMPL)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (data->manufacturer_data.len > 0u) {
        if (!h2_bk_ble_adv_put(
                out,
                &len,
                capacity,
                BK_BLE_AD_TYPE_MANU,
                data->manufacturer_data.data,
                data->manufacturer_data.len)) {
            if (scan_rsp == NULL || scan_rsp_len == NULL ||
                !h2_bk_ble_adv_put(
                    scan_rsp,
                    &response_len,
                    scan_rsp_capacity,
                    BK_BLE_AD_TYPE_MANU,
                    data->manufacturer_data.data,
                    data->manufacturer_data.len)) {
                return H2_PAL_ERR_INVALID_ARG;
            }
        }
    }
    if (data->service_data.len > 0u) {
        uint8_t service_data[254u];
        size_t uuid_len = data->service_data_uuid.len;
        uint8_t type = BK_BLE_AD_TYPE_SERVICE_DATA;
        if (uuid_len != 0u && uuid_len != 2u && uuid_len != 4u && uuid_len != 16u) {
            return H2_PAL_ERR_INVALID_ARG;
        }
        if (uuid_len + data->service_data.len > sizeof(service_data)) {
            return H2_PAL_ERR_INVALID_ARG;
        }
        if (uuid_len == 4u) type = H2_BK_BLE_AD_TYPE_SERVICE_DATA32;
        if (uuid_len == 16u) type = H2_BK_BLE_AD_TYPE_SERVICE_DATA128;
        if (uuid_len > 0u) memcpy(service_data, data->service_data_uuid.data, uuid_len);
        memcpy(service_data + uuid_len, data->service_data.data, data->service_data.len);
        if (!h2_bk_ble_adv_put(
                out, &len, capacity, type, service_data,
                uuid_len + data->service_data.len)) {
            return H2_PAL_ERR_INVALID_ARG;
        }
    }
    if (data->local_name != NULL) {
        size_t name_len = strlen(data->local_name);
        if (!h2_bk_ble_adv_put(
                out,
                &len,
                capacity,
                BK_BLE_AD_TYPE_NAME_CMPL,
                (const uint8_t *)data->local_name,
                name_len)) {
            if (scan_rsp == NULL || scan_rsp_len == NULL ||
                !h2_bk_ble_adv_put(
                    scan_rsp,
                    &response_len,
                    scan_rsp_capacity,
                    BK_BLE_AD_TYPE_NAME_CMPL,
                    (const uint8_t *)data->local_name,
                    name_len)) {
                return H2_PAL_ERR_INVALID_ARG;
            }
        }
    }
    *out_len = len;
    if (scan_rsp_len != NULL) {
        *scan_rsp_len = response_len;
    }
    return H2_PAL_OK;
}

static void h2_bk_ble_parse_adv_data(
    const uint8_t *data,
    size_t len,
    char *name,
    size_t name_size,
    h2_pal_ble_uuid_t *service_uuids,
    uint8_t service_uuid_data[][16],
    size_t service_uuid_capacity,
    h2_pal_ble_scan_result_t *result) {
    size_t off = 0u;
    while (off < len) {
        uint8_t field_len = data[off++];
        if (field_len == 0u || off + field_len > len) {
            break;
        }
        uint8_t type = data[off++];
        size_t value_len = (size_t)field_len - 1u;
        const uint8_t *value = &data[off];
        size_t service_uuid_len = 0u;
        if (type == BK_BLE_AD_TYPE_16SRV_PART ||
            type == BK_BLE_AD_TYPE_16SRV_CMPL) {
            service_uuid_len = 2u;
        } else if (type == BK_BLE_AD_TYPE_32SRV_PART ||
                   type == BK_BLE_AD_TYPE_32SRV_CMPL) {
            service_uuid_len = 4u;
        } else if (type == BK_BLE_AD_TYPE_128SRV_PART ||
                   type == BK_BLE_AD_TYPE_128SRV_CMPL) {
            service_uuid_len = 16u;
        }
        if (service_uuid_len > 0u) {
            for (size_t uuid_off = 0u;
                 uuid_off + service_uuid_len <= value_len &&
                 result->service_uuid_count < service_uuid_capacity;
                 uuid_off += service_uuid_len) {
                const size_t index = result->service_uuid_count++;
                memcpy(service_uuid_data[index], value + uuid_off,
                    service_uuid_len);
                service_uuids[index].data = service_uuid_data[index];
                service_uuids[index].len = service_uuid_len;
            }
            result->service_uuids = service_uuids;
        } else if ((type == BK_BLE_AD_TYPE_NAME_SHORT || type == BK_BLE_AD_TYPE_NAME_CMPL) && value_len > 0u) {
            size_t copy_len = value_len < name_size - 1u ? value_len : name_size - 1u;
            memcpy(name, value, copy_len);
            name[copy_len] = '\0';
            result->local_name = name;
            result->local_name_len = copy_len;
        } else if (type == BK_BLE_AD_TYPE_MANU && value_len > 0u) {
            result->manufacturer_data.data = value;
            result->manufacturer_data.len = value_len;
        } else if ((type == BK_BLE_AD_TYPE_SERVICE_DATA ||
                    type == H2_BK_BLE_AD_TYPE_SERVICE_DATA32 ||
                    type == H2_BK_BLE_AD_TYPE_SERVICE_DATA128) &&
                   value_len > 0u) {
            result->service_data.data = value;
            result->service_data.len = value_len;
        }
        off += value_len;
    }
}

static h2_pal_ble_phy_t h2_bk_ble_phy(bk_ble_gap_phy_t phy) {
    switch (phy) {
    case BK_BLE_GAP_PHY_1M:
        return H2_PAL_BLE_PHY_1M;
    case BK_BLE_GAP_PHY_2M:
        return H2_PAL_BLE_PHY_2M;
    case BK_BLE_GAP_PHY_CODED:
        return H2_PAL_BLE_PHY_CODED;
    default:
        return H2_PAL_BLE_PHY_UNKNOWN;
    }
}

static bk_ble_gap_phy_mask_t h2_bk_ble_phy_mask(h2_pal_ble_phy_t phy) {
    switch (phy) {
    case H2_PAL_BLE_PHY_1M:
        return BK_BLE_GAP_PHY_1M_PREF_MASK;
    case H2_PAL_BLE_PHY_2M:
        return BK_BLE_GAP_PHY_2M_PREF_MASK;
    case H2_PAL_BLE_PHY_CODED:
        return BK_BLE_GAP_PHY_CODED_PREF_MASK;
    default:
        return 0u;
    }
}

static void h2_bk_ble_gap_cb(bk_ble_gap_cb_event_t event, bk_ble_gap_cb_param_t *param) {
    switch (event) {
    case BK_BLE_GAP_SEC_REQ_EVT:
        if (param != NULL) {
            (void)bk_ble_gap_security_rsp(
                param->ble_security.ble_req.bd_addr,
                s_h2_bk_ble_pairing.enabled);
        }
        break;
    case BK_BLE_GAP_PASSKEY_REQ_EVT:
        if (param != NULL) {
            (void)bk_ble_passkey_reply(
                param->ble_security.key_notif.bd_addr,
                s_h2_bk_ble_pairing.enabled,
                s_h2_bk_ble_pairing.passkey);
        }
        break;
    case BK_BLE_GAP_AUTH_CMPL_EVT:
        if (param != NULL) {
            const uint8_t *expected_addr =
                s_h2_bk_ble_pair_conn_handle ==
                        s_h2_bk_ble_peripheral_conn_handle
                    ? s_h2_bk_ble_peripheral_peer_addr
                    : (s_h2_bk_ble_pair_conn_handle ==
                               s_h2_bk_ble_conn_handle
                           ? s_h2_bk_ble_peer_addr
                           : NULL);
            if (expected_addr != NULL &&
                memcmp(expected_addr,
                       param->ble_security.auth_cmpl.bd_addr,
                       sizeof(param->ble_security.auth_cmpl.bd_addr)) ==
                    0) {
                h2_bk_ble_signal(
                    param->ble_security.auth_cmpl.success &&
                            (param->ble_security.auth_cmpl.auth_mode &
                             BK_LE_AUTH_REQ_SC_MITM) ==
                                BK_LE_AUTH_REQ_SC_MITM
                        ? H2_PAL_OK
                        : H2_PAL_ERR_IO);
            }
        }
        break;
    case BK_BLE_GAP_EXT_ADV_PARAMS_SET_COMPLETE_EVT:
        h2_bk_ble_signal(h2_bk_ble_map_error(param != NULL ? param->adv_params_set.status : BK_FAIL));
        break;
    case BK_BLE_GAP_EXT_ADV_DATA_RAW_SET_COMPLETE_EVT:
        h2_bk_ble_signal(h2_bk_ble_map_error(param != NULL ? param->adv_data_raw_set.status : BK_FAIL));
        break;
    case BK_BLE_GAP_EXT_SCAN_RSP_DATA_RAW_SET_COMPLETE_EVT:
        h2_bk_ble_signal(h2_bk_ble_map_error(param != NULL ? param->scan_rsp_data_raw_set.status : BK_FAIL));
        break;
    case BK_BLE_GAP_EXT_ADV_START_COMPLETE_EVT:
        h2_bk_ble_signal(h2_bk_ble_map_error(param != NULL ? param->adv_start.status : BK_FAIL));
        break;
    case BK_BLE_GAP_EXT_ADV_STOP_COMPLETE_EVT:
        h2_bk_ble_signal(h2_bk_ble_map_error(param != NULL ? param->adv_stop.status : BK_FAIL));
        break;
    case BK_BLE_GAP_EXT_ADV_SET_REMOVE_COMPLETE_EVT:
        h2_bk_ble_signal(h2_bk_ble_map_error(param != NULL ? param->adv_remove.status : BK_FAIL));
        break;
    case BK_BLE_GAP_ADV_TERMINATED_EVT:
        if (param != NULL) {
            h2_pal_ble_adv_set_t *set =
                h2_bk_ble_adv_set_for_instance(param->adv_terminate.adv_instance);
            if (set != NULL && set->active) {
                set->active = 0;
                h2_bk_ble_post_adv_set(
                    H2_PAL_SYSTEM_EVENT_TYPE_BLE_ADVERTISING_STOPPED,
                    set,
                    H2_PAL_OK);
            } else if (param->adv_terminate.adv_instance == s_h2_bk_ble_adv_activity &&
                       s_h2_bk_ble_adv_started) {
                s_h2_bk_ble_adv_started = 0;
                h2_bk_ble_post(
                    H2_PAL_SYSTEM_EVENT_TYPE_BLE_ADVERTISING_STOPPED, NULL, 0u);
            }
        }
        break;
    case BK_BLE_GAP_SCAN_PARAM_SET_COMPLETE_EVT:
    case BK_BLE_GAP_EXT_SCAN_PARAMS_SET_COMPLETE_EVT:
        h2_bk_ble_signal(h2_bk_ble_map_error(param != NULL ? param->scan_params_set.status : BK_FAIL));
        break;
    case BK_BLE_GAP_SCAN_START_COMPLETE_EVT:
    case BK_BLE_GAP_EXT_SCAN_START_COMPLETE_EVT:
        h2_bk_ble_signal(h2_bk_ble_map_error(param != NULL ? param->scan_start.status : BK_FAIL));
        if (param != NULL && param->scan_start.status == BK_OK) {
            s_h2_bk_ble_scan_started = 1;
            h2_bk_ble_post(H2_PAL_SYSTEM_EVENT_TYPE_BLE_SCAN_STARTED, NULL, 0u);
        }
        break;
    case BK_BLE_GAP_SCAN_STOP_COMPLETE_EVT:
    case BK_BLE_GAP_EXT_SCAN_STOP_COMPLETE_EVT:
    case BK_BLE_GAP_SCAN_TIMEOUT_EVT:
        s_h2_bk_ble_scan_started = 0;
        h2_bk_ble_signal(event == BK_BLE_GAP_SCAN_TIMEOUT_EVT
                             ? H2_PAL_OK
                             : h2_bk_ble_map_error(param != NULL ? param->scan_stop.status : BK_FAIL));
        s_h2_bk_ble_scan_cb = NULL;
        s_h2_bk_ble_scan_user = NULL;
        h2_bk_ble_post(H2_PAL_SYSTEM_EVENT_TYPE_BLE_SCAN_STOPPED, NULL, 0u);
        break;
    case BK_BLE_GAP_EXT_ADV_REPORT_EVT: {
        if (param == NULL || s_h2_bk_ble_scan_cb == NULL) {
            break;
        }
        const bk_ble_gap_ext_adv_reprot_t *report = &param->ext_adv_report.params;
        h2_pal_ble_scan_result_t result;
        memset(&result, 0, sizeof(result));
        result.addr.type = h2_bk_ble_addr_type(report->addr_type);
        memcpy(result.addr.value, report->addr, sizeof(result.addr.value));
        result.rssi = report->rssi;
        result.connectable = report->event_type == BK_BLE_LEGACY_ADV_TYPE_IND ||
                             report->event_type == BK_BLE_LEGACY_ADV_TYPE_DIRECT_IND ||
                             (report->event_type & BK_BLE_ADV_REPORT_EXT_ADV_IND) != 0u;
        result.scan_response = report->event_type == BK_BLE_LEGACY_ADV_TYPE_SCAN_RSP_TO_ADV_IND ||
                               report->event_type == BK_BLE_LEGACY_ADV_TYPE_SCAN_RSP_TO_ADV_SCAN_IND ||
                               (report->event_type & BK_BLE_ADV_REPORT_EXT_SCAN_RSP) != 0u;
        if ((report->event_type & 0x10u) != 0u) {
            result.adv_type = H2_PAL_BLE_ADV_TYPE_LEGACY;
            result.primary_phy = H2_PAL_BLE_PHY_1M;
            result.tx_power = 127;
        } else {
            result.adv_type = H2_PAL_BLE_ADV_TYPE_EXTENDED;
            result.primary_phy = h2_bk_ble_phy(report->primary_phy);
            result.secondary_phy = h2_bk_ble_phy(report->secondly_phy);
            result.sid = report->sid;
            result.data_status = report->data_status == BK_BLE_GAP_EXT_ADV_DATA_INCOMPLETE
                                     ? H2_PAL_BLE_ADV_DATA_INCOMPLETE
                                 : report->data_status == BK_BLE_GAP_EXT_ADV_DATA_TRUNCATED
                                     ? H2_PAL_BLE_ADV_DATA_TRUNCATED
                                     : H2_PAL_BLE_ADV_DATA_COMPLETE;
            result.tx_power = (int8_t)report->tx_power;
        }
        result.raw_data.data = report->adv_data;
        result.raw_data.len = report->adv_data_len;
        char name[32] = { 0 };
        h2_pal_ble_uuid_t service_uuids[H2_BK_BLE_MAX_SCAN_SERVICE_UUIDS];
        uint8_t service_uuid_data[H2_BK_BLE_MAX_SCAN_SERVICE_UUIDS][16];
        memset(service_uuids, 0, sizeof(service_uuids));
        memset(service_uuid_data, 0, sizeof(service_uuid_data));
        h2_bk_ble_parse_adv_data(
            report->adv_data,
            report->adv_data_len,
            name,
            sizeof(name),
            service_uuids,
            service_uuid_data,
            H2_BK_BLE_MAX_SCAN_SERVICE_UUIDS,
            &result);
        if (s_h2_bk_ble_scan_cb(s_h2_bk_ble_scan_user, &result)) {
            (void)bk_ble_gap_stop_scan();
        }
        break;
    }
    case BK_BLE_GAP_CONNECT_COMPLETE_EVT:
        if (param != NULL && param->connect_complete.status == BK_OK) {
            if (param->connect_complete.link_role != 0u) {
                memcpy(
                    s_h2_bk_ble_peripheral_peer_addr,
                    param->connect_complete.remote_bda,
                    sizeof(s_h2_bk_ble_peripheral_peer_addr));
                s_h2_bk_ble_peripheral_peer_addr_type =
                    h2_bk_ble_addr_type(
                        param->connect_complete.remote_bda_type);
                s_h2_bk_ble_peripheral_hci_handle =
                    param->connect_complete.hci_handle;
                break;
            }
            memcpy(s_h2_bk_ble_peer_addr, param->connect_complete.remote_bda, sizeof(s_h2_bk_ble_peer_addr));
            s_h2_bk_ble_peer_addr_type =
                h2_bk_ble_addr_type(param->connect_complete.remote_bda_type);
            s_h2_bk_ble_connect_result = H2_PAL_OK;
            s_h2_bk_ble_hci_handle = param->connect_complete.hci_handle;
        } else {
            s_h2_bk_ble_connect_result = H2_PAL_ERR_IO;
            s_h2_bk_ble_conn_handle = H2_PAL_BLE_INVALID_CONN_HANDLE;
            s_h2_bk_ble_hci_handle = H2_PAL_BLE_INVALID_CONN_HANDLE;
        }
        h2_bk_ble_signal(s_h2_bk_ble_connect_result);
        break;
    case BK_BLE_GAP_UPDATE_CONN_PARAMS_EVT:
        if (param != NULL) {
            const h2_pal_ble_connection_params_t params = {
                .interval_min_ms = (uint16_t)(
                    (param->update_conn_params.conn_int * 125u) / 100u),
                .interval_max_ms = (uint16_t)(
                    (param->update_conn_params.conn_int * 125u) / 100u),
                .latency = param->update_conn_params.latency,
                .supervision_timeout_ms = (uint16_t)(
                    param->update_conn_params.timeout * 10u),
            };
            os_printf(
                "H2_BK_BLE_CONN_UPDATE status=%d interval_units=%u interval_ms=%u latency=%u timeout_units=%u\n",
                (int)param->update_conn_params.status,
                (unsigned)param->update_conn_params.conn_int,
                (unsigned)((param->update_conn_params.conn_int * 125u) / 100u),
                (unsigned)param->update_conn_params.latency,
                (unsigned)param->update_conn_params.timeout);
            h2_bk_ble_post(
                H2_PAL_SYSTEM_EVENT_TYPE_BLE_CONNECTION_UPDATED,
                &params, sizeof(params));
            h2_bk_ble_signal(param->update_conn_params.status == BK_OK ? H2_PAL_OK : H2_PAL_ERR_IO);
        } else {
            h2_bk_ble_signal(H2_PAL_ERR_IO);
        }
        break;
    case BK_BLE_GAP_SET_PKT_LENGTH_COMPLETE_EVT:
        if (param != NULL) {
            os_printf(
                "H2_BK_BLE_DATA_LEN status=%d tx_len=%u rx_len=%u\n",
                (int)param->pkt_data_length_cmpl.status,
                (unsigned)param->pkt_data_length_cmpl.params.tx_len,
                (unsigned)param->pkt_data_length_cmpl.params.rx_len);
        }
        break;
    case BK_BLE_GAP_SET_PREFERRED_PHY_COMPLETE_EVT:
        if (param != NULL) {
            os_printf("H2_BK_BLE_PHY_SET status=%d\n", (int)param->set_perf_phy.status);
            h2_bk_ble_signal(param->set_perf_phy.status == BK_OK ? H2_PAL_OK : H2_PAL_ERR_IO);
        } else {
            h2_bk_ble_signal(H2_PAL_ERR_IO);
        }
        break;
    case BK_BLE_GAP_READ_PHY_COMPLETE_EVT:
        if (param != NULL) {
            memset(&s_h2_bk_ble_last_phy, 0, sizeof(s_h2_bk_ble_last_phy));
            s_h2_bk_ble_last_phy.conn_handle = s_h2_bk_ble_conn_handle;
            s_h2_bk_ble_last_phy.tx_phy = h2_bk_ble_phy(param->read_phy.tx_phy);
            s_h2_bk_ble_last_phy.rx_phy = h2_bk_ble_phy(param->read_phy.rx_phy);
            os_printf(
                "H2_BK_BLE_PHY_READ status=%d tx_phy=%u rx_phy=%u\n",
                (int)param->read_phy.status,
                (unsigned)param->read_phy.tx_phy,
                (unsigned)param->read_phy.rx_phy);
            h2_bk_ble_signal(param->read_phy.status == BK_OK ? H2_PAL_OK : H2_PAL_ERR_IO);
        } else {
            h2_bk_ble_signal(H2_PAL_ERR_IO);
        }
        break;
    case BK_BLE_GAP_DISCONNECT_COMPLETE_EVT:
        if (param != NULL &&
            s_h2_bk_ble_hci_handle != H2_PAL_BLE_INVALID_CONN_HANDLE &&
            param->disconnect_complete.hci_handle == s_h2_bk_ble_hci_handle) {
            h2_pal_ble_disconnected_info_t info;
            memset(&info, 0, sizeof(info));
            info.conn_handle = s_h2_bk_ble_conn_handle;
            info.peer_addr.type = h2_bk_ble_addr_type(param->disconnect_complete.remote_bda_type);
            memcpy(info.peer_addr.value, param->disconnect_complete.remote_bda, sizeof(info.peer_addr.value));
            info.reason = param->disconnect_complete.reason;
            h2_bk_ble_post(H2_PAL_SYSTEM_EVENT_TYPE_BLE_DISCONNECTED, &info, sizeof(info));
            s_h2_bk_ble_conn_handle = H2_PAL_BLE_INVALID_CONN_HANDLE;
            s_h2_bk_ble_hci_handle = H2_PAL_BLE_INVALID_CONN_HANDLE;
            s_h2_bk_ble_gattc_connected = 0;
            h2_bk_ble_signal(H2_PAL_OK);
        } else if (param != NULL &&
                   s_h2_bk_ble_peripheral_hci_handle !=
                       H2_PAL_BLE_INVALID_CONN_HANDLE &&
                   param->disconnect_complete.hci_handle ==
                       s_h2_bk_ble_peripheral_hci_handle) {
            s_h2_bk_ble_peripheral_hci_handle =
                H2_PAL_BLE_INVALID_CONN_HANDLE;
            h2_bk_ble_signal(H2_PAL_OK);
        }
        break;
    default:
        break;
    }
}

static void h2_bk_ble_cmd_cb(ble_cmd_t cmd, ble_cmd_param_t *param) {
    (void)cmd;
    h2_bk_ble_signal(h2_bk_ble_map_error(param != NULL ? param->status : BK_FAIL));
}

static void h2_bk_ble_sdp_common_cb(MASTER_COMMON_TYPE type, uint8 conidx, void *param) {
    (void)conidx;
    if (type == MST_TYPE_DISCOVER_COMPLETED) {
        h2_bk_ble_signal(H2_PAL_OK);
        return;
    }
    if (type == MST_TYPS_ATTC_PARAM_ERR || type == MST_TYPE_ATTC_ERR) {
        h2_bk_ble_signal(H2_PAL_ERR_IO);
        return;
    }
    if (s_h2_bk_ble_discovery_entries == NULL ||
        s_h2_bk_ble_discovery_count >= s_h2_bk_ble_discovery_max_entries) {
        return;
    }

    uint16_t filter = h2_bk_ble_uuid16_value(&s_h2_bk_ble_discovery_request.uuid_filter);
    if (type == MST_TYPE_DISCOVER_PRI_SERVICE_RSP ||
        type == MST_TYPE_DISCOVER_PRI_SERVICE_BY_UUID_RSP ||
        type == MST_TYPE_DISCOVER_PRI_SERVICE_BY_128_UUID_RSP) {
        if (s_h2_bk_ble_discovery_request.kind != H2_PAL_BLE_GATT_DISCOVERY_SERVICE || param == NULL) {
            return;
        }
        const struct ble_sdp_svc_ind *service = (const struct ble_sdp_svc_ind *)param;
        uint16_t uuid16 = h2_bk_ble_raw_uuid16(service->uuid, service->uuid_len);
        if (filter != 0u && uuid16 != filter) {
            return;
        }
        size_t idx = s_h2_bk_ble_discovery_count++;
        h2_pal_ble_gatt_discovery_entry_t *entry = &s_h2_bk_ble_discovery_entries[idx];
        memset(entry, 0, sizeof(*entry));
        entry->kind = H2_PAL_BLE_GATT_DISCOVERY_SERVICE;
        entry->start_handle = service->start_hdl;
        entry->end_handle = service->end_hdl;
        h2_bk_ble_copy_uuid16(uuid16, &entry->uuid, s_h2_bk_ble_discovery_uuid_data[idx]);
    } else if (type == MST_TYPE_DISCOVER_CHAR_RSP ||
               type == MST_TYPE_DISCOVER_CHAR_BY_UUID_RSP ||
               type == MST_TYPE_DISCOVER_CHAR_BY_128_UUID_RSP) {
        if (s_h2_bk_ble_discovery_request.kind != H2_PAL_BLE_GATT_DISCOVERY_CHARACTERISTIC || param == NULL) {
            return;
        }
        const struct ble_sdp_char_inf *characteristic = (const struct ble_sdp_char_inf *)param;
        uint16_t uuid16 = h2_bk_ble_raw_uuid16(characteristic->uuid, characteristic->uuid_len);
        if (filter != 0u && uuid16 != filter) {
            return;
        }
        size_t idx = s_h2_bk_ble_discovery_count++;
        h2_pal_ble_gatt_discovery_entry_t *entry = &s_h2_bk_ble_discovery_entries[idx];
        memset(entry, 0, sizeof(*entry));
        entry->kind = H2_PAL_BLE_GATT_DISCOVERY_CHARACTERISTIC;
        entry->start_handle = characteristic->char_hdl;
        entry->end_handle = characteristic->char_hdl + characteristic->char_ehdl_off;
        entry->value_handle = characteristic->val_hdl;
        entry->properties = h2_bk_ble_gatt_properties_from_bk(
            characteristic->prop);
        h2_bk_ble_copy_uuid16(uuid16, &entry->uuid, s_h2_bk_ble_discovery_uuid_data[idx]);
    } else if (type == MST_TYPE_DISCOVER_CHAR_DESC) {
        if (s_h2_bk_ble_discovery_request.kind != H2_PAL_BLE_GATT_DISCOVERY_DESCRIPTOR || param == NULL) {
            return;
        }
        const struct ble_sdp_char_desc_inf *descriptor = (const struct ble_sdp_char_desc_inf *)param;
        uint16_t uuid16 = h2_bk_ble_raw_uuid16(descriptor->uuid, descriptor->uuid_len);
        if (filter != 0u && uuid16 != filter) {
            return;
        }
        size_t idx = s_h2_bk_ble_discovery_count++;
        h2_pal_ble_gatt_discovery_entry_t *entry = &s_h2_bk_ble_discovery_entries[idx];
        memset(entry, 0, sizeof(*entry));
        entry->kind = H2_PAL_BLE_GATT_DISCOVERY_DESCRIPTOR;
        entry->value_handle = descriptor->desc_hdl;
        h2_bk_ble_copy_uuid16(uuid16, &entry->uuid, s_h2_bk_ble_discovery_uuid_data[idx]);
    }
}

static void h2_bk_ble_sdp_char_cb(CHAR_TYPE type, uint8 conidx, uint16_t hdl, uint16_t len, uint8 *data) {
    if (type == CHARAC_NOTIFY || type == CHARAC_INDICATE) {
        h2_pal_ble_gatt_client_value_t value;
        memset(&value, 0, sizeof(value));
        value.conn_handle = conidx;
        value.attr_handle = hdl;
        value.value_len = len;
        if (value.value_len > sizeof(value.value)) {
            value.value_len = sizeof(value.value);
        }
        if (value.value_len > 0u && data != NULL) {
            memcpy(value.value, data, value.value_len);
        } else {
            value.value_len = 0u;
        }
        h2_bk_ble_post(
            type == CHARAC_NOTIFY
                ? H2_PAL_SYSTEM_EVENT_TYPE_BLE_GATT_CLIENT_NOTIFICATION
                : H2_PAL_SYSTEM_EVENT_TYPE_BLE_GATT_CLIENT_INDICATION,
            &value,
            sizeof(value));
    } else if (hdl == s_h2_bk_ble_pending_handle && type == CHARAC_READ) {
        s_h2_bk_ble_read_out_len = len;
        if (s_h2_bk_ble_read_out_len > s_h2_bk_ble_read_out_size) {
            s_h2_bk_ble_read_out_len = s_h2_bk_ble_read_out_size;
        }
        if (s_h2_bk_ble_read_out_len > 0u && s_h2_bk_ble_read_out != NULL && data != NULL) {
            memcpy(s_h2_bk_ble_read_out, data, s_h2_bk_ble_read_out_len);
        }
        h2_bk_ble_signal(H2_PAL_OK);
    } else if (type == CHARAC_WRITE_DONE && s_h2_bk_ble_waiting_write) {
        /* The legacy BK callback does not preserve the requested descriptor
         * handle for CCC writes.  Writes are serialized and stale completion
         * signals are drained before setting waiting_write. */
        h2_bk_ble_signal(H2_PAL_OK);
    }
}

static void h2_bk_ble_clear_scan_state(void) {
    s_h2_bk_ble_scan_started = 0;
    s_h2_bk_ble_scan_created = 0;
    s_h2_bk_ble_scan_extended = 0;
    s_h2_bk_ble_scan_activity = H2_BK_BLE_INVALID_ACTIVITY;
    s_h2_bk_ble_scan_cb = NULL;
    s_h2_bk_ble_scan_user = NULL;
}

static void h2_bk_ble_clear_adv_state(void) {
    s_h2_bk_ble_adv_started = 0;
    s_h2_bk_ble_adv_created = 0;
    s_h2_bk_ble_adv_activity = H2_BK_BLE_INVALID_ACTIVITY;
}

static int h2_bk_ble_uses_ethermind(void) {
    return bk_ble_get_host_stack_type() == BK_BLE_HOST_STACK_TYPE_ETHERMIND;
}

static void h2_bk_ble_clear_init_state(void) {
    s_h2_bk_ble_init_started = 0;
    s_h2_bk_ble_init_connected = 0;
}

static h2_pal_result_t h2_bk_ble_cleanup_scan(void) {
    h2_pal_result_t rc = H2_PAL_OK;
    if (s_h2_bk_ble_scan_extended) {
        if (s_h2_bk_ble_scan_started) {
            h2_bk_ble_drain_signals();
            rc = h2_bk_ble_map_error(bk_ble_gap_stop_scan());
            if (rc == H2_PAL_OK) {
                rc = h2_bk_ble_wait(H2_BK_BLE_TIMEOUT_MS);
            }
        }
        h2_bk_ble_clear_scan_state();
        return rc;
    }
    if (s_h2_bk_ble_scan_started) {
        h2_bk_ble_drain_signals();
        rc = h2_bk_ble_map_error(bk_ble_stop_scaning(s_h2_bk_ble_scan_activity, h2_bk_ble_cmd_cb));
        if (rc == H2_PAL_OK) {
            rc = h2_bk_ble_wait(H2_BK_BLE_TIMEOUT_MS);
        }
        if (rc != H2_PAL_OK) {
            h2_bk_ble_clear_scan_state();
            return rc;
        }
    }
    if (s_h2_bk_ble_scan_created) {
        h2_bk_ble_drain_signals();
        rc = h2_bk_ble_map_error(bk_ble_delete_scaning(s_h2_bk_ble_scan_activity, h2_bk_ble_cmd_cb));
        if (rc == H2_PAL_OK) {
            rc = h2_bk_ble_wait(H2_BK_BLE_TIMEOUT_MS);
        }
        if (rc != H2_PAL_OK) {
            h2_bk_ble_clear_scan_state();
            return rc;
        }
    }
    h2_bk_ble_clear_scan_state();
    h2_bk_ble_post(H2_PAL_SYSTEM_EVENT_TYPE_BLE_SCAN_STOPPED, NULL, 0u);
    return H2_PAL_OK;
}

static h2_pal_result_t h2_bk_ble_cleanup_advertising(void) {
    h2_pal_result_t rc = H2_PAL_OK;
    if (!h2_bk_ble_uses_ethermind()) {
        if (s_h2_bk_ble_adv_started) {
            h2_bk_ble_drain_signals();
            rc = h2_bk_ble_map_error(
                bk_ble_stop_advertising(s_h2_bk_ble_adv_activity, h2_bk_ble_cmd_cb));
            if (rc == H2_PAL_OK) {
                rc = h2_bk_ble_wait(H2_BK_BLE_TIMEOUT_MS);
            }
            if (rc != H2_PAL_OK) {
                h2_bk_ble_clear_adv_state();
                return rc;
            }
        }
        if (s_h2_bk_ble_adv_created) {
            h2_bk_ble_drain_signals();
            rc = h2_bk_ble_map_error(
                bk_ble_delete_advertising(s_h2_bk_ble_adv_activity, h2_bk_ble_cmd_cb));
            if (rc == H2_PAL_OK) {
                rc = h2_bk_ble_wait(H2_BK_BLE_TIMEOUT_MS);
            }
            if (rc != H2_PAL_OK) {
                h2_bk_ble_clear_adv_state();
                return rc;
            }
        }
        h2_bk_ble_clear_adv_state();
        h2_bk_ble_post(H2_PAL_SYSTEM_EVENT_TYPE_BLE_ADVERTISING_STOPPED, NULL, 0u);
        return H2_PAL_OK;
    }
    if (s_h2_bk_ble_adv_started) {
        uint8_t instance = s_h2_bk_ble_adv_activity;
        h2_bk_ble_drain_signals();
        rc = h2_bk_ble_map_error(bk_ble_gap_adv_stop(1u, &instance));
        if (rc == H2_PAL_OK) {
            rc = h2_bk_ble_wait(H2_BK_BLE_TIMEOUT_MS);
        }
        if (rc != H2_PAL_OK) {
            h2_bk_ble_clear_adv_state();
            return rc;
        }
        if (s_h2_bk_ble_adv_started) {
            s_h2_bk_ble_adv_started = 0;
            h2_bk_ble_post(H2_PAL_SYSTEM_EVENT_TYPE_BLE_ADVERTISING_STOPPED, NULL, 0u);
        }
    }
    if (s_h2_bk_ble_adv_created) {
        h2_bk_ble_drain_signals();
        rc = h2_bk_ble_map_error(bk_ble_gap_adv_set_remove(s_h2_bk_ble_adv_activity));
        if (rc == H2_PAL_OK) {
            rc = h2_bk_ble_wait(H2_BK_BLE_TIMEOUT_MS);
        }
        if (rc != H2_PAL_OK) {
            h2_bk_ble_clear_adv_state();
            return rc;
        }
    }
    h2_bk_ble_clear_adv_state();
    return H2_PAL_OK;
}

static h2_pal_result_t h2_bk_ble_set_extended_adv_data(
    uint8_t instance,
    const uint8_t *data,
    size_t len) {
    size_t offset = 0u;
    while (offset < len) {
        size_t remaining = len - offset;
        size_t fragment_len = remaining > H2_BK_BLE_EXT_ADV_FRAGMENT_MAX_LEN
                                  ? H2_BK_BLE_EXT_ADV_FRAGMENT_MAX_LEN
                                  : remaining;
        uint8_t operation;
        if (len <= H2_BK_BLE_EXT_ADV_FRAGMENT_MAX_LEN) {
            operation = H2_BK_BLE_EXT_ADV_DATA_COMPLETE;
        } else if (offset == 0u) {
            operation = H2_BK_BLE_EXT_ADV_DATA_FIRST;
        } else if (fragment_len == remaining) {
            operation = H2_BK_BLE_EXT_ADV_DATA_LAST;
        } else {
            operation = H2_BK_BLE_EXT_ADV_DATA_INTERMEDIATE;
        }

        ble_gap_adv_data_t request = {
            .adv_handle = instance,
            .operation = operation,
            .frag_pref = 1u,
            .adv_data_len = (uint8_t)fragment_len,
            .cmd_type = CMD_TYPE_ADV_RAW,
        };
        memcpy(request.adv_data, &data[offset], fragment_len);
        h2_bk_ble_drain_signals();
        h2_pal_result_t rc = h2_bk_ble_map_error(ble_ethermind_post_msg(
            BLE_ETHERMIND_MSG_GAP_API_REQ,
            BLE_ETHERMIND_GAP_API_REQ_SUBMSG_SET_ADV_DATA,
            &request,
            sizeof(request),
            NULL));
        if (rc == H2_PAL_OK) {
            rc = h2_bk_ble_wait(H2_BK_BLE_TIMEOUT_MS);
        }
        if (rc != H2_PAL_OK) {
            return rc;
        }
        offset += fragment_len;
    }
    return H2_PAL_OK;
}

static void h2_bk_ble_fill_ext_adv_params(
    const h2_pal_ble_adv_params_t *params,
    bk_ble_gap_ext_adv_params_t *out) {
    memset(out, 0, sizeof(*out));
    if (params->type == H2_PAL_BLE_ADV_TYPE_LEGACY) {
        out->type = params->mode == H2_PAL_BLE_ADV_MODE_CONNECTABLE
                        ? BK_BLE_GAP_SET_EXT_ADV_PROP_LEGACY_IND
                        : BK_BLE_GAP_SET_EXT_ADV_PROP_LEGACY_SCAN;
    } else {
        out->type = params->mode == H2_PAL_BLE_ADV_MODE_CONNECTABLE
                        ? BK_BLE_GAP_SET_EXT_ADV_PROP_CONNECTABLE
                        : BK_BLE_GAP_SET_EXT_ADV_PROP_NONCONN_NONSCANNABLE_UNDIRECTED;
    }
    out->interval_min = h2_bk_ble_ms_to_ext_units625(params->interval_min_ms);
    out->interval_max = h2_bk_ble_ms_to_ext_units625(params->interval_max_ms);
    out->channel_map = BK_ADV_CHNL_ALL;
    out->own_addr_type = BLE_ADDR_TYPE_PUBLIC;
    out->filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY;
    out->tx_power = EXT_ADV_TX_PWR_NO_PREFERENCE;
    out->primary_phy = params->primary_phy == H2_PAL_BLE_PHY_CODED
                           ? BK_BLE_GAP_PRI_PHY_CODED
                           : BK_BLE_GAP_PRI_PHY_1M;
    out->secondary_phy = params->secondary_phy == H2_PAL_BLE_PHY_2M
                             ? BK_BLE_GAP_PHY_2M
                             : params->secondary_phy == H2_PAL_BLE_PHY_CODED
                                   ? BK_BLE_GAP_PHY_CODED
                                   : BK_BLE_GAP_PHY_1M;
    out->sid = params->sid;
}

static h2_pal_result_t h2_bk_ble_cleanup_adv_set(h2_pal_ble_adv_set_t *set) {
    if (!h2_bk_ble_adv_set_valid(set)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (set->active) {
        uint8_t instance = set->instance;
        h2_bk_ble_drain_signals();
        h2_pal_result_t rc = h2_bk_ble_map_error(
            bk_ble_gap_adv_stop(1u, &instance));
        if (rc == H2_PAL_OK) {
            rc = h2_bk_ble_wait(H2_BK_BLE_TIMEOUT_MS);
        }
        if (rc != H2_PAL_OK) {
            return rc;
        }
        set->active = 0;
        h2_bk_ble_post_adv_set(
            H2_PAL_SYSTEM_EVENT_TYPE_BLE_ADVERTISING_STOPPED,
            set,
            H2_PAL_OK);
    }
    h2_bk_ble_drain_signals();
    h2_pal_result_t rc = h2_bk_ble_map_error(
        bk_ble_gap_adv_set_remove(set->instance));
    if (rc == H2_PAL_OK) {
        rc = h2_bk_ble_wait(H2_BK_BLE_TIMEOUT_MS);
    }
    if (rc == H2_PAL_OK) {
        memset(set, 0, sizeof(*set));
    }
    return rc;
}

static h2_pal_result_t h2_bk_ble_cleanup_initiator(void) {
    if (s_h2_bk_ble_init_started || s_h2_bk_ble_init_connected) {
        h2_bk_ble_drain_signals();
        h2_pal_result_t rc = h2_bk_ble_map_error(
            bk_ble_init_stop_conn(s_h2_bk_ble_init_activity, h2_bk_ble_cmd_cb));
        if (rc == H2_PAL_OK) {
            rc = h2_bk_ble_wait(H2_BK_BLE_TIMEOUT_MS);
        }
        if (rc != H2_PAL_OK) {
            h2_bk_ble_clear_init_state();
            return rc;
        }
    }
    h2_bk_ble_clear_init_state();
    return H2_PAL_OK;
}

static void h2_bk_ble_notice_cb_unlocked(ble_notice_t notice, void *param);

static void h2_bk_ble_notice_cb(ble_notice_t notice, void *param) {
    int guarded = notice == BLE_5_WRITE_EVENT || notice == BLE_5_READ_EVENT;
    if (guarded && s_h2_bk_ble_gatt_mutex == NULL) return;
    if (guarded && rtos_lock_mutex(&s_h2_bk_ble_gatt_mutex) != kNoErr) return;
    h2_bk_ble_notice_cb_unlocked(notice, param);
    if (guarded) (void)rtos_unlock_mutex(&s_h2_bk_ble_gatt_mutex);
}

static void h2_bk_ble_notice_cb_unlocked(ble_notice_t notice, void *param) {
    if (notice == BLE_5_PAIRING_REQ) {
        const ble_smp_ind_t *security = (const ble_smp_ind_t *)param;
        if (security != NULL && s_h2_bk_ble_pairing.enabled) {
            (void)bk_ble_sec_send_auth_mode(
                security->conn_idx,
                GAP_AUTH_REQ_SEC_CON_NO_BOND,
                s_h2_bk_ble_pairing.io ==
                        H2_PAL_BLE_PAIRING_IO_DISPLAY_ONLY
                    ? BK_BLE_GAP_IO_CAP_DISPLAY_ONLY
                    : BK_BLE_GAP_IO_CAP_KB_ONLY,
                GAP_SEC1_SEC_CON_PAIR_ENC,
                GAP_OOB_AUTH_DATA_NOT_PRESENT);
        }
    } else if (notice == BLE_5_PAIRING_SECURITY_REQ_EVENT) {
        const ble_smp_ind_t *security = (const ble_smp_ind_t *)param;
        if (security != NULL && s_h2_bk_ble_pairing.enabled) {
            (void)bk_ble_create_bond(
                security->conn_idx,
                GAP_AUTH_REQ_SEC_CON_NO_BOND,
                s_h2_bk_ble_pairing.io ==
                        H2_PAL_BLE_PAIRING_IO_DISPLAY_ONLY
                    ? BK_BLE_GAP_IO_CAP_DISPLAY_ONLY
                    : BK_BLE_GAP_IO_CAP_KB_ONLY,
                GAP_SEC1_SEC_CON_PAIR_ENC,
                GAP_OOB_AUTH_DATA_NOT_PRESENT);
        }
    } else if (notice == BLE_5_PARING_PASSKEY_REQ) {
        const ble_smp_ind_t *security = (const ble_smp_ind_t *)param;
        if (security != NULL) {
            (void)bk_ble_passkey_send(
                security->conn_idx,
                s_h2_bk_ble_pairing.enabled,
                s_h2_bk_ble_pairing.passkey);
        }
    } else if (notice == BLE_5_PAIRING_SUCCEED ||
               notice == BLE_5_PAIRING_FAILED) {
        const ble_smp_ind_t *security = (const ble_smp_ind_t *)param;
        if (security != NULL &&
            security->conn_idx == s_h2_bk_ble_pair_conn_handle) {
            h2_bk_ble_signal(notice == BLE_5_PAIRING_SUCCEED
                                 ? H2_PAL_OK
                                 : H2_PAL_ERR_IO);
        }
    } else if (notice == BLE_5_REPORT_ADV) {
        if (param == NULL || s_h2_bk_ble_scan_cb == NULL) {
            return;
        }
        const ble_recv_adv_t *report = (const ble_recv_adv_t *)param;
        h2_pal_ble_scan_result_t result;
        memset(&result, 0, sizeof(result));
        result.addr.type = h2_bk_ble_addr_type(report->adv_addr_type);
        memcpy(result.addr.value, report->adv_addr, sizeof(result.addr.value));
        result.rssi = report->rssi;
        result.connectable = (report->evt_type & REPORT_INFO_CONN_ADV_BIT) != 0u;
        result.scan_response = (report->evt_type & REPORT_INFO_REPORT_TYPE_MASK) == REPORT_TYPE_SCAN_RSP_LEG ||
                               (report->evt_type & REPORT_INFO_REPORT_TYPE_MASK) == REPORT_TYPE_SCAN_RSP_EXT;
        result.adv_type = H2_PAL_BLE_ADV_TYPE_LEGACY;
        result.primary_phy = H2_PAL_BLE_PHY_1M;
        result.tx_power = 127;
        result.raw_data.data = report->data;
        result.raw_data.len = report->data_len;
        char name[32] = { 0 };
        h2_pal_ble_uuid_t service_uuids[H2_BK_BLE_MAX_SCAN_SERVICE_UUIDS];
        uint8_t service_uuid_data[H2_BK_BLE_MAX_SCAN_SERVICE_UUIDS][16];
        memset(service_uuids, 0, sizeof(service_uuids));
        memset(service_uuid_data, 0, sizeof(service_uuid_data));
        h2_bk_ble_parse_adv_data(
            report->data,
            report->data_len,
            name,
            sizeof(name),
            service_uuids,
            service_uuid_data,
            H2_BK_BLE_MAX_SCAN_SERVICE_UUIDS,
            &result);
        if (s_h2_bk_ble_scan_cb(s_h2_bk_ble_scan_user, &result)) {
            (void)bk_ble_stop_scaning(s_h2_bk_ble_scan_activity, h2_bk_ble_cmd_cb);
        }
    } else if (notice == BLE_5_SCAN_STOPPED_EVENT) {
        s_h2_bk_ble_scan_started = 0;
        s_h2_bk_ble_scan_cb = NULL;
        s_h2_bk_ble_scan_user = NULL;
        h2_bk_ble_post(H2_PAL_SYSTEM_EVENT_TYPE_BLE_SCAN_STOPPED, NULL, 0u);
    } else if (notice == BLE_5_ADV_STOPPED_EVENT) {
        s_h2_bk_ble_adv_started = 0;
        h2_bk_ble_post(H2_PAL_SYSTEM_EVENT_TYPE_BLE_ADVERTISING_STOPPED, NULL, 0u);
    } else if (notice == BLE_5_CREATE_DB) {
        const ble_create_db_t *created = (const ble_create_db_t *)param;
        if (created != NULL && created->prf_id == H2_BK_BLE_LEGACY_PRF_ID && created->status == BK_ERR_BLE_SUCCESS) {
            s_h2_bk_ble_service_handle[0] =
                created->start_hdl + H2_BK_BLE_LEGACY_IDX_SVC;
            s_h2_bk_ble_value_handle[0] = created->start_hdl + H2_BK_BLE_LEGACY_IDX_CHAR0_VALUE;
            s_h2_bk_ble_cccd_handle[0] = created->start_hdl + H2_BK_BLE_LEGACY_IDX_CHAR0_CCCD;
            if (s_h2_bk_ble_service_characteristic_count[0] > 1u) {
                s_h2_bk_ble_value_handle[1] = created->start_hdl + H2_BK_BLE_LEGACY_IDX_CHAR1_VALUE;
                s_h2_bk_ble_cccd_handle[1] = created->start_hdl + H2_BK_BLE_LEGACY_IDX_CHAR1_CCCD;
            }
            h2_bk_ble_update_out_handles();
            s_h2_bk_ble_legacy_service_created = 1;
            s_h2_bk_ble_service_started[0] = 1;
            h2_bk_ble_signal(H2_PAL_OK);
        } else if (created != NULL && created->prf_id == H2_BK_BLE_LEGACY_PRF_ID) {
            h2_bk_ble_signal(H2_PAL_ERR_IO);
        }
    } else if (notice == BLE_5_CONNECT_EVENT) {
        const ble_conn_ind_t *conn = (const ble_conn_ind_t *)param;
        if (conn != NULL) {
            h2_bk_ble_mark_connectable_advertising_stopped();
            s_h2_bk_ble_peripheral_conn_handle = conn->conn_idx;
            memcpy(
                s_h2_bk_ble_peripheral_peer_addr,
                conn->peer_addr,
                sizeof(s_h2_bk_ble_peripheral_peer_addr));
            h2_pal_ble_connection_t event;
            memset(&event, 0, sizeof(event));
            event.conn_handle = conn->conn_idx;
            event.role = H2_PAL_BLE_ROLE_PERIPHERAL;
            event.peer_addr.type = h2_bk_ble_addr_type(conn->peer_addr_type);
            memcpy(event.peer_addr.value, conn->peer_addr, sizeof(event.peer_addr.value));
            h2_bk_ble_post(H2_PAL_SYSTEM_EVENT_TYPE_BLE_CONNECTED, &event, sizeof(event));
        }
    } else if (notice == BLE_5_DISCONNECT_EVENT) {
        const ble_discon_ind_t *disconn = (const ble_discon_ind_t *)param;
        /* A disconnect can still arrive when the stack omitted or delayed the
         * matching peripheral-connect notice.  Normalize the controller's
         * stopped connectable advertisement here as well so the upper layer
         * can always restart it after the session. */
        h2_bk_ble_mark_connectable_advertising_stopped();
        h2_pal_ble_disconnected_info_t event;
        memset(&event, 0, sizeof(event));
        event.conn_handle = disconn != NULL
            ? disconn->conn_idx
            : s_h2_bk_ble_peripheral_conn_handle;
        event.reason = disconn != NULL ? disconn->reason : 0;
        h2_bk_ble_post(H2_PAL_SYSTEM_EVENT_TYPE_BLE_DISCONNECTED, &event, sizeof(event));
        if (event.conn_handle == s_h2_bk_ble_peripheral_conn_handle) {
            s_h2_bk_ble_peripheral_conn_handle =
                H2_PAL_BLE_INVALID_CONN_HANDLE;
        }
    } else if (notice == BLE_5_INIT_CONNECT_EVENT) {
        const ble_conn_ind_t *conn = (const ble_conn_ind_t *)param;
        if (conn != NULL) {
            s_h2_bk_ble_conn_handle = conn->conn_idx;
            s_h2_bk_ble_init_started = 0;
            s_h2_bk_ble_init_connected = 1;
            memcpy(s_h2_bk_ble_peer_addr, conn->peer_addr, sizeof(s_h2_bk_ble_peer_addr));
            h2_pal_ble_connection_t event;
            memset(&event, 0, sizeof(event));
            event.conn_handle = conn->conn_idx;
            event.role = H2_PAL_BLE_ROLE_CENTRAL;
            event.peer_addr.type = h2_bk_ble_addr_type(conn->peer_addr_type);
            memcpy(event.peer_addr.value, conn->peer_addr, sizeof(event.peer_addr.value));
            h2_bk_ble_post(H2_PAL_SYSTEM_EVENT_TYPE_BLE_CONNECTED, &event, sizeof(event));
            h2_bk_ble_signal(H2_PAL_OK);
        }
    } else if (notice == BLE_5_INIT_DISCONNECT_EVENT) {
        const ble_discon_ind_t *disconn = (const ble_discon_ind_t *)param;
        h2_pal_ble_disconnected_info_t event;
        memset(&event, 0, sizeof(event));
        event.conn_handle = disconn != NULL ? disconn->conn_idx : s_h2_bk_ble_conn_handle;
        event.reason = disconn != NULL ? disconn->reason : 0;
        h2_bk_ble_post(H2_PAL_SYSTEM_EVENT_TYPE_BLE_DISCONNECTED, &event, sizeof(event));
        s_h2_bk_ble_conn_handle = H2_PAL_BLE_INVALID_CONN_HANDLE;
        s_h2_bk_ble_init_started = 0;
        s_h2_bk_ble_init_connected = 0;
        h2_bk_ble_signal(H2_PAL_OK);
    } else if (notice == BLE_5_INIT_CONNECT_FAILED_EVENT) {
        s_h2_bk_ble_conn_handle = H2_PAL_BLE_INVALID_CONN_HANDLE;
        s_h2_bk_ble_init_started = 0;
        s_h2_bk_ble_init_connected = 0;
        h2_bk_ble_signal(H2_PAL_ERR_IO);
    } else if (notice == BLE_5_MTU_CHANGE) {
        const ble_mtu_change_t *mtu = (const ble_mtu_change_t *)param;
        if (mtu != NULL) {
            h2_bk_ble_post_mtu_changed(mtu->conn_idx, mtu->mtu_size);
            h2_bk_ble_signal(H2_PAL_OK);
        }
    } else if (notice == BLE_5_GAP_CMD_CMP_EVENT) {
        const ble_cmd_cmp_evt_t *event = (const ble_cmd_cmp_evt_t *)param;
        if (event != NULL && event->cmd == BLE_SET_MAX_MTU) {
            os_printf(
                "H2_BK_BLE_LOCAL_MTU_SET status=%u requested_mtu=%u\n",
                (unsigned)event->status,
                (unsigned)H2_BK_BLE_LOCAL_MAX_MTU);
            h2_bk_ble_signal(h2_bk_ble_map_error(event->status));
        }
    } else if (notice == BLE_5_TX_DONE) {
        h2_bk_ble_signal_legacy_notify_done(H2_PAL_OK);
    } else if (notice == BLE_5_CONN_UPDATA_EVENT) {
        const ble_conn_param_t *conn = (const ble_conn_param_t *)param;
        if (conn != NULL) {
            const h2_pal_ble_connection_params_t params = {
                .interval_min_ms =
                    (uint16_t)((conn->intv_min * 125u) / 100u),
                .interval_max_ms =
                    (uint16_t)((conn->intv_min * 125u) / 100u),
                .latency = conn->con_latency,
                .supervision_timeout_ms =
                    (uint16_t)(conn->sup_to * 10u),
            };
            os_printf(
                "H2_BK_BLE_CONN_UPDATE status=0 interval_units=%u interval_ms=%u latency=%u timeout_units=%u\n",
                (unsigned)conn->intv_min,
                (unsigned)((conn->intv_min * 125u) / 100u),
                (unsigned)conn->con_latency,
                (unsigned)conn->sup_to);
            h2_bk_ble_post(
                H2_PAL_SYSTEM_EVENT_TYPE_BLE_CONNECTION_UPDATED,
                &params, sizeof(params));
        }
        h2_bk_ble_signal(H2_PAL_OK);
    } else if (notice == BLE_5_WRITE_EVENT) {
        const ble_write_req_t *write = (const ble_write_req_t *)param;
        if (write != NULL && write->prf_id == H2_BK_BLE_LEGACY_PRF_ID && s_h2_bk_ble_gatt_attached) {
            int value_index = h2_bk_ble_legacy_value_index(write->att_idx);
            int cccd_index = h2_bk_ble_legacy_cccd_index(write->att_idx);
            if (value_index >= 0) {
                size_t index = (size_t)value_index;
                uint16_t len = write->len;
                if (len > s_h2_bk_ble_value_max_len[index]) {
                    len = s_h2_bk_ble_value_max_len[index];
                }
                if (len > 0u && write->value != NULL) {
                    memcpy(s_h2_bk_ble_value[index], write->value, len);
                }
                s_h2_bk_ble_value_len[index] = len;
                if (s_h2_bk_ble_write[index] != NULL) {
                    h2_pal_ble_gatt_access_t access = {
                        .conn_handle = write->conn_idx,
                        .attr_handle = s_h2_bk_ble_value_handle[index],
                        .offset = 0u,
                    };
                    (void)s_h2_bk_ble_write[index](s_h2_bk_ble_gatt_user[index], &access,
                        s_h2_bk_ble_value[index], s_h2_bk_ble_value_len[index]);
                }
            } else if (cccd_index >= 0 && write->len >= sizeof(s_h2_bk_ble_cccd_value[0]) && write->value != NULL) {
                size_t index = (size_t)cccd_index;
                memcpy(s_h2_bk_ble_cccd_value[index], write->value, sizeof(s_h2_bk_ble_cccd_value[index]));
                h2_bk_ble_post_subscription_changed(write->conn_idx, index,
                    s_h2_bk_ble_cccd_value[index], sizeof(s_h2_bk_ble_cccd_value[index]));
            }
        }
    } else if (notice == BLE_5_READ_EVENT) {
        const ble_read_req_t *read = (const ble_read_req_t *)param;
        if (read != NULL && read->prf_id == H2_BK_BLE_LEGACY_PRF_ID && s_h2_bk_ble_gatt_attached) {
            int value_index = h2_bk_ble_legacy_value_index(read->att_idx);
            int cccd_index = h2_bk_ble_legacy_cccd_index(read->att_idx);
            if (value_index >= 0) {
                size_t index = (size_t)value_index;
                uint8_t *out = s_h2_bk_ble_read_scratch;
                size_t out_len = 0u;
                h2_pal_result_t rc = H2_PAL_OK;
                if (s_h2_bk_ble_read[index] != NULL) {
                    h2_pal_ble_gatt_access_t access = {
                        .conn_handle = read->conn_idx,
                        .attr_handle = s_h2_bk_ble_value_handle[index],
                        .offset = 0u,
                    };
                    rc = s_h2_bk_ble_read[index](s_h2_bk_ble_gatt_user[index], &access,
                        out, H2_BK_BLE_MAX_VALUE_LEN, &out_len);
                } else {
                    out_len = s_h2_bk_ble_value_len[index];
                    memcpy(out, s_h2_bk_ble_value[index], out_len);
                }
                if (rc == H2_PAL_OK) {
                    (void)bk_ble_read_response_value(
                        read->conn_idx,
                        (uint32_t)out_len,
                        out,
                        read->prf_id,
                        read->att_idx);
                }
            } else if (cccd_index >= 0) {
                size_t index = (size_t)cccd_index;
                (void)bk_ble_read_response_value(
                    read->conn_idx,
                    sizeof(s_h2_bk_ble_cccd_value[index]),
                    s_h2_bk_ble_cccd_value[index],
                    read->prf_id,
                    read->att_idx);
            }
        }
    }
}

static uint8_t h2_bk_ble_build_attr_db(size_t service_index) {
    bk_gatts_attr_db_t *attr_db = s_h2_bk_ble_attr_db[service_index];
    uint8_t *value_attr_index =
        s_h2_bk_ble_value_attr_index[service_index];
    uint8_t *cccd_attr_index =
        s_h2_bk_ble_cccd_attr_index[service_index];
    size_t first = h2_bk_ble_service_characteristic_first(service_index);
    size_t characteristic_count =
        s_h2_bk_ble_service_characteristic_count[service_index];
    memset(attr_db, 0, sizeof(s_h2_bk_ble_attr_db[service_index]));
    memset(value_attr_index, H2_BK_BLE_INVALID_ACTIVITY,
        sizeof(s_h2_bk_ble_value_attr_index[service_index]));
    memset(cccd_attr_index, H2_BK_BLE_INVALID_ACTIVITY,
        sizeof(s_h2_bk_ble_cccd_attr_index[service_index]));
    attr_db[0].att_desc.attr_type =
        H2_BK_GATT_ATTR_TYPE(BK_GATT_UUID_PRI_SERVICE);
    attr_db[0].att_desc.attr_content =
        s_h2_bk_ble_service_uuid[service_index];
    size_t attr_index = 1u;
    for (size_t i = 0u; i < characteristic_count; ++i) {
        size_t index = first + i;
        size_t value_index = attr_index++;
        value_attr_index[i] = (uint8_t)value_index;
        uint16_t properties = 0u;
        if ((s_h2_bk_ble_properties[index] & H2_PAL_BLE_GATT_PROPERTY_READ) != 0u) properties |= BK_GATT_CHAR_PROP_BIT_READ;
        if ((s_h2_bk_ble_properties[index] & H2_PAL_BLE_GATT_PROPERTY_WRITE) != 0u) properties |= BK_GATT_CHAR_PROP_BIT_WRITE;
        if ((s_h2_bk_ble_properties[index] & H2_PAL_BLE_GATT_PROPERTY_WRITE_NO_RSP) != 0u) properties |= BK_GATT_CHAR_PROP_BIT_WRITE_NR;
        if ((s_h2_bk_ble_properties[index] & H2_PAL_BLE_GATT_PROPERTY_NOTIFY) != 0u) properties |= BK_GATT_CHAR_PROP_BIT_NOTIFY;
        if ((s_h2_bk_ble_properties[index] & H2_PAL_BLE_GATT_PROPERTY_INDICATE) != 0u) properties |= BK_GATT_CHAR_PROP_BIT_INDICATE;
        uint16_t permissions = 0u;
        if ((s_h2_bk_ble_permissions[index] & H2_PAL_BLE_GATT_PERMISSION_READ) != 0u) permissions |= BK_GATT_PERM_READ;
        if ((s_h2_bk_ble_permissions[index] & H2_PAL_BLE_GATT_PERMISSION_WRITE) != 0u) permissions |= BK_GATT_PERM_WRITE;
        if ((s_h2_bk_ble_permissions[index] & H2_PAL_BLE_GATT_PERMISSION_READ_ENCRYPTED) != 0u) permissions |= BK_GATT_PERM_READ_ENCRYPTED;
        if ((s_h2_bk_ble_permissions[index] & H2_PAL_BLE_GATT_PERMISSION_WRITE_ENCRYPTED) != 0u) permissions |= BK_GATT_PERM_WRITE_ENCRYPTED;
        if ((s_h2_bk_ble_permissions[index] & H2_PAL_BLE_GATT_PERMISSION_READ_AUTHENTICATED) != 0u) permissions |= BK_GATT_PERM_READ_ENC_MITM;
        if ((s_h2_bk_ble_permissions[index] & H2_PAL_BLE_GATT_PERMISSION_WRITE_AUTHENTICATED) != 0u) permissions |= BK_GATT_PERM_WRITE_ENC_MITM;
        attr_db[value_index].att_desc.attr_type = H2_BK_GATT_ATTR_TYPE(BK_GATT_UUID_CHAR_DECLARE);
        attr_db[value_index].att_desc.attr_content = s_h2_bk_ble_char_uuid[index];
        attr_db[value_index].att_desc.value = H2_BK_GATT_ATTR_VALUE(s_h2_bk_ble_value_max_len[index], s_h2_bk_ble_value[index]);
        attr_db[value_index].att_desc.prop = properties;
        attr_db[value_index].att_desc.perm = permissions;
        attr_db[value_index].attr_control.auto_rsp = BK_GATT_RSP_BY_APP;
        if ((s_h2_bk_ble_properties[index] &
             (H2_PAL_BLE_GATT_PROPERTY_NOTIFY |
              H2_PAL_BLE_GATT_PROPERTY_INDICATE)) != 0u) {
            size_t cccd_index = attr_index++;
            cccd_attr_index[i] = (uint8_t)cccd_index;
            attr_db[cccd_index].att_desc.attr_type =
                H2_BK_GATT_ATTR_TYPE(BK_GATT_UUID_CHAR_CLIENT_CONFIG);
            attr_db[cccd_index].att_desc.value =
                H2_BK_GATT_ATTR_VALUE(
                    sizeof(s_h2_bk_ble_cccd_value[index]),
                    s_h2_bk_ble_cccd_value[index]);
            attr_db[cccd_index].att_desc.perm =
                BK_GATT_PERM_READ | BK_GATT_PERM_WRITE;
            attr_db[cccd_index].attr_control.auto_rsp =
                BK_GATT_AUTO_RSP;
        }
    }
    s_h2_bk_ble_attr_db_count[service_index] = (uint8_t)attr_index;
    return s_h2_bk_ble_attr_db_count[service_index];
}

static void h2_bk_ble_build_legacy_attr_db(void) {
    memset(s_h2_bk_ble_legacy_attr_db, 0, sizeof(s_h2_bk_ble_legacy_attr_db));
    s_h2_bk_ble_legacy_attr_db[H2_BK_BLE_LEGACY_IDX_SVC].uuid[0] = 0x00u;
    s_h2_bk_ble_legacy_attr_db[H2_BK_BLE_LEGACY_IDX_SVC].uuid[1] = 0x28u;
    s_h2_bk_ble_legacy_attr_db[H2_BK_BLE_LEGACY_IDX_SVC].perm = BK_BLE_PERM_SET(RD, ENABLE);

    for (size_t i = 0u;
         i < s_h2_bk_ble_service_characteristic_count[0];
         ++i) {
        size_t decl_index = 1u + 3u * i;
        size_t value_index = decl_index + 1u;
        size_t cccd_index = value_index + 1u;
        s_h2_bk_ble_legacy_attr_db[decl_index].uuid[0] = 0x03u;
        s_h2_bk_ble_legacy_attr_db[decl_index].uuid[1] = 0x28u;
        s_h2_bk_ble_legacy_attr_db[decl_index].perm = BK_BLE_PERM_SET(RD, ENABLE);
        s_h2_bk_ble_legacy_attr_db[value_index].uuid[0] =
            (uint8_t)(s_h2_bk_ble_char_uuid[i].uuid.uuid16 & 0xffu);
        s_h2_bk_ble_legacy_attr_db[value_index].uuid[1] =
            (uint8_t)(s_h2_bk_ble_char_uuid[i].uuid.uuid16 >> 8);
        uint16_t permissions = 0u;
        if ((s_h2_bk_ble_properties[i] & H2_PAL_BLE_GATT_PROPERTY_READ) != 0u) permissions |= BK_BLE_PERM_SET(RD, ENABLE);
        if ((s_h2_bk_ble_properties[i] & H2_PAL_BLE_GATT_PROPERTY_WRITE) != 0u) permissions |= BK_BLE_PERM_SET(WRITE_REQ, ENABLE);
        if ((s_h2_bk_ble_properties[i] & H2_PAL_BLE_GATT_PROPERTY_WRITE_NO_RSP) != 0u) permissions |= BK_BLE_PERM_SET(WRITE_COMMAND, ENABLE);
        if ((s_h2_bk_ble_properties[i] & H2_PAL_BLE_GATT_PROPERTY_NOTIFY) != 0u) permissions |= BK_BLE_PERM_SET(NTF, ENABLE);
        if ((s_h2_bk_ble_permissions[i] &
             H2_PAL_BLE_GATT_PERMISSION_READ_AUTHENTICATED) != 0u) {
            permissions |= BK_BLE_PERM_SET(RP, SEC_CON);
        } else if ((s_h2_bk_ble_permissions[i] &
                    H2_PAL_BLE_GATT_PERMISSION_READ_ENCRYPTED) != 0u) {
            permissions |= BK_BLE_PERM_SET(RP, UNAUTH);
        }
        if ((s_h2_bk_ble_permissions[i] &
             H2_PAL_BLE_GATT_PERMISSION_WRITE_AUTHENTICATED) != 0u) {
            permissions |= BK_BLE_PERM_SET(WP, SEC_CON);
        } else if ((s_h2_bk_ble_permissions[i] &
                    H2_PAL_BLE_GATT_PERMISSION_WRITE_ENCRYPTED) != 0u) {
            permissions |= BK_BLE_PERM_SET(WP, UNAUTH);
        }
        s_h2_bk_ble_legacy_attr_db[value_index].perm = permissions;
        s_h2_bk_ble_legacy_attr_db[value_index].ext_perm = BK_BLE_PERM_SET(RI, ENABLE) | BK_BLE_PERM_SET(UUID_LEN, UUID_16);
        s_h2_bk_ble_legacy_attr_db[value_index].max_size = s_h2_bk_ble_value_max_len[i];
        s_h2_bk_ble_legacy_attr_db[cccd_index].uuid[0] = 0x02u;
        s_h2_bk_ble_legacy_attr_db[cccd_index].uuid[1] = 0x29u;
        s_h2_bk_ble_legacy_attr_db[cccd_index].perm = BK_BLE_PERM_SET(RD, ENABLE) | BK_BLE_PERM_SET(WRITE_REQ, ENABLE);
    }

    memset(&s_h2_bk_ble_legacy_db_cfg, 0, sizeof(s_h2_bk_ble_legacy_db_cfg));
    s_h2_bk_ble_legacy_db_cfg.prf_task_id = H2_BK_BLE_LEGACY_PRF_ID;
    s_h2_bk_ble_legacy_db_cfg.uuid[0] =
        (uint8_t)(s_h2_bk_ble_service_uuid[0].uuid.uuid16 & 0xffu);
    s_h2_bk_ble_legacy_db_cfg.uuid[1] =
        (uint8_t)(s_h2_bk_ble_service_uuid[0].uuid.uuid16 >> 8);
    s_h2_bk_ble_legacy_db_cfg.att_db_nb =
        (uint8_t)(1u + 3u * s_h2_bk_ble_service_characteristic_count[0]);
    s_h2_bk_ble_legacy_db_cfg.start_hdl = 0u;
    s_h2_bk_ble_legacy_db_cfg.att_db = s_h2_bk_ble_legacy_attr_db;
    s_h2_bk_ble_legacy_db_cfg.svc_perm = BK_BLE_PERM_SET(SVC_UUID_LEN, UUID_16);
}

static int32_t h2_bk_ble_gatts_cb_unlocked(
    bk_gatts_cb_event_t event,
    bk_gatt_if_t gatts_if,
    bk_ble_gatts_cb_param_t *param);

static int32_t h2_bk_ble_gatts_cb(
    bk_gatts_cb_event_t event,
    bk_gatt_if_t gatts_if,
    bk_ble_gatts_cb_param_t *param) {
    int guarded = event == BK_GATTS_READ_EVT ||
                  event == BK_GATTS_WRITE_EVT ||
                  event == BK_GATTS_CONF_EVT ||
                  event == BK_GATTS_DISCONNECT_EVT;
    if (guarded && s_h2_bk_ble_gatt_mutex == NULL) return 0;
    if (guarded && rtos_lock_mutex(&s_h2_bk_ble_gatt_mutex) != kNoErr) return 0;
    int32_t rc = h2_bk_ble_gatts_cb_unlocked(event, gatts_if, param);
    if (guarded) (void)rtos_unlock_mutex(&s_h2_bk_ble_gatt_mutex);
    return rc;
}

static int32_t h2_bk_ble_gatts_cb_unlocked(
    bk_gatts_cb_event_t event,
    bk_gatt_if_t gatts_if,
    bk_ble_gatts_cb_param_t *param) {
    switch (event) {
    case BK_GATTS_REG_EVT:
        if (param != NULL && param->reg.status == BK_GATT_OK) {
            s_h2_bk_ble_gatts_if = param->reg.gatt_if;
            h2_bk_ble_signal(H2_PAL_OK);
        } else {
            h2_bk_ble_signal(H2_PAL_ERR_IO);
        }
        break;
    case BK_GATTS_CREAT_ATTR_TAB_EVT: {
        size_t service_index = s_h2_bk_ble_pending_service_index;
        if (service_index >= s_h2_bk_ble_service_count) {
            h2_bk_ble_signal(H2_PAL_ERR_INVALID_STATE);
            break;
        }
        size_t attr_count = s_h2_bk_ble_attr_db_count[service_index];
        size_t first = h2_bk_ble_service_characteristic_first(service_index);
        if (param != NULL && param->add_attr_tab.status == BK_GATT_OK &&
            param->add_attr_tab.handles != NULL && param->add_attr_tab.num_handle >= attr_count) {
            memcpy(s_h2_bk_ble_attr_handles[service_index],
                param->add_attr_tab.handles,
                attr_count * sizeof(s_h2_bk_ble_attr_handles[service_index][0]));
            s_h2_bk_ble_service_handle[service_index] =
                s_h2_bk_ble_attr_handles[service_index][0];
            for (size_t i = 0u;
                 i < s_h2_bk_ble_service_characteristic_count[service_index];
                 ++i) {
                size_t index = first + i;
                s_h2_bk_ble_value_handle[index] =
                    s_h2_bk_ble_attr_handles[service_index]
                                                 [s_h2_bk_ble_value_attr_index
                                                      [service_index][i]];
                s_h2_bk_ble_cccd_handle[index] =
                    s_h2_bk_ble_cccd_attr_index[service_index][i] ==
                            H2_BK_BLE_INVALID_ACTIVITY
                        ? H2_PAL_BLE_INVALID_ATTR_HANDLE
                        : s_h2_bk_ble_attr_handles[service_index]
                                                     [s_h2_bk_ble_cccd_attr_index
                                                          [service_index][i]];
            }
            h2_bk_ble_update_out_handles();
            h2_bk_ble_signal(H2_PAL_OK);
        } else {
            h2_bk_ble_signal(H2_PAL_ERR_IO);
        }
    }
        break;
    case BK_GATTS_START_EVT:
        h2_bk_ble_signal(h2_bk_ble_map_error(param != NULL ? param->start.status : BK_FAIL));
        break;
    case BK_GATTS_MTU_EVT:
        if (param != NULL) {
            h2_bk_ble_post_mtu_changed(param->mtu.conn_id, param->mtu.mtu);
        }
        break;
    case BK_GATTS_CONF_EVT:
        if (param != NULL) {
            h2_bk_ble_finish_gatts_tx_unlocked(
                param->conf.conn_id,
                param->conf.status == BK_GATT_OK
                    ? H2_PAL_OK
                    : H2_PAL_ERR_IO);
        }
        break;
    case BK_GATTS_READ_EVT:
        if (param != NULL && param->read.need_rsp && s_h2_bk_ble_gatt_attached) {
            int found_index = h2_bk_ble_value_index(param->read.handle);
            if (found_index < 0) break;
            size_t index = (size_t)found_index;
            uint8_t *out = s_h2_bk_ble_read_scratch;
            size_t out_len = 0u;
            h2_pal_ble_gatt_access_t access = {
                .conn_handle = param->read.conn_id,
                .attr_handle = param->read.handle,
                .offset = param->read.offset,
            };
            h2_pal_result_t rc = H2_PAL_OK;
            if (s_h2_bk_ble_read[index] != NULL) {
                rc = s_h2_bk_ble_read[index](s_h2_bk_ble_gatt_user[index], &access, out, H2_BK_BLE_MAX_VALUE_LEN, &out_len);
            } else {
                out_len = s_h2_bk_ble_value_len[index];
                memcpy(out, s_h2_bk_ble_value[index], out_len);
            }
            bk_gatt_rsp_t rsp;
            memset(&rsp, 0, sizeof(rsp));
            rsp.attr_value.handle = param->read.handle;
            rsp.attr_value.offset = param->read.offset;
            rsp.attr_value.value = out;
            rsp.attr_value.len = (uint16_t)out_len;
            (void)bk_ble_gatts_send_response(
                gatts_if,
                param->read.conn_id,
                param->read.trans_id,
                rc == H2_PAL_OK ? BK_GATT_OK : BK_GATT_ERR_UNLIKELY,
                &rsp);
        }
        break;
    case BK_GATTS_WRITE_EVT:
        if (param != NULL && s_h2_bk_ble_gatt_attached) {
            int found_value = h2_bk_ble_value_index(param->write.handle);
            int found_cccd = h2_bk_ble_cccd_index(param->write.handle);
            uint8_t *response_value = NULL;
            uint16_t response_len = 0u;
            if (found_value >= 0) {
                size_t index = (size_t)found_value;
                uint16_t len = param->write.len;
                if (len > s_h2_bk_ble_value_max_len[index]) {
                    len = s_h2_bk_ble_value_max_len[index];
                }
                if (len > 0u && param->write.value != NULL) {
                    memcpy(s_h2_bk_ble_value[index], param->write.value, len);
                }
                s_h2_bk_ble_value_len[index] = len;
                if (s_h2_bk_ble_write[index] != NULL) {
                    h2_pal_ble_gatt_access_t access = {
                        .conn_handle = param->write.conn_id,
                        .attr_handle = param->write.handle,
                        .offset = param->write.offset,
                    };
                    (void)s_h2_bk_ble_write[index](s_h2_bk_ble_gatt_user[index], &access,
                        s_h2_bk_ble_value[index], s_h2_bk_ble_value_len[index]);
                }
                response_value = s_h2_bk_ble_value[index];
                response_len = len;
            } else if (found_cccd >= 0) {
                size_t index = (size_t)found_cccd;
                size_t len = param->write.len;
                if (len > sizeof(s_h2_bk_ble_cccd_value[index])) {
                    len = sizeof(s_h2_bk_ble_cccd_value[index]);
                }
                memset(s_h2_bk_ble_cccd_value[index], 0, sizeof(s_h2_bk_ble_cccd_value[index]));
                if (len > 0u && param->write.value != NULL) {
                    memcpy(s_h2_bk_ble_cccd_value[index], param->write.value, len);
                }
                response_value = s_h2_bk_ble_cccd_value[index];
                response_len = (uint16_t)len;
            }
            if (param->write.need_rsp) {
                bk_gatt_rsp_t rsp;
                memset(&rsp, 0, sizeof(rsp));
                rsp.attr_value.auth_req = BK_GATT_AUTH_REQ_NONE;
                rsp.attr_value.handle = param->write.handle;
                rsp.attr_value.offset = param->write.offset;
                rsp.attr_value.value = response_value;
                rsp.attr_value.len = response_len;
                (void)bk_ble_gatts_send_response(
                    gatts_if,
                    param->write.conn_id,
                    param->write.trans_id,
                    response_value != NULL ? BK_GATT_OK : BK_GATT_REQ_NOT_SUPPORTED,
                    &rsp);
            }
            if (found_cccd >= 0) {
                size_t index = (size_t)found_cccd;
                h2_bk_ble_post_subscription_changed(param->write.conn_id, index,
                    s_h2_bk_ble_cccd_value[index], sizeof(s_h2_bk_ble_cccd_value[index]));
            }
        }
        break;
    case BK_GATTS_CONNECT_EVT:
        if (param != NULL) {
            /* A successful peripheral connection terminates connectable
             * advertising in the controller. EtherMind does not reliably
             * emit the GAP advertising-terminated callback, so mirror that
             * physical transition before the disconnect path restarts it. */
            h2_bk_ble_mark_connectable_advertising_stopped();
            s_h2_bk_ble_peripheral_conn_handle = param->connect.conn_id;
            memcpy(
                s_h2_bk_ble_peripheral_peer_addr,
                param->connect.remote_bda,
                sizeof(s_h2_bk_ble_peripheral_peer_addr));
            os_printf(
                "H2_BK_BLE_GATTS_CONNECTED conn=%u\n",
                (unsigned)param->connect.conn_id);
            h2_pal_ble_connection_t connection;
            memset(&connection, 0, sizeof(connection));
            connection.conn_handle = param->connect.conn_id;
            connection.role = H2_PAL_BLE_ROLE_PERIPHERAL;
            connection.peer_addr.type =
                s_h2_bk_ble_peripheral_peer_addr_type;
            connection.mtu = H2_BK_BLE_LOCAL_MAX_MTU;
            memcpy(
                connection.peer_addr.value,
                param->connect.remote_bda,
                sizeof(connection.peer_addr.value));
            h2_bk_ble_post(
                H2_PAL_SYSTEM_EVENT_TYPE_BLE_CONNECTED,
                &connection,
                sizeof(connection));
        }
        break;
    case BK_GATTS_DISCONNECT_EVT:
        if (param != NULL) {
            /* EtherMind may report a failed/short-lived session only through
             * GATTS_DISCONNECT.  Clear the stale advertising state before the
             * upper-layer disconnect handler schedules a restart. */
            h2_bk_ble_mark_connectable_advertising_stopped();
            os_printf(
                "H2_BK_BLE_GATTS_DISCONNECTED conn=%u reason=%u\n",
                (unsigned)param->disconnect.conn_id,
                (unsigned)param->disconnect.reason);
            h2_pal_ble_disconnected_info_t disconnected;
            memset(&disconnected, 0, sizeof(disconnected));
            disconnected.conn_handle = param->disconnect.conn_id;
            disconnected.peer_addr.type =
                s_h2_bk_ble_peripheral_peer_addr_type;
            memcpy(
                disconnected.peer_addr.value,
                param->disconnect.remote_bda,
                sizeof(disconnected.peer_addr.value));
            disconnected.reason = param->disconnect.reason;
            h2_bk_ble_finish_gatts_tx_unlocked(
                param->disconnect.conn_id,
                H2_PAL_ERR_CLOSED);
            h2_bk_ble_post(
                H2_PAL_SYSTEM_EVENT_TYPE_BLE_DISCONNECTED,
                &disconnected,
                sizeof(disconnected));
        }
        s_h2_bk_ble_legacy_notify_in_flight = 0u;
        if (param != NULL &&
            param->disconnect.conn_id == s_h2_bk_ble_peripheral_conn_handle) {
            s_h2_bk_ble_peripheral_conn_handle =
                H2_PAL_BLE_INVALID_CONN_HANDLE;
        }
        break;
    default:
        break;
    }
    return 0;
}

static int32_t h2_bk_ble_gattc_cb(bk_gattc_cb_event_t event, bk_gatt_if_t gattc_if, bk_ble_gattc_cb_param_t *param) {
    switch (event) {
    case BK_GATTC_REG_EVT:
        if (param != NULL && param->reg.status == BK_GATT_OK) {
            s_h2_bk_ble_gattc_if = param->reg.gatt_if;
            h2_bk_ble_signal(H2_PAL_OK);
        } else {
            h2_bk_ble_signal(H2_PAL_ERR_IO);
        }
        break;
    case BK_GATTC_CFG_MTU_EVT:
        if (param != NULL && param->cfg_mtu.status == BK_GATT_OK) {
            h2_bk_ble_post_mtu_changed(param->cfg_mtu.conn_id, param->cfg_mtu.mtu);
            h2_bk_ble_signal(H2_PAL_OK);
        } else {
            h2_bk_ble_signal(H2_PAL_ERR_IO);
        }
        break;
    case BK_GATTC_CONNECT_EVT:
        if (param != NULL) {
            s_h2_bk_ble_conn_handle = param->connect.conn_id;
            s_h2_bk_ble_gattc_connected = 1;
            memcpy(s_h2_bk_ble_peer_addr, param->connect.remote_bda, sizeof(s_h2_bk_ble_peer_addr));
            h2_pal_ble_connection_t connection;
            memset(&connection, 0, sizeof(connection));
            connection.conn_handle = param->connect.conn_id;
            connection.role = H2_PAL_BLE_ROLE_CENTRAL;
            connection.peer_addr.type = s_h2_bk_ble_peer_addr_type;
            memcpy(
                connection.peer_addr.value,
                param->connect.remote_bda,
                sizeof(connection.peer_addr.value));
            h2_bk_ble_post(
                H2_PAL_SYSTEM_EVENT_TYPE_BLE_CONNECTED,
                &connection,
                sizeof(connection));
            h2_bk_ble_signal(H2_PAL_OK);
        }
        break;
    case BK_GATTC_DISCONNECT_EVT:
        s_h2_bk_ble_gattc_connected = 0;
        break;
    case BK_GATTC_DIS_RES_SERVICE_EVT:
        if (param != NULL && s_h2_bk_ble_discovery_request.kind == H2_PAL_BLE_GATT_DISCOVERY_SERVICE) {
            for (uint32_t i = 0u; i < param->dis_res_service.count; ++i) {
                if (s_h2_bk_ble_discovery_count >= s_h2_bk_ble_discovery_max_entries) {
                    break;
                }
                const bk_bt_uuid_t *uuid = &param->dis_res_service.array[i].srvc_id.uuid;
                if (uuid->len != BK_UUID_LEN_16) {
                    continue;
                }
                uint16_t filter = h2_bk_ble_uuid16_value(&s_h2_bk_ble_discovery_request.uuid_filter);
                if (filter != 0u && uuid->uuid.uuid16 != filter) {
                    continue;
                }
                size_t idx = s_h2_bk_ble_discovery_count++;
                h2_pal_ble_gatt_discovery_entry_t *entry = &s_h2_bk_ble_discovery_entries[idx];
                memset(entry, 0, sizeof(*entry));
                entry->kind = H2_PAL_BLE_GATT_DISCOVERY_SERVICE;
                entry->start_handle = param->dis_res_service.array[i].start_handle;
                entry->end_handle = param->dis_res_service.array[i].end_handle;
                h2_bk_ble_copy_uuid16(uuid->uuid.uuid16, &entry->uuid, s_h2_bk_ble_discovery_uuid_data[idx]);
            }
        }
        break;
    case BK_GATTC_DIS_RES_CHAR_EVT:
        if (param != NULL && s_h2_bk_ble_discovery_request.kind == H2_PAL_BLE_GATT_DISCOVERY_CHARACTERISTIC) {
            for (uint32_t i = 0u; i < param->dis_res_char.count; ++i) {
                if (s_h2_bk_ble_discovery_count >= s_h2_bk_ble_discovery_max_entries) {
                    break;
                }
                const bk_bt_uuid_t *uuid = &param->dis_res_char.array[i].uuid.uuid;
                if (uuid->len != BK_UUID_LEN_16) {
                    continue;
                }
                uint16_t filter = h2_bk_ble_uuid16_value(&s_h2_bk_ble_discovery_request.uuid_filter);
                if (filter != 0u && uuid->uuid.uuid16 != filter) {
                    continue;
                }
                uint16_t value_handle = param->dis_res_char.array[i].char_value_handle;
                if (value_handle < s_h2_bk_ble_discovery_request.start_handle ||
                    value_handle > s_h2_bk_ble_discovery_request.end_handle) {
                    continue;
                }
                size_t idx = s_h2_bk_ble_discovery_count++;
                h2_pal_ble_gatt_discovery_entry_t *entry = &s_h2_bk_ble_discovery_entries[idx];
                memset(entry, 0, sizeof(*entry));
                entry->kind = H2_PAL_BLE_GATT_DISCOVERY_CHARACTERISTIC;
                entry->start_handle = param->dis_res_char.array[i].start_handle;
                entry->end_handle = param->dis_res_char.array[i].end_handle;
                entry->value_handle = value_handle;
                entry->properties = h2_bk_ble_gatt_properties_from_bk(
                    param->dis_res_char.array[i].prop);
                h2_bk_ble_copy_uuid16(uuid->uuid.uuid16, &entry->uuid, s_h2_bk_ble_discovery_uuid_data[idx]);
            }
        }
        break;
    case BK_GATTC_DIS_RES_CHAR_DESC_EVT:
        if (param != NULL && s_h2_bk_ble_discovery_request.kind == H2_PAL_BLE_GATT_DISCOVERY_DESCRIPTOR) {
            for (uint32_t i = 0u; i < param->dis_res_char_desc.count; ++i) {
                if (s_h2_bk_ble_discovery_count >= s_h2_bk_ble_discovery_max_entries) {
                    break;
                }
                const bk_bt_uuid_t *uuid = &param->dis_res_char_desc.array[i].uuid.uuid;
                if (uuid->len != BK_UUID_LEN_16) {
                    continue;
                }
                uint16_t filter = h2_bk_ble_uuid16_value(&s_h2_bk_ble_discovery_request.uuid_filter);
                if (filter != 0u && uuid->uuid.uuid16 != filter) {
                    continue;
                }
                uint16_t desc_handle =
                    param->dis_res_char_desc.array[i].desc_handle;
                if (desc_handle < s_h2_bk_ble_discovery_request.start_handle ||
                    desc_handle > s_h2_bk_ble_discovery_request.end_handle) {
                    continue;
                }
                size_t idx = s_h2_bk_ble_discovery_count++;
                h2_pal_ble_gatt_discovery_entry_t *entry = &s_h2_bk_ble_discovery_entries[idx];
                memset(entry, 0, sizeof(*entry));
                entry->kind = H2_PAL_BLE_GATT_DISCOVERY_DESCRIPTOR;
                entry->start_handle = param->dis_res_char_desc.array[i].char_handle;
                entry->value_handle = desc_handle;
                h2_bk_ble_copy_uuid16(uuid->uuid.uuid16, &entry->uuid, s_h2_bk_ble_discovery_uuid_data[idx]);
            }
        }
        break;
    case BK_GATTC_DIS_SRVC_CMPL_EVT:
        if (param != NULL &&
            (param->dis_srvc_cmpl.status == BK_GATT_OK ||
             param->dis_srvc_cmpl.status == BK_GATT_NOT_FOUND)) {
            h2_bk_ble_signal(H2_PAL_OK);
        } else {
            h2_bk_ble_signal(h2_bk_ble_map_error(
                param != NULL ? param->dis_srvc_cmpl.status : BK_FAIL));
        }
        break;
    case BK_GATTC_READ_CHAR_EVT:
    case BK_GATTC_READ_DESCR_EVT:
        if (param != NULL && param->read.status == BK_GATT_OK && param->read.handle == s_h2_bk_ble_pending_handle) {
            s_h2_bk_ble_read_out_len = param->read.value_len;
            if (s_h2_bk_ble_read_out_len > s_h2_bk_ble_read_out_size) {
                s_h2_bk_ble_read_out_len = s_h2_bk_ble_read_out_size;
            }
            if (s_h2_bk_ble_read_out_len > 0u && s_h2_bk_ble_read_out != NULL && param->read.value != NULL) {
                memcpy(s_h2_bk_ble_read_out, param->read.value, s_h2_bk_ble_read_out_len);
            }
            h2_bk_ble_signal(H2_PAL_OK);
        } else {
            h2_bk_ble_signal(H2_PAL_ERR_IO);
        }
        break;
    case BK_GATTC_WRITE_CHAR_EVT:
    case BK_GATTC_WRITE_DESCR_EVT:
        if (param != NULL && param->write.handle == s_h2_bk_ble_pending_handle) {
            h2_bk_ble_signal(h2_bk_ble_map_error(param->write.status));
        }
        break;
    case BK_GATTC_NOTIFY_EVT:
        if (param != NULL) {
            h2_pal_ble_gatt_client_value_t value;
            memset(&value, 0, sizeof(value));
            value.conn_handle = param->notify.conn_id;
            value.attr_handle = param->notify.handle;
            value.value_len = param->notify.value_len;
            if (value.value_len > sizeof(value.value)) {
                value.value_len = sizeof(value.value);
            }
            if (value.value_len > 0u && param->notify.value != NULL) {
                memcpy(value.value, param->notify.value, value.value_len);
            } else {
                value.value_len = 0u;
            }
            h2_bk_ble_post(param->notify.is_notify
                               ? H2_PAL_SYSTEM_EVENT_TYPE_BLE_GATT_CLIENT_NOTIFICATION
                               : H2_PAL_SYSTEM_EVENT_TYPE_BLE_GATT_CLIENT_INDICATION,
                &value,
                sizeof(value));
        }
        break;
    default:
        break;
    }
    (void)gattc_if;
    return 0;
}

static h2_pal_result_t h2_bk_ble_ensure_gap_registered(void) {
    h2_pal_result_t rc = h2_bk_ble_ensure_sem();
    if (rc != H2_PAL_OK) {
        return rc;
    }
    if (!s_h2_bk_ble_gap_registered) {
        bk_ble_gap_register_callback(h2_bk_ble_gap_cb);
        s_h2_bk_ble_gap_registered = 1;
    }
    return H2_PAL_OK;
}

static h2_pal_result_t h2_bk_ble_ensure_gatts_registered(void) {
    h2_pal_result_t rc = h2_bk_ble_ensure_gap_registered();
    if (rc != H2_PAL_OK) {
        return rc;
    }
    if (!s_h2_bk_ble_gatts_registered) {
        if (bk_ble_gatts_register_callback(h2_bk_ble_gatts_cb) != BK_OK) {
            return H2_PAL_ERR_IO;
        }
        if (bk_ble_gatts_app_register(H2_BK_BLE_GATTS_APP_ID) != BK_OK) {
            return H2_PAL_ERR_IO;
        }
        rc = h2_bk_ble_wait(H2_BK_BLE_TIMEOUT_MS);
        if (rc != H2_PAL_OK) {
            return rc;
        }
        s_h2_bk_ble_gatts_registered = 1;
    }
    return H2_PAL_OK;
}

static h2_pal_result_t h2_bk_ble_ensure_gattc_registered(void) {
    h2_pal_result_t rc = h2_bk_ble_ensure_gap_registered();
    if (rc != H2_PAL_OK) {
        return rc;
    }
    if (!s_h2_bk_ble_gattc_registered) {
        if (bk_ble_gattc_register_callback(h2_bk_ble_gattc_cb) != BK_OK) {
            return H2_PAL_ERR_IO;
        }
        if (bk_ble_gattc_app_register(H2_BK_BLE_GATTC_APP_ID) != BK_OK) {
            return H2_PAL_ERR_IO;
        }
        rc = h2_bk_ble_wait(H2_BK_BLE_TIMEOUT_MS);
        if (rc != H2_PAL_OK) {
            return rc;
        }
        s_h2_bk_ble_gattc_registered = 1;
    }
    return H2_PAL_OK;
}

static h2_pal_result_t h2_bk_ble_set_local_mtu(void) {
    if (bk_ble_get_host_stack_type() != BK_BLE_HOST_STACK_TYPE_ETHERMIND) {
        if (s_h2_bk_ble_service_count > 1u) {
            return H2_PAL_ERR_UNSUPPORTED;
        }
        if (s_h2_bk_ble_service_count == 1u &&
            s_h2_bk_ble_service_uuid[0].len != BK_UUID_LEN_16) {
            return H2_PAL_ERR_UNSUPPORTED;
        }
        for (size_t i = 0u;
             s_h2_bk_ble_service_count == 1u &&
             i < s_h2_bk_ble_service_characteristic_count[0];
             ++i) {
            if (s_h2_bk_ble_char_uuid[i].len != BK_UUID_LEN_16) {
                return H2_PAL_ERR_UNSUPPORTED;
            }
        }
        bk_ble_set_notice_cb(h2_bk_ble_notice_cb);
        h2_pal_result_t rc = h2_bk_ble_ensure_sem();
        if (rc != H2_PAL_OK) {
            return rc;
        }
        h2_bk_ble_drain_signals();
        rc = h2_bk_ble_map_error(bk_ble_set_max_mtu(H2_BK_BLE_LOCAL_MAX_MTU));
        if (rc != H2_PAL_OK) {
            return rc;
        }
        return h2_bk_ble_wait(H2_BK_BLE_TIMEOUT_MS);
    }
    h2_pal_result_t rc = h2_bk_ble_map_error(bk_ble_gatt_set_local_mtu(H2_BK_BLE_LOCAL_MAX_MTU));
    os_printf(
        "H2_BK_BLE_LOCAL_MTU_SET status=%d requested_mtu=%u\n",
        rc,
        (unsigned)H2_BK_BLE_LOCAL_MAX_MTU);
    return rc;
}

static h2_pal_result_t h2_bk_ble_start_configured_services(void) {
    if (s_h2_bk_ble_service_count == 0u) {
        return H2_PAL_OK;
    }
    if (bk_ble_get_host_stack_type() != BK_BLE_HOST_STACK_TYPE_ETHERMIND) {
        if (s_h2_bk_ble_service_count != 1u) {
            return H2_PAL_ERR_UNSUPPORTED;
        }
        if (s_h2_bk_ble_legacy_service_created) {
            s_h2_bk_ble_service_started[0] = 1;
            return H2_PAL_OK;
        }
        h2_bk_ble_build_legacy_attr_db();
        bk_ble_set_notice_cb(h2_bk_ble_notice_cb);
        h2_pal_result_t rc = h2_bk_ble_ensure_sem();
        if (rc != H2_PAL_OK) {
            return rc;
        }
        rc = h2_bk_ble_map_error(bk_ble_create_db(&s_h2_bk_ble_legacy_db_cfg));
        if (rc != H2_PAL_OK) {
            return rc;
        }
        return h2_bk_ble_wait(H2_BK_BLE_TIMEOUT_MS);
    }
    h2_pal_result_t rc = h2_bk_ble_ensure_gatts_registered();
    if (rc != H2_PAL_OK) {
        return rc;
    }
    for (size_t service_index = 0u;
         service_index < s_h2_bk_ble_service_count;
         ++service_index) {
        if (s_h2_bk_ble_service_started[service_index]) {
            continue;
        }
        uint8_t attr_count = h2_bk_ble_build_attr_db(service_index);
        uint32_t max_attr_count =
            (uint32_t)attr_count +
            (uint32_t)s_h2_bk_ble_service_characteristic_count[service_index];
        s_h2_bk_ble_pending_service_index = service_index;
        if (bk_ble_gatts_create_attr_tab(
                s_h2_bk_ble_attr_db[service_index],
                s_h2_bk_ble_gatts_if,
                attr_count,
                max_attr_count) != BK_OK) {
            s_h2_bk_ble_pending_service_index =
                H2_BK_BLE_MAX_GATT_SERVICES;
            return H2_PAL_ERR_IO;
        }
        rc = h2_bk_ble_wait(H2_BK_BLE_TIMEOUT_MS);
        s_h2_bk_ble_pending_service_index = H2_BK_BLE_MAX_GATT_SERVICES;
        if (rc != H2_PAL_OK) {
            return rc;
        }
        if (bk_ble_gatts_start_service(
                s_h2_bk_ble_service_handle[service_index]) != BK_OK) {
            return H2_PAL_ERR_IO;
        }
        rc = h2_bk_ble_wait(H2_BK_BLE_TIMEOUT_MS);
        if (rc != H2_PAL_OK) {
            return rc;
        }
        s_h2_bk_ble_service_started[service_index] = 1;
    }
    return H2_PAL_OK;
}

static h2_pal_result_t h2_bk_ble_start(h2_pal_ble_t *ble) {
    (void)ble;
    if (s_h2_bk_ble_started) {
        return H2_PAL_OK;
    }
    h2_pal_result_t rc = h2_bk_ble_ensure_adv_mutex();
    if (rc != H2_PAL_OK) {
        return rc;
    }
    rc = h2_bk_ble_ensure_gap_registered();
    if (rc != H2_PAL_OK) {
        return rc;
    }
    rc = h2_bk_ble_set_local_mtu();
    if (rc != H2_PAL_OK) {
        return rc;
    }
    rc = h2_bk_ble_start_configured_services();
    if (rc != H2_PAL_OK) {
        return rc;
    }
    h2_bk_ble_clear_out_handle_ptrs();
    s_h2_bk_ble_started = 1;
    h2_bk_ble_post(H2_PAL_SYSTEM_EVENT_TYPE_BLE_HOST_STARTED, NULL, 0u);
    return H2_PAL_OK;
}

static h2_pal_result_t h2_bk_ble_stop(h2_pal_ble_t *ble) {
    (void)ble;
    if (s_h2_bk_ble_gatt_mutex != NULL &&
        rtos_lock_mutex(&s_h2_bk_ble_gatt_mutex) == kNoErr) {
        if (s_h2_bk_ble_gatts_tx.kind != H2_BK_BLE_GATTS_TX_IDLE) {
            h2_bk_ble_finish_gatts_tx_unlocked(
                s_h2_bk_ble_gatts_tx.conn_handle,
                H2_PAL_ERR_CLOSED);
        }
        (void)rtos_unlock_mutex(&s_h2_bk_ble_gatt_mutex);
    }
    (void)h2_bk_ble_cleanup_scan();
    if (h2_bk_ble_lock_adv() == H2_PAL_OK) {
        for (size_t i = 0u; i < H2_BK_BLE_ADV_SET_COUNT; ++i) {
            if (s_h2_bk_ble_adv_sets[i].allocated) {
                (void)h2_bk_ble_cleanup_adv_set(
                    &s_h2_bk_ble_adv_sets[i]);
            }
        }
        h2_bk_ble_unlock_adv();
    }
    (void)h2_bk_ble_cleanup_advertising();
    (void)h2_bk_ble_cleanup_initiator();
    s_h2_bk_ble_conn_handle = H2_PAL_BLE_INVALID_CONN_HANDLE;
    s_h2_bk_ble_peripheral_conn_handle = H2_PAL_BLE_INVALID_CONN_HANDLE;
    s_h2_bk_ble_hci_handle = H2_PAL_BLE_INVALID_CONN_HANDLE;
    s_h2_bk_ble_peripheral_hci_handle = H2_PAL_BLE_INVALID_CONN_HANDLE;
    s_h2_bk_ble_gattc_connected = 0;
    s_h2_bk_ble_adv_data_len = 0u;
    s_h2_bk_ble_adv_data_staged = 0;
    s_h2_bk_ble_legacy_adv_data_len = 0u;
    s_h2_bk_ble_legacy_scan_rsp_data_len = 0u;
    s_h2_bk_ble_legacy_adv_data_valid = 0;
    memset(s_h2_bk_ble_adv_sets, 0, sizeof(s_h2_bk_ble_adv_sets));
    s_h2_bk_ble_started = 0;
    memset(s_h2_bk_ble_service_started, 0,
        sizeof(s_h2_bk_ble_service_started));
    h2_bk_ble_post(H2_PAL_SYSTEM_EVENT_TYPE_BLE_HOST_STOPPED, NULL, 0u);
    return H2_PAL_OK;
}

static h2_pal_result_t h2_bk_ble_set_adv_data(h2_pal_ble_t *ble, const h2_pal_ble_adv_data_t *data) {
    (void)ble;
    if (!s_h2_bk_ble_started || data == NULL) {
        return data == NULL ? H2_PAL_ERR_INVALID_ARG : H2_PAL_ERR_INVALID_STATE;
    }
    if ((data->service_uuid_count > 0u && data->service_uuids == NULL) ||
        (data->manufacturer_data.len > 0u && data->manufacturer_data.data == NULL) ||
        (data->service_data.len > 0u && data->service_data.data == NULL)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    uint8_t encoded[H2_PAL_BLE_EXT_ADV_DATA_MAX_LEN];
    size_t len = 0u;
    h2_pal_result_t rc = h2_bk_ble_encode_adv_data(
        data,
        encoded,
        sizeof(encoded),
        &len,
        NULL,
        0u,
        NULL);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    memcpy(s_h2_bk_ble_adv_data, encoded, len);
    s_h2_bk_ble_adv_data_len = len;
    s_h2_bk_ble_legacy_adv_data_len = 0u;
    s_h2_bk_ble_legacy_scan_rsp_data_len = 0u;
    s_h2_bk_ble_legacy_adv_data_valid =
        h2_bk_ble_encode_adv_data(
            data,
            s_h2_bk_ble_legacy_adv_data,
            sizeof(s_h2_bk_ble_legacy_adv_data),
            &s_h2_bk_ble_legacy_adv_data_len,
            s_h2_bk_ble_legacy_scan_rsp_data,
            sizeof(s_h2_bk_ble_legacy_scan_rsp_data),
            &s_h2_bk_ble_legacy_scan_rsp_data_len) == H2_PAL_OK;
    s_h2_bk_ble_adv_data_staged = 1;
    return H2_PAL_OK;
}

static h2_pal_result_t h2_bk_ble_start_advertising(h2_pal_ble_t *ble, const h2_pal_ble_adv_params_t *params) {
    (void)ble;
    if (!s_h2_bk_ble_started || params == NULL) {
        return params == NULL ? H2_PAL_ERR_INVALID_ARG : H2_PAL_ERR_INVALID_STATE;
    }
    if (!s_h2_bk_ble_adv_data_staged || s_h2_bk_ble_adv_started) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    if (params->type == H2_PAL_BLE_ADV_TYPE_LEGACY && !s_h2_bk_ble_legacy_adv_data_valid) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (s_h2_bk_ble_adv_created) {
        h2_pal_result_t remove_rc = h2_bk_ble_cleanup_advertising();
        if (remove_rc != H2_PAL_OK) {
            return remove_rc;
        }
    }

    if (!h2_bk_ble_uses_ethermind()) {
        if (params->type == H2_PAL_BLE_ADV_TYPE_EXTENDED) {
            return H2_PAL_ERR_UNSUPPORTED;
        }
        bk_ble_set_notice_cb(h2_bk_ble_notice_cb);
        uint8_t activity = bk_ble_get_idle_actv_idx_handle();
        if (activity == H2_BK_BLE_INVALID_ACTIVITY) {
            return H2_PAL_ERR_UNAVAILABLE;
        }
        ble_adv_param_t adv_params;
        memset(&adv_params, 0, sizeof(adv_params));
        adv_params.own_addr_type = OWN_ADDR_TYPE_PUBLIC_ADDR;
        adv_params.adv_type = ADV_TYPE_LEGACY;
        adv_params.chnl_map = ADV_ALL_CHNLS;
        adv_params.adv_prop = ADV_PROP_PROP_LEGACY_BIT | ADV_PROP_SCANNABLE_BIT;
        if (params->mode == H2_PAL_BLE_ADV_MODE_CONNECTABLE) {
            adv_params.adv_prop |= ADV_PROP_CONNECTABLE_BIT;
        }
        adv_params.adv_intv_min = h2_bk_ble_ms_to_units625(params->interval_min_ms);
        adv_params.adv_intv_max = h2_bk_ble_ms_to_units625(params->interval_max_ms);
        adv_params.prim_phy = PHY_TYPE_LE_1M;
        adv_params.second_phy = PHY_TYPE_LE_1M;
        h2_pal_result_t rc = h2_bk_ble_map_error(
            bk_ble_create_advertising(activity, &adv_params, h2_bk_ble_cmd_cb));
        if (rc == H2_PAL_OK) {
            rc = h2_bk_ble_wait(H2_BK_BLE_TIMEOUT_MS);
        }
        if (rc != H2_PAL_OK) {
            return rc;
        }
        s_h2_bk_ble_adv_activity = activity;
        s_h2_bk_ble_adv_created = 1;
        rc = h2_bk_ble_map_error(bk_ble_set_adv_data(
            activity,
            s_h2_bk_ble_legacy_adv_data,
            (uint8_t)s_h2_bk_ble_legacy_adv_data_len,
            h2_bk_ble_cmd_cb));
        if (rc == H2_PAL_OK) {
            rc = h2_bk_ble_wait(H2_BK_BLE_TIMEOUT_MS);
        }
        if (rc == H2_PAL_OK && s_h2_bk_ble_legacy_scan_rsp_data_len > 0u) {
            rc = h2_bk_ble_map_error(bk_ble_set_scan_rsp_data(
                activity,
                s_h2_bk_ble_legacy_scan_rsp_data,
                (uint8_t)s_h2_bk_ble_legacy_scan_rsp_data_len,
                h2_bk_ble_cmd_cb));
            if (rc == H2_PAL_OK) {
                rc = h2_bk_ble_wait(H2_BK_BLE_TIMEOUT_MS);
            }
        }
        if (rc == H2_PAL_OK) {
            rc = h2_bk_ble_map_error(bk_ble_start_advertising(
                activity,
                0u,
                h2_bk_ble_cmd_cb));
        }
        if (rc == H2_PAL_OK) {
            rc = h2_bk_ble_wait(H2_BK_BLE_TIMEOUT_MS);
        }
        if (rc == H2_PAL_OK) {
            s_h2_bk_ble_adv_started = 1;
            h2_bk_ble_post(H2_PAL_SYSTEM_EVENT_TYPE_BLE_ADVERTISING_STARTED, NULL, 0u);
        } else {
            (void)h2_bk_ble_cleanup_advertising();
        }
        return rc;
    }

    bk_ble_gap_ext_adv_params_t adv_params;
    h2_bk_ble_fill_ext_adv_params(params, &adv_params);

    s_h2_bk_ble_adv_activity = H2_BK_BLE_ADV_INSTANCE;
    h2_bk_ble_drain_signals();
    h2_pal_result_t rc = h2_bk_ble_map_error(
        bk_ble_gap_set_adv_params(s_h2_bk_ble_adv_activity, &adv_params));
    if (rc == H2_PAL_OK) {
        rc = h2_bk_ble_wait(H2_BK_BLE_TIMEOUT_MS);
    }
    if (rc != H2_PAL_OK) {
        h2_bk_ble_clear_adv_state();
        return rc;
    }
    s_h2_bk_ble_adv_created = 1;

    if (params->type == H2_PAL_BLE_ADV_TYPE_EXTENDED) {
        rc = h2_bk_ble_set_extended_adv_data(
            s_h2_bk_ble_adv_activity,
            s_h2_bk_ble_adv_data,
            s_h2_bk_ble_adv_data_len);
    } else {
        h2_bk_ble_drain_signals();
        rc = h2_bk_ble_map_error(bk_ble_gap_set_adv_data_raw(
            s_h2_bk_ble_adv_activity,
            (uint16_t)s_h2_bk_ble_legacy_adv_data_len,
            s_h2_bk_ble_legacy_adv_data));
        if (rc == H2_PAL_OK) {
            rc = h2_bk_ble_wait(H2_BK_BLE_TIMEOUT_MS);
        }
        if (rc == H2_PAL_OK && s_h2_bk_ble_legacy_scan_rsp_data_len > 0u) {
            h2_bk_ble_drain_signals();
            rc = h2_bk_ble_map_error(bk_ble_gap_set_scan_rsp_data_raw(
                s_h2_bk_ble_adv_activity,
                (uint16_t)s_h2_bk_ble_legacy_scan_rsp_data_len,
                s_h2_bk_ble_legacy_scan_rsp_data));
            if (rc == H2_PAL_OK) {
                rc = h2_bk_ble_wait(H2_BK_BLE_TIMEOUT_MS);
            }
        }
    }
    if (rc != H2_PAL_OK) {
        (void)h2_bk_ble_cleanup_advertising();
        return rc;
    }

    bk_ble_gap_ext_adv_t ext_adv = {
        .instance = s_h2_bk_ble_adv_activity,
        .duration = (int)((params->duration_ms + 9u) / 10u),
        .max_events = params->max_adv_events,
    };
    h2_bk_ble_drain_signals();
    rc = h2_bk_ble_map_error(bk_ble_gap_adv_start(1u, &ext_adv));
    if (rc == H2_PAL_OK) {
        rc = h2_bk_ble_wait(H2_BK_BLE_TIMEOUT_MS);
    }
    if (rc != H2_PAL_OK) {
        (void)h2_bk_ble_cleanup_advertising();
    } else {
        s_h2_bk_ble_adv_started = 1;
        h2_bk_ble_post(H2_PAL_SYSTEM_EVENT_TYPE_BLE_ADVERTISING_STARTED, NULL, 0u);
    }
    return rc;
}

static h2_pal_result_t h2_bk_ble_stop_advertising(h2_pal_ble_t *ble) {
    (void)ble;
    return h2_bk_ble_cleanup_advertising();
}

static h2_pal_result_t h2_bk_ble_adv_set_create(
    h2_pal_ble_t *ble,
    const h2_pal_ble_adv_params_t *params,
    h2_pal_ble_adv_set_t **out_set) {
    (void)ble;
    if (params == NULL || out_set == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_set = NULL;
    if (!s_h2_bk_ble_started) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    if (!h2_bk_ble_uses_ethermind()) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    h2_pal_result_t lock_rc = h2_bk_ble_lock_adv();
    if (lock_rc != H2_PAL_OK) {
        return lock_rc;
    }
    for (size_t i = 0u; i < H2_BK_BLE_ADV_SET_COUNT; ++i) {
        h2_pal_ble_adv_set_t *set = &s_h2_bk_ble_adv_sets[i];
        if (set->allocated) {
            continue;
        }
        memset(set, 0, sizeof(*set));
        set->params = *params;
        set->instance = (uint8_t)(H2_BK_BLE_ADV_SET_FIRST_INSTANCE + i);
        bk_ble_gap_ext_adv_params_t adv_params;
        h2_bk_ble_fill_ext_adv_params(params, &adv_params);
        h2_bk_ble_drain_signals();
        h2_pal_result_t rc = h2_bk_ble_map_error(
            bk_ble_gap_set_adv_params(set->instance, &adv_params));
        if (rc == H2_PAL_OK) {
            rc = h2_bk_ble_wait(H2_BK_BLE_TIMEOUT_MS);
        }
        if (rc != H2_PAL_OK) {
            memset(set, 0, sizeof(*set));
            h2_bk_ble_unlock_adv();
            return rc;
        }
        set->allocated = 1;
        *out_set = set;
        h2_bk_ble_unlock_adv();
        return H2_PAL_OK;
    }
    h2_bk_ble_unlock_adv();
    return H2_PAL_ERR_NO_SPACE;
}

static h2_pal_result_t h2_bk_ble_adv_set_set_data(
    h2_pal_ble_t *ble,
    h2_pal_ble_adv_set_t *set,
    const h2_pal_ble_adv_data_t *data) {
    (void)ble;
    if (data == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    h2_pal_result_t lock_rc = h2_bk_ble_lock_adv();
    if (lock_rc != H2_PAL_OK) {
        return lock_rc;
    }
    if (!h2_bk_ble_adv_set_valid(set)) {
        h2_bk_ble_unlock_adv();
        return H2_PAL_ERR_INVALID_ARG;
    }
    uint8_t encoded[H2_PAL_BLE_EXT_ADV_DATA_MAX_LEN];
    size_t encoded_len = 0u;
    h2_pal_result_t rc;
    if (set->params.type == H2_PAL_BLE_ADV_TYPE_EXTENDED) {
        rc = h2_bk_ble_encode_adv_data(
            data, encoded, sizeof(encoded), &encoded_len, NULL, 0u, NULL);
        if (rc == H2_PAL_OK) {
            rc = h2_bk_ble_set_extended_adv_data(
                set->instance, encoded, encoded_len);
        }
    } else {
        uint8_t scan_response[H2_PAL_BLE_LEGACY_ADV_DATA_MAX_LEN];
        size_t scan_response_len = 0u;
        rc = h2_bk_ble_encode_adv_data(
            data,
            encoded,
            H2_PAL_BLE_LEGACY_ADV_DATA_MAX_LEN,
            &encoded_len,
            scan_response,
            sizeof(scan_response),
            &scan_response_len);
        if (rc == H2_PAL_OK) {
            h2_bk_ble_drain_signals();
            rc = h2_bk_ble_map_error(bk_ble_gap_set_adv_data_raw(
                set->instance, (uint16_t)encoded_len, encoded));
            if (rc == H2_PAL_OK) {
                rc = h2_bk_ble_wait(H2_BK_BLE_TIMEOUT_MS);
            }
        }
        if (rc == H2_PAL_OK && scan_response_len > 0u) {
            h2_bk_ble_drain_signals();
            rc = h2_bk_ble_map_error(bk_ble_gap_set_scan_rsp_data_raw(
                set->instance,
                (uint16_t)scan_response_len,
                scan_response));
            if (rc == H2_PAL_OK) {
                rc = h2_bk_ble_wait(H2_BK_BLE_TIMEOUT_MS);
            }
        }
    }
    if (rc == H2_PAL_OK) {
        set->data_staged = 1;
    }
    h2_bk_ble_unlock_adv();
    return rc;
}

static h2_pal_result_t h2_bk_ble_adv_set_start(
    h2_pal_ble_t *ble,
    h2_pal_ble_adv_set_t *set) {
    (void)ble;
    h2_pal_result_t lock_rc = h2_bk_ble_lock_adv();
    if (lock_rc != H2_PAL_OK) {
        return lock_rc;
    }
    if (!h2_bk_ble_adv_set_valid(set)) {
        h2_bk_ble_unlock_adv();
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (!set->data_staged || set->active) {
        h2_bk_ble_unlock_adv();
        return H2_PAL_ERR_INVALID_STATE;
    }
    bk_ble_gap_ext_adv_t ext_adv = {
        .instance = set->instance,
        .duration = (int)((set->params.duration_ms + 9u) / 10u),
        .max_events = set->params.max_adv_events,
    };
    h2_bk_ble_drain_signals();
    h2_pal_result_t rc = h2_bk_ble_map_error(
        bk_ble_gap_adv_start(1u, &ext_adv));
    if (rc == H2_PAL_OK) {
        rc = h2_bk_ble_wait(H2_BK_BLE_TIMEOUT_MS);
    }
    if (rc == H2_PAL_OK) {
        set->active = 1;
        h2_bk_ble_post_adv_set(
            H2_PAL_SYSTEM_EVENT_TYPE_BLE_ADVERTISING_STARTED,
            set,
            H2_PAL_OK);
    }
    h2_bk_ble_unlock_adv();
    return rc;
}

static h2_pal_result_t h2_bk_ble_adv_set_stop_unlocked(
    h2_pal_ble_adv_set_t *set) {
    if (!h2_bk_ble_adv_set_valid(set)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (!set->active) {
        return H2_PAL_OK;
    }
    uint8_t instance = set->instance;
    h2_bk_ble_drain_signals();
    h2_pal_result_t rc = h2_bk_ble_map_error(
        bk_ble_gap_adv_stop(1u, &instance));
    if (rc == H2_PAL_OK) {
        rc = h2_bk_ble_wait(H2_BK_BLE_TIMEOUT_MS);
    }
    if (rc == H2_PAL_OK && set->active) {
        set->active = 0;
        h2_bk_ble_post_adv_set(
            H2_PAL_SYSTEM_EVENT_TYPE_BLE_ADVERTISING_STOPPED,
            set,
            H2_PAL_OK);
    }
    return rc;
}

static h2_pal_result_t h2_bk_ble_adv_set_stop(
    h2_pal_ble_t *ble,
    h2_pal_ble_adv_set_t *set) {
    (void)ble;
    h2_pal_result_t rc = h2_bk_ble_lock_adv();
    if (rc != H2_PAL_OK) {
        return rc;
    }
    rc = h2_bk_ble_adv_set_stop_unlocked(set);
    h2_bk_ble_unlock_adv();
    return rc;
}

static h2_pal_result_t h2_bk_ble_adv_set_destroy(
    h2_pal_ble_t *ble,
    h2_pal_ble_adv_set_t *set) {
    (void)ble;
    h2_pal_result_t rc = h2_bk_ble_lock_adv();
    if (rc != H2_PAL_OK) {
        return rc;
    }
    rc = h2_bk_ble_adv_set_stop_unlocked(set);
    if (rc != H2_PAL_OK) {
        h2_bk_ble_unlock_adv();
        return rc;
    }
    if (!h2_bk_ble_adv_set_valid(set)) {
        h2_bk_ble_unlock_adv();
        return H2_PAL_ERR_INVALID_ARG;
    }
    h2_bk_ble_drain_signals();
    rc = h2_bk_ble_map_error(bk_ble_gap_adv_set_remove(set->instance));
    if (rc == H2_PAL_OK) {
        rc = h2_bk_ble_wait(H2_BK_BLE_TIMEOUT_MS);
    }
    if (rc == H2_PAL_OK) {
        memset(set, 0, sizeof(*set));
    }
    h2_bk_ble_unlock_adv();
    return rc;
}

static h2_pal_result_t h2_bk_ble_start_scan(
    h2_pal_ble_t *ble,
    const h2_pal_ble_scan_params_t *params,
    h2_pal_ble_scan_result_fn on_result,
    void *user) {
    (void)ble;
    if (!s_h2_bk_ble_started || params == NULL || on_result == NULL) {
        return params == NULL || on_result == NULL ? H2_PAL_ERR_INVALID_ARG : H2_PAL_ERR_INVALID_STATE;
    }
    if (h2_bk_ble_uses_ethermind()) {
        h2_pal_result_t rc = h2_bk_ble_ensure_gap_registered();
        if (rc != H2_PAL_OK) {
            return rc;
        }
        h2_pal_ble_scan_phy_mask_t mask = params->type == H2_PAL_BLE_SCAN_TYPE_LEGACY
                                              ? H2_PAL_BLE_SCAN_PHY_1M
                                          : params->phy_mask == 0u
                                              ? H2_PAL_BLE_SCAN_PHY_1M
                                              : params->phy_mask;
        uint16_t scan_interval = (uint16_t)((params->interval_ms * 1600u) / 1000u);
        uint16_t scan_window = (uint16_t)((params->window_ms * 1600u) / 1000u);
        bk_ble_ext_scan_cfg_t cfg = {
            .scan_type = params->mode == H2_PAL_BLE_SCAN_MODE_ACTIVE
                             ? BLE_SCAN_TYPE_ACTIVE
                             : BLE_SCAN_TYPE_PASSIVE,
            .scan_interval = scan_interval,
            .scan_window = scan_window,
        };
        bk_ble_ext_scan_params_t ext_params;
        memset(&ext_params, 0, sizeof(ext_params));
        ext_params.own_addr_type = BLE_ADDR_TYPE_PUBLIC;
        ext_params.filter_policy = BLE_SCAN_FILTER_ALLOW_ALL;
        ext_params.scan_duplicate = BLE_SCAN_DUPLICATE_DISABLE;
        if ((mask & H2_PAL_BLE_SCAN_PHY_1M) != 0u) {
            ext_params.cfg_mask |= BK_BLE_GAP_EXT_SCAN_CFG_UNCODE_MASK;
            ext_params.uncoded_cfg = cfg;
        }
        if ((mask & H2_PAL_BLE_SCAN_PHY_CODED) != 0u) {
            ext_params.cfg_mask |= H2_BK_BLE_EXT_SCAN_CODED_MASK;
            ext_params.coded_cfg = cfg;
        }
        s_h2_bk_ble_scan_cb = on_result;
        s_h2_bk_ble_scan_user = user;
        s_h2_bk_ble_scan_extended = 1;
        h2_bk_ble_drain_signals();
        rc = h2_bk_ble_map_error(bk_ble_gap_set_scan_params(&ext_params));
        if (rc == H2_PAL_OK) {
            rc = h2_bk_ble_wait(H2_BK_BLE_TIMEOUT_MS);
        }
        uint32_t duration = params->timeout_ms == 0u
                                ? 0u
                                : (params->timeout_ms + 9u) / 10u;
        if (rc == H2_PAL_OK) {
            h2_bk_ble_drain_signals();
            rc = h2_bk_ble_map_error(bk_ble_gap_start_scan(duration, 0u));
        }
        if (rc == H2_PAL_OK) {
            rc = h2_bk_ble_wait(H2_BK_BLE_TIMEOUT_MS);
        }
        if (rc != H2_PAL_OK) {
            h2_bk_ble_clear_scan_state();
        }
        return rc;
    }
    if (params->type == H2_PAL_BLE_SCAN_TYPE_EXTENDED) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    uint16_t scan_interval = h2_bk_ble_ms_to_units625(params->interval_ms);
    uint16_t scan_window = h2_bk_ble_ms_to_units625(params->window_ms);
    if (scan_window > scan_interval) {
        scan_window = scan_interval;
    }
    s_h2_bk_ble_scan_cb = on_result;
    s_h2_bk_ble_scan_user = user;
    bk_ble_set_notice_cb(h2_bk_ble_notice_cb);
    if (!s_h2_bk_ble_scan_created) {
        uint8_t activity = bk_ble_get_idle_actv_idx_handle();
        if (activity == H2_BK_BLE_INVALID_ACTIVITY) {
            return H2_PAL_ERR_UNAVAILABLE;
        }
        ble_scan_param_t scan_params;
        memset(&scan_params, 0, sizeof(scan_params));
        scan_params.own_addr_type = OWN_ADDR_TYPE_PUBLIC_ADDR;
        scan_params.scan_phy = 0x01u;
        scan_params.scan_intv = scan_interval;
        scan_params.scan_wd = scan_window;
        scan_params.scan_type = params->mode == H2_PAL_BLE_SCAN_MODE_ACTIVE ? ACTIVE_SCANNING : PASSIVE_SCANNING;
        scan_params.scan_filter = BASIC_UNFILTER_SCAN_POLICY;
        h2_pal_result_t rc = h2_bk_ble_map_error(
            bk_ble_create_scaning(activity, &scan_params, h2_bk_ble_cmd_cb));
        if (rc != H2_PAL_OK) {
            return rc;
        }
        rc = h2_bk_ble_wait(H2_BK_BLE_TIMEOUT_MS);
        if (rc != H2_PAL_OK) {
            return rc;
        }
        s_h2_bk_ble_scan_activity = activity;
        s_h2_bk_ble_scan_created = 1;
    }
    uint32_t duration = params->timeout_ms == 0u ? 0u : (params->timeout_ms / 10u);
    if (duration == 0u && params->timeout_ms != 0u) {
        duration = 1u;
    }
    h2_pal_result_t rc = h2_bk_ble_map_error(
        bk_ble_start_scaning_ex(s_h2_bk_ble_scan_activity, 0u, (uint16_t)duration, 0u, h2_bk_ble_cmd_cb));
    if (rc == H2_PAL_OK) {
        rc = h2_bk_ble_wait(H2_BK_BLE_TIMEOUT_MS);
    }
    if (rc == H2_PAL_OK) {
        s_h2_bk_ble_scan_started = 1;
        h2_bk_ble_post(H2_PAL_SYSTEM_EVENT_TYPE_BLE_SCAN_STARTED, NULL, 0u);
    }
    return rc;
}

static h2_pal_result_t h2_bk_ble_stop_scan(h2_pal_ble_t *ble) {
    (void)ble;
    return h2_bk_ble_cleanup_scan();
}

static h2_pal_result_t h2_bk_ble_register_gatt_services(
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
            H2_BK_BLE_MAX_GATT_CHARACTERISTICS_PER_SERVICE) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    bk_bt_uuid_t service_uuid;
    if (!h2_bk_ble_uuid_from_pal(&services[0].uuid, &service_uuid)) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    bk_bt_uuid_t
        char_uuids[H2_BK_BLE_MAX_GATT_CHARACTERISTICS_PER_SERVICE];
    memset(char_uuids, 0, sizeof(char_uuids));
    for (size_t i = 0u; i < services[0].characteristic_count; ++i) {
        if (!h2_bk_ble_uuid_from_pal(
                &services[0].characteristics[i].uuid, &char_uuids[i])) {
            return H2_PAL_ERR_UNSUPPORTED;
        }
    }
    h2_pal_result_t mutex_rc = h2_bk_ble_ensure_gatt_mutex();
    if (mutex_rc != H2_PAL_OK) return mutex_rc;
    if (rtos_lock_mutex(&s_h2_bk_ble_gatt_mutex) != kNoErr) {
        return H2_PAL_ERR_IO;
    }
    size_t service_index = s_h2_bk_ble_service_count;
    for (size_t i = 0u; i < s_h2_bk_ble_service_count; ++i) {
        if (h2_bk_ble_uuid_equal(
                &s_h2_bk_ble_service_uuid[i], &service_uuid)) {
            service_index = i;
            break;
        }
    }
    int add_service = service_index == s_h2_bk_ble_service_count;
    if (add_service &&
        s_h2_bk_ble_service_count >= H2_BK_BLE_MAX_GATT_SERVICES) {
        (void)rtos_unlock_mutex(&s_h2_bk_ble_gatt_mutex);
        return H2_PAL_ERR_NO_MEMORY;
    }
    if (add_service &&
        bk_ble_get_host_stack_type() != BK_BLE_HOST_STACK_TYPE_ETHERMIND &&
        s_h2_bk_ble_service_count != 0u) {
        (void)rtos_unlock_mutex(&s_h2_bk_ble_gatt_mutex);
        return H2_PAL_ERR_UNSUPPORTED;
    }
    if (!add_service &&
        s_h2_bk_ble_service_characteristic_count[service_index] !=
            services[0].characteristic_count) {
        (void)rtos_unlock_mutex(&s_h2_bk_ble_gatt_mutex);
        return H2_PAL_ERR_INVALID_STATE;
    }
    size_t first = h2_bk_ble_service_characteristic_first(service_index);
    for (size_t i = 0u; i < services[0].characteristic_count; ++i) {
        if (!add_service &&
            !h2_bk_ble_uuid_equal(
                &s_h2_bk_ble_char_uuid[first + i], &char_uuids[i])) {
            (void)rtos_unlock_mutex(&s_h2_bk_ble_gatt_mutex);
            return H2_PAL_ERR_INVALID_STATE;
        }
    }
    if (add_service) {
        s_h2_bk_ble_service_uuid[service_index] = service_uuid;
        s_h2_bk_ble_service_characteristic_count[service_index] =
            services[0].characteristic_count;
        ++s_h2_bk_ble_service_count;
    }
    for (size_t i = 0u; i < services[0].characteristic_count; ++i) {
        size_t index = first + i;
        const h2_pal_ble_gatt_characteristic_t *ch = &services[0].characteristics[i];
        s_h2_bk_ble_char_uuid[index] = char_uuids[i];
        s_h2_bk_ble_value_max_len[index] =
            ch->max_value_len > 0u &&
                    ch->max_value_len < H2_BK_BLE_MAX_VALUE_LEN
                ? (uint16_t)ch->max_value_len
                : H2_BK_BLE_MAX_VALUE_LEN;
        s_h2_bk_ble_value_len[index] =
            ch->initial_value_len < s_h2_bk_ble_value_max_len[index]
                ? (uint16_t)ch->initial_value_len
                : s_h2_bk_ble_value_max_len[index];
        memset(s_h2_bk_ble_value[index], 0,
            sizeof(s_h2_bk_ble_value[index]));
        if (s_h2_bk_ble_value_len[index] > 0u && ch->initial_value != NULL) {
            memcpy(s_h2_bk_ble_value[index], ch->initial_value,
                s_h2_bk_ble_value_len[index]);
        }
        s_h2_bk_ble_properties[index] = ch->properties;
        s_h2_bk_ble_permissions[index] = ch->permissions;
        s_h2_bk_ble_read[index] = ch->read;
        s_h2_bk_ble_write[index] = ch->write;
        s_h2_bk_ble_gatt_user[index] = ch->user;
        s_h2_bk_ble_out_value_handle[index] = ch->out_value_handle;
        s_h2_bk_ble_out_cccd_handle[index] = ch->out_cccd_handle;
        if (add_service) {
            s_h2_bk_ble_value_handle[index] =
                bk_ble_get_host_stack_type() ==
                        BK_BLE_HOST_STACK_TYPE_ETHERMIND
                    ? H2_PAL_BLE_INVALID_ATTR_HANDLE
                    : (uint16_t)(2u + 2u * i);
            s_h2_bk_ble_cccd_handle[index] =
                bk_ble_get_host_stack_type() ==
                        BK_BLE_HOST_STACK_TYPE_ETHERMIND
                    ? H2_PAL_BLE_INVALID_ATTR_HANDLE
                    : (uint16_t)(3u + 2u * i);
        }
    }
    s_h2_bk_ble_out_service_handle[service_index] =
        services[0].out_service_handle;
    if (add_service) {
        s_h2_bk_ble_service_handle[service_index] =
            bk_ble_get_host_stack_type() == BK_BLE_HOST_STACK_TYPE_ETHERMIND
                ? H2_PAL_BLE_INVALID_ATTR_HANDLE
                : 1u;
    }
    h2_bk_ble_update_out_handles();
    s_h2_bk_ble_gatt_attached = 1;
    if (s_h2_bk_ble_started && add_service) {
        h2_pal_result_t rc = h2_bk_ble_start_configured_services();
        if (rc != H2_PAL_OK) {
            h2_bk_ble_rollback_service_slot(service_index);
        }
        h2_bk_ble_clear_out_handle_ptrs();
        (void)rtos_unlock_mutex(&s_h2_bk_ble_gatt_mutex);
        return rc;
    }
    if (s_h2_bk_ble_started) {
        h2_bk_ble_clear_out_handle_ptrs();
    }
    (void)rtos_unlock_mutex(&s_h2_bk_ble_gatt_mutex);
    return H2_PAL_OK;
}

static h2_pal_result_t h2_bk_ble_unregister_gatt_services(
    h2_pal_ble_t *ble) {
    (void)ble;
    if (s_h2_bk_ble_gatt_mutex != NULL) {
        if (rtos_lock_mutex(&s_h2_bk_ble_gatt_mutex) != kNoErr) {
            return H2_PAL_ERR_IO;
        }
    }
    s_h2_bk_ble_gatt_attached = 0;
    for (size_t i = 0u; i < H2_BK_BLE_MAX_GATT_CHARACTERISTICS; ++i) {
        s_h2_bk_ble_read[i] = NULL;
        s_h2_bk_ble_write[i] = NULL;
        s_h2_bk_ble_gatt_user[i] = NULL;
    }
    h2_bk_ble_clear_out_handle_ptrs();
    if (s_h2_bk_ble_gatt_mutex != NULL) {
        (void)rtos_unlock_mutex(&s_h2_bk_ble_gatt_mutex);
    }
    return H2_PAL_OK;
}

static h2_pal_result_t h2_bk_ble_notify(
    h2_pal_ble_t *ble,
    uint16_t conn_handle,
    uint16_t attr_handle,
    const uint8_t *data,
    size_t len) {
    (void)ble;
    if (len > UINT16_MAX) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (bk_ble_get_host_stack_type() != BK_BLE_HOST_STACK_TYPE_ETHERMIND) {
        int index = h2_bk_ble_value_index(attr_handle);
        if (index < 0) return H2_PAL_ERR_INVALID_ARG;
        uint16_t legacy_index = index == 0
                                    ? H2_BK_BLE_LEGACY_IDX_CHAR0_VALUE
                                    : H2_BK_BLE_LEGACY_IDX_CHAR1_VALUE;
        h2_pal_result_t rc = h2_bk_ble_wait_legacy_notify_slot();
        if (rc != H2_PAL_OK) {
            return rc;
        }
        rc = h2_bk_ble_map_error(bk_ble_send_noti_value(
            (uint8_t)conn_handle,
            (uint32_t)len,
            (uint8_t *)data,
            H2_BK_BLE_LEGACY_PRF_ID,
            legacy_index));
        if (rc != H2_PAL_OK) {
            return rc;
        }
        s_h2_bk_ble_legacy_notify_in_flight++;
        return H2_PAL_OK;
    }
    /* EtherMind only confirms indications. A notification completes when the
     * controller accepts it, so waiting for BK_GATTS_CONF_EVT always times
     * out and tears down an otherwise healthy BLEIKCP session. */
    return h2_bk_ble_map_error(bk_ble_gatts_send_indicate(
        s_h2_bk_ble_gatts_if,
        conn_handle,
        attr_handle,
        (uint16_t)len,
        (uint8_t *)data,
        false));
}

static h2_pal_result_t h2_bk_ble_indicate(
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
    if (bk_ble_get_host_stack_type() != BK_BLE_HOST_STACK_TYPE_ETHERMIND) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    h2_pal_result_t rc = h2_bk_ble_ensure_indication_sem();
    if (rc != H2_PAL_OK) {
        return rc;
    }
    rc = h2_bk_ble_begin_gatts_tx(
        H2_BK_BLE_GATTS_TX_INDICATE_WAITING,
        conn_handle,
        attr_handle);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    h2_bk_ble_drain_indication_signals();
    rc = h2_bk_ble_map_error(
        bk_ble_gatts_send_indicate(s_h2_bk_ble_gatts_if, conn_handle, attr_handle, (uint16_t)len, (uint8_t *)data, true));
    if (rc != H2_PAL_OK) {
        h2_bk_ble_cancel_gatts_tx_submission(conn_handle, attr_handle);
        return rc;
    }
    return h2_bk_ble_wait_indication(timeout_ms);
}

static h2_pal_result_t h2_bk_ble_connect(
    h2_pal_ble_t *ble,
    const h2_pal_ble_addr_t *addr,
    const h2_pal_ble_connect_params_t *params,
    uint16_t *out_conn_handle) {
    (void)ble;
    if (!s_h2_bk_ble_started || addr == NULL || params == NULL || out_conn_handle == NULL) {
        return addr == NULL || params == NULL || out_conn_handle == NULL ? H2_PAL_ERR_INVALID_ARG : H2_PAL_ERR_INVALID_STATE;
    }
    if (bk_ble_get_host_stack_type() != BK_BLE_HOST_STACK_TYPE_ETHERMIND) {
        h2_pal_result_t rc = h2_bk_ble_ensure_sem();
        if (rc != H2_PAL_OK) {
            return rc;
        }
        bk_ble_set_notice_cb(h2_bk_ble_notice_cb);
        if (!s_h2_bk_ble_init_created) {
            uint8_t activity = bk_ble_get_idle_conn_idx_handle();
            if (activity == H2_BK_BLE_INVALID_ACTIVITY) {
                return H2_PAL_ERR_UNAVAILABLE;
            }
            ble_conn_param_t conn_params;
            memset(&conn_params, 0, sizeof(conn_params));
            conn_params.intv_min = h2_bk_ble_ms_to_units1250(params->interval_min_ms);
            conn_params.intv_max = h2_bk_ble_ms_to_units1250(params->interval_max_ms);
            conn_params.con_latency = params->latency;
            conn_params.sup_to = h2_bk_ble_ms_to_units10(params->supervision_timeout_ms);
            conn_params.init_phys = INIT_PHY_TYPE_LE_1M;
            conn_params.conn_idx = activity;
            h2_bk_ble_drain_signals();
            rc = h2_bk_ble_map_error(bk_ble_create_init(activity, &conn_params, h2_bk_ble_cmd_cb));
            if (rc != H2_PAL_OK) {
                return rc;
            }
            rc = h2_bk_ble_wait(H2_BK_BLE_TIMEOUT_MS);
            if (rc != H2_PAL_OK) {
                return rc;
            }
            s_h2_bk_ble_init_activity = activity;
            s_h2_bk_ble_init_created = 1;
        }

        bd_addr_t peer;
        memset(&peer, 0, sizeof(peer));
        memcpy(peer.addr, addr->value, sizeof(peer.addr));
        rc = h2_bk_ble_map_error(bk_ble_init_set_connect_dev_addr(
            s_h2_bk_ble_init_activity,
            &peer,
            h2_bk_ble_to_bk_addr_type(addr->type)));
        if (rc != H2_PAL_OK) {
            (void)h2_bk_ble_cleanup_initiator();
            return rc;
        }
        h2_bk_ble_drain_signals();
        s_h2_bk_ble_init_started = 1;
        s_h2_bk_ble_init_connected = 0;
        rc = h2_bk_ble_map_error(bk_ble_init_start_conn(s_h2_bk_ble_init_activity, h2_bk_ble_cmd_cb));
        if (rc != H2_PAL_OK) {
            s_h2_bk_ble_init_started = 0;
            (void)h2_bk_ble_cleanup_initiator();
            return rc;
        }
        rc = h2_bk_ble_wait(H2_BK_BLE_TIMEOUT_MS);
        if (rc != H2_PAL_OK) {
            (void)h2_bk_ble_cleanup_initiator();
            return rc;
        }
        if (!s_h2_bk_ble_init_connected) {
            rc = h2_bk_ble_wait(params->timeout_ms == 0u ? H2_BK_BLE_TIMEOUT_MS : params->timeout_ms);
            if (rc != H2_PAL_OK) {
                (void)h2_bk_ble_cleanup_initiator();
                return rc;
            }
        }
        if (!s_h2_bk_ble_init_connected || s_h2_bk_ble_conn_handle == H2_PAL_BLE_INVALID_CONN_HANDLE) {
            (void)h2_bk_ble_cleanup_initiator();
            return H2_PAL_ERR_IO;
        }
        *out_conn_handle = s_h2_bk_ble_conn_handle;
        return H2_PAL_OK;
    }
    h2_pal_result_t rc = h2_bk_ble_ensure_gattc_registered();
    if (rc != H2_PAL_OK) {
        return rc;
    }
    bk_gap_create_conn_params_t conn_params;
    memset(&conn_params, 0, sizeof(conn_params));
    conn_params.scan_interval = h2_bk_ble_ms_to_units625(100u);
    conn_params.scan_window = h2_bk_ble_ms_to_units625(50u);
    conn_params.initiator_filter_policy = 0u;
    conn_params.local_addr_type = BLE_ADDR_TYPE_PUBLIC;
    conn_params.peer_addr_type = h2_bk_ble_to_bk_addr_type(addr->type);
    memcpy(conn_params.peer_addr, addr->value, sizeof(conn_params.peer_addr));
    conn_params.conn_interval_min = h2_bk_ble_ms_to_units1250(params->interval_min_ms);
    conn_params.conn_interval_max = h2_bk_ble_ms_to_units1250(params->interval_max_ms);
    conn_params.conn_latency = params->latency;
    conn_params.supervision_timeout = h2_bk_ble_ms_to_units10(params->supervision_timeout_ms);
    h2_bk_ble_drain_signals();
    s_h2_bk_ble_connect_result = H2_PAL_ERR_UNAVAILABLE;
    s_h2_bk_ble_conn_handle = H2_PAL_BLE_INVALID_CONN_HANDLE;
    s_h2_bk_ble_hci_handle = H2_PAL_BLE_INVALID_CONN_HANDLE;
    s_h2_bk_ble_gattc_connected = 0;
    rc = h2_bk_ble_map_error(bk_ble_gap_connect(&conn_params));
    if (rc != H2_PAL_OK) {
        return rc;
    }
    const uint32_t timeout_ms = params->timeout_ms == 0u ?
        H2_BK_BLE_TIMEOUT_MS : params->timeout_ms;
    rc = h2_bk_ble_wait(timeout_ms);
    if (rc == H2_PAL_OK &&
        (s_h2_bk_ble_hci_handle == H2_PAL_BLE_INVALID_CONN_HANDLE ||
         !s_h2_bk_ble_gattc_connected)) {
        rc = h2_bk_ble_wait(timeout_ms);
    }
    if (rc == H2_PAL_OK &&
        s_h2_bk_ble_hci_handle != H2_PAL_BLE_INVALID_CONN_HANDLE &&
        s_h2_bk_ble_gattc_connected &&
        s_h2_bk_ble_conn_handle != H2_PAL_BLE_INVALID_CONN_HANDLE) {
        *out_conn_handle = s_h2_bk_ble_conn_handle;
    } else if (rc == H2_PAL_OK) {
        rc = H2_PAL_ERR_IO;
    }
    return rc;
}

static h2_pal_result_t h2_bk_ble_configure_pairing(
    h2_pal_ble_t *ble,
    const h2_pal_ble_pairing_config_t *config) {
    (void)ble;
    if (config == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    s_h2_bk_ble_pairing = *config;
    if (!config->enabled) {
        memset(&s_h2_bk_ble_pairing, 0,
               sizeof(s_h2_bk_ble_pairing));
    }
    if (bk_ble_get_host_stack_type() !=
        BK_BLE_HOST_STACK_TYPE_ETHERMIND) {
        return H2_PAL_OK;
    }
    bk_ble_auth_req_t auth =
        config->enabled ? BK_LE_AUTH_REQ_SC_MITM
                        : BK_LE_AUTH_NO_BOND;
    bk_io_cap_enum_t io =
        !config->enabled
            ? BK_IO_CAP_NONE
            : (config->io == H2_PAL_BLE_PAIRING_IO_DISPLAY_ONLY
                   ? BK_IO_CAP_DISPLAY_ONLY
                   : BK_IO_CAP_KEYBOARD_ONLY);
    uint8_t only_secure =
        config->enabled
            ? BK_BLE_ONLY_ACCEPT_SPECIFIED_AUTH_ENABLE
            : BK_BLE_ONLY_ACCEPT_SPECIFIED_AUTH_DISABLE;
    h2_pal_result_t rc = h2_bk_ble_map_error(
        bk_ble_gap_set_security_param(
            BK_BLE_SM_AUTHEN_REQ_MODE, &auth, sizeof(auth)));
    if (rc == H2_PAL_OK) {
        rc = h2_bk_ble_map_error(
            bk_ble_gap_set_security_param(
                BK_BLE_SM_IOCAP_MODE, &io, sizeof(io)));
    }
    if (rc == H2_PAL_OK) {
        rc = h2_bk_ble_map_error(
            bk_ble_gap_set_security_param(
                BK_BLE_SM_ONLY_ACCEPT_SPECIFIED_SEC_AUTH,
                &only_secure, sizeof(only_secure)));
    }
    if (rc == H2_PAL_OK) {
        rc = h2_bk_ble_map_error(
            bk_ble_gap_set_security_param(
                config->enabled ? BK_BLE_SM_SET_STATIC_PASSKEY
                                : BK_BLE_SM_CLEAR_STATIC_PASSKEY,
                config->enabled
                    ? (void *)&config->passkey
                    : NULL,
                config->enabled ? sizeof(config->passkey) : 0u));
    }
    return rc;
}

static h2_pal_result_t h2_bk_ble_pair(
    h2_pal_ble_t *ble,
    uint16_t conn_handle,
    uint32_t timeout_ms) {
    (void)ble;
    if (!s_h2_bk_ble_started || !s_h2_bk_ble_pairing.enabled ||
        conn_handle == H2_PAL_BLE_INVALID_CONN_HANDLE) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    h2_pal_result_t rc = h2_bk_ble_ensure_sem();
    if (rc != H2_PAL_OK) {
        return rc;
    }
    h2_bk_ble_drain_signals();
    s_h2_bk_ble_pair_conn_handle = conn_handle;
    if (bk_ble_get_host_stack_type() !=
        BK_BLE_HOST_STACK_TYPE_ETHERMIND) {
        bk_ble_set_notice_cb(h2_bk_ble_notice_cb);
        rc = h2_bk_ble_map_error(
            bk_ble_create_bond(
                (uint8_t)conn_handle,
                GAP_AUTH_REQ_SEC_CON_NO_BOND,
                s_h2_bk_ble_pairing.io ==
                        H2_PAL_BLE_PAIRING_IO_DISPLAY_ONLY
                    ? BK_BLE_GAP_IO_CAP_DISPLAY_ONLY
                    : BK_BLE_GAP_IO_CAP_KB_ONLY,
                GAP_SEC1_SEC_CON_PAIR_ENC,
                GAP_OOB_AUTH_DATA_NOT_PRESENT));
    } else {
        const uint8_t *peer_addr =
            conn_handle == s_h2_bk_ble_peripheral_conn_handle
                ? s_h2_bk_ble_peripheral_peer_addr
                : (conn_handle == s_h2_bk_ble_conn_handle
                       ? s_h2_bk_ble_peer_addr
                       : NULL);
        if (peer_addr == NULL) {
            s_h2_bk_ble_pair_conn_handle =
                H2_PAL_BLE_INVALID_CONN_HANDLE;
            return H2_PAL_ERR_NOT_FOUND;
        }
        rc = h2_bk_ble_map_error(
            bk_ble_set_encryption(
                (uint8_t *)peer_addr,
                BK_BLE_SEC_ENCRYPT_MITM));
    }
    if (rc != H2_PAL_OK) {
        s_h2_bk_ble_pair_conn_handle =
            H2_PAL_BLE_INVALID_CONN_HANDLE;
        return rc;
    }
    rc = h2_bk_ble_wait(timeout_ms);
    s_h2_bk_ble_pair_conn_handle = H2_PAL_BLE_INVALID_CONN_HANDLE;
    return rc;
}

static h2_pal_result_t h2_bk_ble_disconnect(h2_pal_ble_t *ble, uint16_t conn_handle) {
    (void)ble;
    if (bk_ble_get_host_stack_type() != BK_BLE_HOST_STACK_TYPE_ETHERMIND) {
        if (conn_handle == H2_PAL_BLE_INVALID_CONN_HANDLE) {
            return H2_PAL_ERR_INVALID_ARG;
        }
        h2_bk_ble_drain_signals();
        h2_pal_result_t rc = h2_bk_ble_map_error(bk_ble_disconnect((uint8_t)conn_handle));
        if (rc == H2_PAL_OK) {
            rc = h2_bk_ble_wait(H2_BK_BLE_TIMEOUT_MS);
        }
        if (conn_handle == s_h2_bk_ble_conn_handle) {
            s_h2_bk_ble_init_connected = 0;
            s_h2_bk_ble_conn_handle = H2_PAL_BLE_INVALID_CONN_HANDLE;
        } else if (conn_handle == s_h2_bk_ble_peripheral_conn_handle) {
            s_h2_bk_ble_peripheral_conn_handle =
                H2_PAL_BLE_INVALID_CONN_HANDLE;
        }
        return rc;
    }
    uint8_t *peer_addr = conn_handle == s_h2_bk_ble_peripheral_conn_handle
        ? s_h2_bk_ble_peripheral_peer_addr
        : s_h2_bk_ble_peer_addr;
    h2_pal_result_t rc = h2_bk_ble_map_error(
        bk_ble_gap_disconnect(peer_addr));
    if (rc == H2_PAL_OK) {
        rc = h2_bk_ble_wait(H2_BK_BLE_TIMEOUT_MS);
    }
    return rc;
}

static h2_pal_result_t h2_bk_ble_update_connection(
    h2_pal_ble_t *ble,
    uint16_t conn_handle,
    const h2_pal_ble_connection_params_t *params) {
    (void)ble;
    if (conn_handle == H2_PAL_BLE_INVALID_CONN_HANDLE || params == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (bk_ble_get_host_stack_type() == BK_BLE_HOST_STACK_TYPE_ETHERMIND &&
        conn_handle == s_h2_bk_ble_peripheral_conn_handle) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    h2_pal_result_t rc = h2_bk_ble_ensure_sem();
    if (rc != H2_PAL_OK) {
        return rc;
    }
    h2_bk_ble_drain_signals();
    if (bk_ble_get_host_stack_type() != BK_BLE_HOST_STACK_TYPE_ETHERMIND) {
        ble_conn_param_t conn_params;
        memset(&conn_params, 0, sizeof(conn_params));
        conn_params.conn_idx = (uint8_t)conn_handle;
        conn_params.intv_min = h2_bk_ble_ms_to_units1250(params->interval_min_ms);
        conn_params.intv_max = h2_bk_ble_ms_to_units1250(params->interval_max_ms);
        conn_params.con_latency = params->latency;
        conn_params.sup_to = h2_bk_ble_ms_to_units10(params->supervision_timeout_ms);
        conn_params.init_phys = INIT_PHY_TYPE_LE_1M;
        rc = h2_bk_ble_map_error(bk_ble_update_param((uint8_t)conn_handle, &conn_params));
    } else {
        const uint8_t *peer_addr = NULL;
        if (conn_handle == s_h2_bk_ble_peripheral_conn_handle) {
            peer_addr = s_h2_bk_ble_peripheral_peer_addr;
        } else if (conn_handle == s_h2_bk_ble_conn_handle) {
            peer_addr = s_h2_bk_ble_peer_addr;
        } else {
            return H2_PAL_ERR_INVALID_ARG;
        }
        bk_ble_conn_update_params_t conn_params;
        memset(&conn_params, 0, sizeof(conn_params));
        memcpy(conn_params.bda, peer_addr, sizeof(conn_params.bda));
        conn_params.min_int = h2_bk_ble_ms_to_units1250(params->interval_min_ms);
        conn_params.max_int = h2_bk_ble_ms_to_units1250(params->interval_max_ms);
        conn_params.latency = params->latency;
        conn_params.timeout = h2_bk_ble_ms_to_units10(params->supervision_timeout_ms);
        rc = h2_bk_ble_map_error(bk_ble_gap_update_conn_params(&conn_params));
    }
    if (rc != H2_PAL_OK) {
        return rc;
    }
    return h2_bk_ble_wait(H2_BK_BLE_TIMEOUT_MS);
}

static h2_pal_result_t h2_bk_ble_exchange_mtu(
    h2_pal_ble_t *ble,
    uint16_t conn_handle,
    uint16_t *out_mtu,
    uint32_t timeout_ms) {
    (void)ble;
    if (conn_handle == H2_PAL_BLE_INVALID_CONN_HANDLE || out_mtu == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (bk_ble_get_host_stack_type() == BK_BLE_HOST_STACK_TYPE_ETHERMIND &&
        conn_handle == s_h2_bk_ble_peripheral_conn_handle) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    h2_pal_result_t rc = h2_bk_ble_ensure_sem();
    if (rc != H2_PAL_OK) {
        return rc;
    }
    h2_bk_ble_drain_signals();
    s_h2_bk_ble_last_mtu = 0u;
    if (bk_ble_get_host_stack_type() != BK_BLE_HOST_STACK_TYPE_ETHERMIND) {
        rc = h2_bk_ble_map_error(bk_ble_gatt_mtu_change((uint8_t)conn_handle));
    } else {
        rc = h2_bk_ble_ensure_gattc_registered();
        if (rc != H2_PAL_OK) {
            return rc;
        }
        rc = h2_bk_ble_map_error(bk_ble_gattc_send_mtu_req(s_h2_bk_ble_gattc_if, conn_handle));
    }
    if (rc != H2_PAL_OK) {
        return rc;
    }
    rc = h2_bk_ble_wait(timeout_ms == 0u ? H2_BK_BLE_TIMEOUT_MS : timeout_ms);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    *out_mtu = s_h2_bk_ble_last_mtu;
    return *out_mtu > 0u ? H2_PAL_OK : H2_PAL_ERR_IO;
}

static h2_pal_result_t h2_bk_ble_set_preferred_phy(
    h2_pal_ble_t *ble,
    uint16_t conn_handle,
    h2_pal_ble_phy_t tx_phy,
    h2_pal_ble_phy_t rx_phy,
    uint32_t timeout_ms) {
    (void)ble;
    if (conn_handle == H2_PAL_BLE_INVALID_CONN_HANDLE) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (bk_ble_get_host_stack_type() != BK_BLE_HOST_STACK_TYPE_ETHERMIND) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    if (conn_handle == s_h2_bk_ble_peripheral_conn_handle) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    uint8_t *peer_addr = NULL;
    if (conn_handle == s_h2_bk_ble_peripheral_conn_handle) {
        peer_addr = s_h2_bk_ble_peripheral_peer_addr;
    } else if (conn_handle == s_h2_bk_ble_conn_handle) {
        peer_addr = s_h2_bk_ble_peer_addr;
    } else {
        return H2_PAL_ERR_INVALID_ARG;
    }
    bk_ble_gap_phy_mask_t tx_mask = h2_bk_ble_phy_mask(tx_phy);
    bk_ble_gap_phy_mask_t rx_mask = h2_bk_ble_phy_mask(rx_phy);
    if (tx_mask == 0u || rx_mask == 0u) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    h2_pal_result_t rc = h2_bk_ble_ensure_sem();
    if (rc != H2_PAL_OK) {
        return rc;
    }
    h2_bk_ble_drain_signals();
    rc = h2_bk_ble_map_error(bk_ble_gap_set_preferred_phy(
        peer_addr,
        0u,
        tx_mask,
        rx_mask,
        BK_BLE_GAP_PHY_OPTIONS_NO_PREF));
    if (rc != H2_PAL_OK) {
        return rc;
    }
    return h2_bk_ble_wait(
        timeout_ms == 0u ? H2_BK_BLE_TIMEOUT_MS : timeout_ms);
}

static h2_pal_result_t h2_bk_ble_read_phy(
    h2_pal_ble_t *ble,
    uint16_t conn_handle,
    h2_pal_ble_phy_info_t *out_phy,
    uint32_t timeout_ms) {
    (void)ble;
    if (conn_handle == H2_PAL_BLE_INVALID_CONN_HANDLE || out_phy == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (bk_ble_get_host_stack_type() != BK_BLE_HOST_STACK_TYPE_ETHERMIND) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    uint8_t *peer_addr = NULL;
    if (conn_handle == s_h2_bk_ble_peripheral_conn_handle) {
        peer_addr = s_h2_bk_ble_peripheral_peer_addr;
    } else if (conn_handle == s_h2_bk_ble_conn_handle) {
        peer_addr = s_h2_bk_ble_peer_addr;
    } else {
        return H2_PAL_ERR_INVALID_ARG;
    }
    h2_pal_result_t rc = h2_bk_ble_ensure_sem();
    if (rc != H2_PAL_OK) {
        return rc;
    }
    h2_bk_ble_drain_signals();
    memset(&s_h2_bk_ble_last_phy, 0, sizeof(s_h2_bk_ble_last_phy));
    rc = h2_bk_ble_map_error(bk_ble_gap_read_phy(peer_addr));
    if (rc != H2_PAL_OK) {
        return rc;
    }
    rc = h2_bk_ble_wait(
        timeout_ms == 0u ? H2_BK_BLE_TIMEOUT_MS : timeout_ms);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    s_h2_bk_ble_last_phy.conn_handle = conn_handle;
    *out_phy = s_h2_bk_ble_last_phy;
    return out_phy->tx_phy != H2_PAL_BLE_PHY_UNKNOWN &&
            out_phy->rx_phy != H2_PAL_BLE_PHY_UNKNOWN
        ? H2_PAL_OK
        : H2_PAL_ERR_IO;
}

static h2_pal_result_t h2_bk_ble_gatt_discover(
    h2_pal_ble_t *ble,
    uint16_t conn_handle,
    const h2_pal_ble_gatt_discovery_request_t *request,
    h2_pal_ble_gatt_discovery_entry_t *entries,
    size_t max_entries,
    size_t *out_count,
    uint32_t timeout_ms) {
    (void)ble;
    if (request == NULL || out_count == NULL || (max_entries > 0u && entries == NULL)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_count = 0u;
    s_h2_bk_ble_discovery_request = *request;
    s_h2_bk_ble_discovery_entries = entries;
    s_h2_bk_ble_discovery_max_entries = max_entries < H2_BK_BLE_MAX_DISCOVERY_ENTRIES
                                            ? max_entries
                                            : H2_BK_BLE_MAX_DISCOVERY_ENTRIES;
    s_h2_bk_ble_discovery_count = 0u;
    if (bk_ble_get_host_stack_type() != BK_BLE_HOST_STACK_TYPE_ETHERMIND) {
        uint16_t filter = h2_bk_ble_uuid16_value(&request->uuid_filter);
        h2_bk_ble_drain_signals();
        bk_ble_register_app_sdp_common_callback(h2_bk_ble_sdp_common_cb);
        h2_pal_result_t rc = H2_PAL_ERR_UNSUPPORTED;
        if (request->kind == H2_PAL_BLE_GATT_DISCOVERY_SERVICE) {
            rc = h2_bk_ble_map_error(filter == 0u
                                         ? bk_ble_discover_primary_service((uint8_t)conn_handle, request->start_handle, request->end_handle)
                                         : bk_ble_discover_primary_service_by_uuid((uint8_t)conn_handle, request->start_handle, request->end_handle, filter));
        } else if (request->kind == H2_PAL_BLE_GATT_DISCOVERY_CHARACTERISTIC) {
            rc = h2_bk_ble_map_error(filter == 0u
                                         ? bk_ble_discover_characteristic((uint8_t)conn_handle, request->start_handle, request->end_handle)
                                         : bk_ble_discover_characteristic_by_uuid((uint8_t)conn_handle, request->start_handle, request->end_handle, filter));
        } else if (request->kind == H2_PAL_BLE_GATT_DISCOVERY_DESCRIPTOR) {
            rc = h2_bk_ble_map_error(bk_ble_discover_characteristic_descriptor(
                (uint8_t)conn_handle,
                request->start_handle,
                request->end_handle));
        }
        if (rc != H2_PAL_OK) {
            return rc;
        }
        rc = h2_bk_ble_wait(timeout_ms == 0u ? H2_BK_BLE_TIMEOUT_MS : timeout_ms);
        if (rc == H2_PAL_OK) {
            *out_count = s_h2_bk_ble_discovery_count;
        }
        return rc;
    }
    h2_pal_result_t rc = h2_bk_ble_ensure_gattc_registered();
    if (rc != H2_PAL_OK) {
        return rc;
    }
    h2_bk_ble_drain_signals();
    rc = h2_bk_ble_map_error(bk_ble_gattc_discover(s_h2_bk_ble_gattc_if, conn_handle, BK_GATT_AUTH_REQ_NONE));
    if (rc != H2_PAL_OK) {
        return rc;
    }
    rc = h2_bk_ble_wait(timeout_ms == 0u ? H2_BK_BLE_TIMEOUT_MS : timeout_ms);
    if (rc == H2_PAL_OK || s_h2_bk_ble_discovery_count > 0u) {
        *out_count = s_h2_bk_ble_discovery_count;
        rc = H2_PAL_OK;
    }
    return rc;
}

static h2_pal_result_t h2_bk_ble_gatt_read(
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
    s_h2_bk_ble_read_out = out;
    s_h2_bk_ble_read_out_size = out_size;
    s_h2_bk_ble_read_out_len = 0u;
    s_h2_bk_ble_pending_handle = attr_handle;
    if (bk_ble_get_host_stack_type() != BK_BLE_HOST_STACK_TYPE_ETHERMIND) {
        h2_bk_ble_drain_signals();
        bk_ble_register_app_sdp_charac_callback(h2_bk_ble_sdp_char_cb);
        h2_pal_result_t rc = h2_bk_ble_map_error(
            bk_ble_gattc_read((uint8_t)conn_handle, attr_handle, offset));
        if (rc != H2_PAL_OK) {
            return rc;
        }
        rc = h2_bk_ble_wait(timeout_ms == 0u ? H2_BK_BLE_TIMEOUT_MS : timeout_ms);
        if (rc == H2_PAL_OK && out_len != NULL) {
            *out_len = s_h2_bk_ble_read_out_len;
        }
        return rc;
    }
    h2_pal_result_t rc = h2_bk_ble_ensure_gattc_registered();
    if (rc != H2_PAL_OK) {
        return rc;
    }
    rc = h2_bk_ble_map_error(
        bk_ble_gattc_read_char(s_h2_bk_ble_gattc_if, conn_handle, attr_handle, BK_GATT_AUTH_REQ_NONE));
    if (rc != H2_PAL_OK) {
        return rc;
    }
    rc = h2_bk_ble_wait(timeout_ms == 0u ? H2_BK_BLE_TIMEOUT_MS : timeout_ms);
    if (rc == H2_PAL_OK && out_len != NULL) {
        *out_len = s_h2_bk_ble_read_out_len;
    }
    return rc;
}

static h2_pal_result_t h2_bk_ble_gatt_write(
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
    s_h2_bk_ble_pending_handle = attr_handle;
    if (bk_ble_get_host_stack_type() != BK_BLE_HOST_STACK_TYPE_ETHERMIND) {
        h2_bk_ble_drain_signals();
        bk_ble_register_app_sdp_charac_callback(h2_bk_ble_sdp_char_cb);
        s_h2_bk_ble_waiting_write = with_response ? 1 : 0;
        int sdk_rc = bk_ble_gattc_write(
            (uint8_t)conn_handle,
            attr_handle,
            (uint8_t *)data,
            (uint16_t)len,
            with_response ? 0u : 1u);
        h2_pal_result_t rc = h2_bk_ble_map_gatt_write_error(
            sdk_rc, with_response);
        if (rc == H2_PAL_OK && with_response) {
            rc = h2_bk_ble_wait(
                timeout_ms == 0u ? H2_BK_BLE_TIMEOUT_MS : timeout_ms);
        }
        s_h2_bk_ble_waiting_write = 0;
        return rc;
    }
    h2_pal_result_t rc = h2_bk_ble_ensure_gattc_registered();
    if (rc != H2_PAL_OK) {
        return rc;
    }
    rc = h2_bk_ble_map_error(bk_ble_gattc_write_char(
        s_h2_bk_ble_gattc_if,
        conn_handle,
        attr_handle,
        (uint16_t)len,
        (uint8_t *)data,
        with_response ? BK_GATT_WRITE_TYPE_RSP : BK_GATT_WRITE_TYPE_NO_RSP,
        BK_GATT_AUTH_REQ_NONE));
    if (rc != H2_PAL_OK || !with_response) {
        return rc;
    }
    return h2_bk_ble_wait(timeout_ms == 0u ? H2_BK_BLE_TIMEOUT_MS : timeout_ms);
}

static h2_pal_result_t h2_bk_ble_gatt_subscribe(
    h2_pal_ble_t *ble,
    uint16_t conn_handle,
    const h2_pal_ble_gatt_subscribe_t *subscribe,
    uint32_t timeout_ms) {
    (void)ble;
    if (subscribe == NULL || subscribe->cccd_handle == H2_PAL_BLE_INVALID_ATTR_HANDLE) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    uint8_t value[2] = { 0u, 0u };
    if (subscribe->enable) {
        value[0] = subscribe->mode == H2_PAL_BLE_SUBSCRIBE_MODE_INDICATE ? 0x02u : 0x01u;
    }
    s_h2_bk_ble_pending_handle = subscribe->cccd_handle;
    if (bk_ble_get_host_stack_type() != BK_BLE_HOST_STACK_TYPE_ETHERMIND) {
        h2_pal_result_t rc = h2_bk_ble_ensure_sem();
        if (rc != H2_PAL_OK) {
            return rc;
        }
        h2_bk_ble_drain_signals();
        bk_ble_register_app_sdp_charac_callback(h2_bk_ble_sdp_char_cb);
        s_h2_bk_ble_waiting_write = 1;
        rc = h2_bk_ble_map_error(bk_ble_gatt_write_ccc(
            (uint8_t)conn_handle,
            subscribe->cccd_handle,
            (uint16_t)value[0]));
        if (rc == H2_PAL_OK) {
            rc = h2_bk_ble_wait(
                timeout_ms == 0u ? H2_BK_BLE_TIMEOUT_MS : timeout_ms);
        }
        s_h2_bk_ble_waiting_write = 0;
        return rc;
    }
    h2_pal_result_t rc = h2_bk_ble_ensure_gattc_registered();
    if (rc != H2_PAL_OK) {
        return rc;
    }
    rc = h2_bk_ble_map_error(bk_ble_gattc_write_char_descr(
        s_h2_bk_ble_gattc_if,
        conn_handle,
        subscribe->cccd_handle,
        sizeof(value),
        value,
        BK_GATT_WRITE_TYPE_RSP,
        BK_GATT_AUTH_REQ_NONE));
    if (rc != H2_PAL_OK) {
        return rc;
    }
    return h2_bk_ble_wait(timeout_ms == 0u ? H2_BK_BLE_TIMEOUT_MS : timeout_ms);
}
#endif

static h2_pal_result_t
h2_bk_ble_adv_set_set_scan_response_data_unsupported(
    h2_pal_ble_t *ble,
    h2_pal_ble_adv_set_t *set,
    const h2_pal_ble_adv_data_t *data) {
    (void)ble;
    (void)set;
    (void)data;
    return H2_PAL_ERR_UNSUPPORTED;
}

#if CONFIG_BLE
static const h2_pal_ble_vtable_t s_h2_bk_ble_vtable = {
    .start = (h2_pal_result_t (*)(void *))h2_bk_ble_start,
    .stop = (h2_pal_result_t (*)(void *))h2_bk_ble_stop,
    .set_adv_data = (h2_pal_result_t (*)(void *, const h2_pal_ble_adv_data_t *))h2_bk_ble_set_adv_data,
    .start_advertising = (h2_pal_result_t (*)(void *, const h2_pal_ble_adv_params_t *))h2_bk_ble_start_advertising,
    .stop_advertising = (h2_pal_result_t (*)(void *))h2_bk_ble_stop_advertising,
    .adv_set_create = (h2_pal_result_t (*)(void *, const h2_pal_ble_adv_params_t *, h2_pal_ble_adv_set_t **))h2_bk_ble_adv_set_create,
    .adv_set_set_data = (h2_pal_result_t (*)(void *, h2_pal_ble_adv_set_t *, const h2_pal_ble_adv_data_t *))h2_bk_ble_adv_set_set_data,
    .adv_set_set_scan_response_data = (h2_pal_result_t (*)(void *, h2_pal_ble_adv_set_t *, const h2_pal_ble_adv_data_t *))h2_bk_ble_adv_set_set_scan_response_data_unsupported,
    .adv_set_start = (h2_pal_result_t (*)(void *, h2_pal_ble_adv_set_t *))h2_bk_ble_adv_set_start,
    .adv_set_stop = (h2_pal_result_t (*)(void *, h2_pal_ble_adv_set_t *))h2_bk_ble_adv_set_stop,
    .adv_set_destroy = (h2_pal_result_t (*)(void *, h2_pal_ble_adv_set_t *))h2_bk_ble_adv_set_destroy,
    .start_scan = (h2_pal_result_t (*)(void *, const h2_pal_ble_scan_params_t *, h2_pal_ble_scan_result_fn, void *))h2_bk_ble_start_scan,
    .stop_scan = (h2_pal_result_t (*)(void *))h2_bk_ble_stop_scan,
    .register_gatt_services = (h2_pal_result_t (*)(void *, const h2_pal_ble_gatt_service_t *, size_t))h2_bk_ble_register_gatt_services,
    .unregister_gatt_services = (h2_pal_result_t (*)(void *))h2_bk_ble_unregister_gatt_services,
    .notify = (h2_pal_result_t (*)(void *, uint16_t, uint16_t, const uint8_t *, size_t))h2_bk_ble_notify,
    .indicate = (h2_pal_result_t (*)(void *, uint16_t, uint16_t, const uint8_t *, size_t, uint32_t))h2_bk_ble_indicate,
    .connect = (h2_pal_result_t (*)(void *, const h2_pal_ble_addr_t *, const h2_pal_ble_connect_params_t *, uint16_t *))h2_bk_ble_connect,
    .configure_pairing = (h2_pal_result_t (*)(void *, const h2_pal_ble_pairing_config_t *))h2_bk_ble_configure_pairing,
    .pair = (h2_pal_result_t (*)(void *, uint16_t, uint32_t))h2_bk_ble_pair,
    .disconnect = (h2_pal_result_t (*)(void *, uint16_t))h2_bk_ble_disconnect,
    .update_connection = (h2_pal_result_t (*)(void *, uint16_t, const h2_pal_ble_connection_params_t *))h2_bk_ble_update_connection,
    .exchange_mtu = (h2_pal_result_t (*)(void *, uint16_t, uint16_t *, uint32_t))h2_bk_ble_exchange_mtu,
    .set_preferred_phy = (h2_pal_result_t (*)(void *, uint16_t, h2_pal_ble_phy_t, h2_pal_ble_phy_t, uint32_t))h2_bk_ble_set_preferred_phy,
    .read_phy = (h2_pal_result_t (*)(void *, uint16_t, h2_pal_ble_phy_info_t *, uint32_t))h2_bk_ble_read_phy,
    .gatt_discover = (h2_pal_result_t (*)(void *, uint16_t, const h2_pal_ble_gatt_discovery_request_t *, h2_pal_ble_gatt_discovery_entry_t *, size_t, size_t *, uint32_t))h2_bk_ble_gatt_discover,
    .gatt_read = (h2_pal_result_t (*)(void *, uint16_t, uint16_t, uint16_t, uint8_t *, size_t, size_t *, uint32_t))h2_bk_ble_gatt_read,
    .gatt_write = (h2_pal_result_t (*)(void *, uint16_t, uint16_t, const uint8_t *, size_t, bool, uint32_t))h2_bk_ble_gatt_write,
    .gatt_subscribe = (h2_pal_result_t (*)(void *, uint16_t, const h2_pal_ble_gatt_subscribe_t *, uint32_t))h2_bk_ble_gatt_subscribe,
};
#else
static h2_pal_result_t h2_bk_ble_unsupported_start(h2_pal_ble_t *ble) {
    (void)ble;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t h2_bk_ble_unsupported_stop(h2_pal_ble_t *ble) {
    (void)ble;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t h2_bk_ble_unsupported_indicate(
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

static const h2_pal_ble_vtable_t s_h2_bk_ble_vtable = {
    .start = (h2_pal_result_t (*)(void *))h2_bk_ble_unsupported_start,
    .stop = (h2_pal_result_t (*)(void *))h2_bk_ble_unsupported_stop,
    .adv_set_set_scan_response_data = (h2_pal_result_t (*)(void *, h2_pal_ble_adv_set_t *, const h2_pal_ble_adv_data_t *))h2_bk_ble_adv_set_set_scan_response_data_unsupported,
    .indicate = (h2_pal_result_t (*)(void *, uint16_t, uint16_t, const uint8_t *, size_t, uint32_t))h2_bk_ble_unsupported_indicate,
};
#endif

static h2_pal_ble_t s_h2_bk_ble = {
    .user = &s_h2_bk_ble,
    .vtable = &s_h2_bk_ble_vtable,
    .allocator = NULL,
};

h2_pal_ble_t *h2_bk_platform_ble(void) {
    s_h2_bk_ble.allocator = h2_bk_platform_default_allocator();
    return &s_h2_bk_ble;
}
