#include "h2_bk3633_platform_core.h"

#if defined(H2_BK3633_BLE_SDK_FAKE)
#include "h2_bk3633_ble_sdk_fake.h"
#else
#include "gap.h"
#include "gapc_task.h"
#include "gapm_task.h"
#include "gattc_task.h"
#include "att.h"
#include "attm.h"
#include "co_error.h"
#include "ke_msg.h"
#include "ke_task.h"
#include "rwip_config.h"
#endif

#include <string.h>

#define BK3633_BLE_AD_TYPE_FLAGS 0x01u
#define BK3633_BLE_AD_TYPE_UUID16_INCOMPLETE 0x02u
#define BK3633_BLE_AD_TYPE_UUID16_COMPLETE 0x03u
#define BK3633_BLE_AD_TYPE_UUID32_INCOMPLETE 0x04u
#define BK3633_BLE_AD_TYPE_UUID32_COMPLETE 0x05u
#define BK3633_BLE_AD_TYPE_UUID128_INCOMPLETE 0x06u
#define BK3633_BLE_AD_TYPE_UUID128_COMPLETE 0x07u
#define BK3633_BLE_AD_TYPE_NAME_SHORT 0x08u
#define BK3633_BLE_AD_TYPE_NAME_COMPLETE 0x09u
#define BK3633_BLE_AD_TYPE_SERVICE_DATA16 0x16u
#define BK3633_BLE_AD_TYPE_SERVICE_DATA32 0x20u
#define BK3633_BLE_AD_TYPE_SERVICE_DATA128 0x21u
#define BK3633_BLE_AD_TYPE_MANUFACTURER 0xffu
#define BK3633_BLE_ACTIVITY_KIND_SCAN 1u
#define BK3633_BLE_ACTIVITY_KIND_ADV 2u
#define BK3633_BLE_ACTIVITY_KIND_ADV_SET_BASE 3u
#define BK3633_BLE_ADV_SET_MAX 2u
#define BK3633_BLE_ADV_OPERATION_CAPACITY 4u
#define BK3633_BLE_ADV_EVENT_CAPACITY 8u
#define BK3633_BLE_ADV_CREATE_LEGACY UINT8_MAX
#define BK3633_BLE_ADV_DATA_MAX_LEN 229u
#define BK3633_BLE_SCAN_REPORT_CAPACITY 4u

_Static_assert(
    sizeof(h2_pal_ble_connection_t) <=
        H2_BK3633_SYSTEM_EVENT_PAYLOAD_MAX,
    "BK3633 system-event payload storage is too small");
_Static_assert(
    sizeof(h2_pal_ble_disconnected_info_t) <=
        H2_BK3633_SYSTEM_EVENT_PAYLOAD_MAX,
    "BK3633 system-event payload storage is too small");
_Static_assert(
    sizeof(h2_pal_ble_connection_params_t) <=
        H2_BK3633_SYSTEM_EVENT_PAYLOAD_MAX,
    "BK3633 system-event payload storage is too small");
_Static_assert(
    sizeof(h2_pal_ble_mtu_info_t) <=
        H2_BK3633_SYSTEM_EVENT_PAYLOAD_MAX,
    "BK3633 system-event payload storage is too small");
_Static_assert(
    sizeof(h2_pal_ble_subscription_state_t) <=
        H2_BK3633_SYSTEM_EVENT_PAYLOAD_MAX,
    "BK3633 system-event payload storage is too small");
_Static_assert(
    sizeof(struct gattc_read_req_ind) == sizeof(uint16_t),
    "BK3633 RWIP read indication ABI must contain only the handle");

/*
 * BK3633 BLE host adapter boundary.
 *
 * The BK3633 SDK exposes GAPM/GAPC/GATTC/GATTS through the RWIP kernel. Those
 * messages are delivered to the image's TASK_APP dispatcher, which is owned by
 * the launcher/project and is not a component-global singleton. The command
 * path below therefore only submits messages; the project must install the
 * dispatcher and call h2_bk3633_platform_ble_dispatch() for indications.
 *
 * This file provides the complete PAL vtable. Scan and advertising commands
 * are submitted asynchronously to TASK_GAPM and their resulting indications
 * are fed back through the dispatch entry below. Unsupported operations return
 * H2_PAL_ERR_UNSUPPORTED explicitly. The adapter must not expose
 * gapm_task.h, gapc_task.h, gattc_task.h, gatts_task.h, or attm_desc through a
 * public header.
 */

struct h2_pal_ble_adv_set {
    bool used;
    bool created;
    bool create_pending;
    bool data_pending;
    bool scan_response_pending;
    bool scan_response_set;
    bool scan_response_configured;
    bool start_pending;
    bool start_requested;
    bool started;
    bool stop_pending;
    bool delete_pending;
    bool destroy_requested;
    uint8_t activity_index;
    h2_pal_ble_adv_params_t params;
    uint8_t data[BK3633_BLE_ADV_DATA_MAX_LEN];
    uint16_t data_len;
    uint16_t scan_response_data_len;
    uint16_t device_name_offset;
    uint8_t device_name_len;
};

static uint8_t *ble_adv_set_scan_response_data(
    h2_pal_ble_adv_set_t *set)
{
    return &set->data[sizeof(set->data) -
                      H2_PAL_BLE_LEGACY_ADV_DATA_MAX_LEN];
}

typedef struct h2_bk3633_ble_adv_event {
    h2_pal_system_event_type_t type;
    h2_pal_ble_adv_set_event_t payload;
} h2_bk3633_ble_adv_event_t;

typedef struct h2_bk3633_ble_scan_report {
    h2_pal_ble_addr_t addr;
    int rssi;
    bool connectable;
    bool scan_response;
    h2_pal_ble_adv_type_t adv_type;
    h2_pal_ble_phy_t primary_phy;
    h2_pal_ble_phy_t secondary_phy;
    uint8_t sid;
    h2_pal_ble_adv_data_status_t data_status;
    int8_t tx_power;
    size_t data_len;
    uint8_t data[BK3633_BLE_ADV_DATA_MAX_LEN];
} h2_bk3633_ble_scan_report_t;

typedef struct h2_bk3633_ble_state {
    h2_pal_result_t host_status;
    bool host_bootstrap_started;
    bool host_started_event_posted;
    bool host_stop_pending;
    bool host_stopped_event_pending;
    h2_pal_ble_scan_result_fn scan_callback;
    void *scan_user;
    uint8_t scan_actv_idx;
    bool scan_created;
    bool scan_create_pending;
    bool scan_started;
    bool scan_start_pending;
    bool scan_stop_pending;
    h2_bk3633_ble_scan_report_t
        pending_scan_reports[BK3633_BLE_SCAN_REPORT_CAPACITY];
    uint8_t pending_scan_head;
    uint8_t pending_scan_count;
    uint8_t adv_actv_idx;
    bool adv_created;
    bool adv_create_pending;
    bool adv_started;
    bool adv_start_pending;
    bool adv_stop_pending;
    bool adv_data_pending;
    h2_pal_ble_adv_params_t adv_params;
    h2_pal_ble_scan_params_t scan_params;
    uint8_t pending_start_kind[4];
    uint8_t pending_start_head;
    uint8_t pending_start_count;
    uint8_t pending_stop_kind[4];
    uint8_t pending_stop_head;
    uint8_t pending_stop_count;
    bool adv_start_after_data;
    uint8_t adv_data[BK3633_BLE_ADV_DATA_MAX_LEN];
    uint16_t adv_data_len;
    uint16_t device_name_offset;
    uint8_t device_name_len;
    h2_pal_ble_connection_t connection;
    uint8_t connection_index;
    bool connected;
    bool disconnect_pending;
    bool indication_pending;
    bool indication_waiting;
    bool indication_abandoned;
    uint16_t indication_seq;
    h2_pal_result_t indication_result;
    struct h2_pal_ble_adv_set adv_sets[BK3633_BLE_ADV_SET_MAX];
    uint8_t pending_adv_create[BK3633_BLE_ADV_OPERATION_CAPACITY];
    uint8_t pending_adv_create_head;
    uint8_t pending_adv_create_count;
    uint8_t pending_adv_data[BK3633_BLE_ADV_OPERATION_CAPACITY];
    uint8_t pending_adv_data_head;
    uint8_t pending_adv_data_count;
    uint8_t pending_adv_delete[BK3633_BLE_ADV_OPERATION_CAPACITY];
    uint8_t pending_adv_delete_head;
    uint8_t pending_adv_delete_count;
    h2_bk3633_ble_adv_event_t
        pending_adv_events[BK3633_BLE_ADV_EVENT_CAPACITY];
    uint8_t pending_adv_event_head;
    uint8_t pending_adv_event_count;
    h2_pal_result_t pending_adv_event_error;
} h2_bk3633_ble_state_t;

static h2_bk3633_ble_state_t s_ble_state;
static uint8_t s_ble_bootstrap_wait_key;
static uint8_t s_ble_indication_wait_key;
static const h2_pal_time_api_t *s_ble_time;
static uint32_t s_ble_bootstrap_timeout_ms;

#define BK3633_BLE_MAX_GATT_SERVICES 8u
#define BK3633_BLE_MAX_GATT_ATTRS_PER_SERVICE 16u
#define BK3633_BLE_MAX_GATT_CHARS 16u

typedef struct h2_bk3633_gatt_char {
    uint16_t value_handle;
    uint16_t cccd_handle;
    uint32_t properties;
    uint32_t permissions;
    size_t max_value_len;
    h2_pal_ble_gatt_read_fn read;
    h2_pal_ble_gatt_write_fn write;
    void *user;
} h2_bk3633_gatt_char_t;

typedef union h2_bk3633_gatt_service_db {
    struct attm_desc db16[BK3633_BLE_MAX_GATT_ATTRS_PER_SERVICE];
    struct attm_desc_128 db128[BK3633_BLE_MAX_GATT_ATTRS_PER_SERVICE];
} h2_bk3633_gatt_service_db_t;

/* attm_svc_create_db[_128]() consumes the descriptor table synchronously, and
 * ATT reads cannot arrive before registration completes. Reuse the larger of
 * those two serialized buffers instead of retaining both for image lifetime. */
typedef union h2_bk3633_ble_gatt_scratch {
    h2_bk3633_gatt_service_db_t service_db;
    uint8_t read_value[H2_PAL_BLE_ATT_MAX_VALUE_LEN];
} h2_bk3633_ble_gatt_scratch_t;

static h2_bk3633_ble_gatt_scratch_t s_gatt_scratch;
static h2_bk3633_gatt_char_t s_gatt_chars[BK3633_BLE_MAX_GATT_CHARS];
static uint16_t s_gatt_cccd_values[BK3633_BLE_MAX_GATT_CHARS];
static uint32_t s_gatt_cfg_flags[BK3633_BLE_MAX_GATT_SERVICES];
static uint8_t s_gatt_attr_counts[BK3633_BLE_MAX_GATT_SERVICES];
static uint16_t s_gatt_service_handles[BK3633_BLE_MAX_GATT_SERVICES];
static uint8_t s_gatt_service_count;
static uint8_t s_gatt_char_count;
static uint16_t s_gatt_start_handle;
static uint8_t ble_att_status(h2_pal_result_t result);

static h2_pal_result_t ble_wait_until(uintptr_t wait_key,
                                     uint64_t started_ms,
                                     uint32_t timeout_ms)
{
    if (timeout_ms == UINT32_MAX) {
        return h2_bk3633_platform_libco_wait(
            wait_key, UINT32_MAX);
    }
    uint64_t now_ms;
    h2_pal_result_t result = h2_pal_time_get_monotonic_ms(
        s_ble_time, &now_ms);
    if (result != H2_PAL_OK) {
        return result;
    }
    uint64_t elapsed_ms = h2_pal_time_elapsed_ms(started_ms, now_ms);
    if (elapsed_ms >= timeout_ms) {
        return H2_PAL_ERR_TIMEOUT;
    }
    uint64_t remaining_ms = timeout_ms - elapsed_ms;
    return h2_bk3633_platform_libco_wait(
        wait_key,
        remaining_ms > UINT32_MAX ? UINT32_MAX : (uint32_t)remaining_ms);
}

typedef enum h2_bk3633_gatt_access_kind {
    H2_BK3633_GATT_ACCESS_READ = 0,
    H2_BK3633_GATT_ACCESS_WRITE,
} h2_bk3633_gatt_access_kind_t;

typedef struct h2_bk3633_gatt_pending_access {
    h2_bk3633_gatt_access_kind_t kind;
    uint16_t dest_id;
    uint16_t src_id;
    uint16_t conn_handle;
    uint16_t attr_handle;
    uint16_t offset;
    uint16_t length;
    bool completed;
    h2_pal_result_t result;
    uint8_t value[H2_PAL_BLE_ATT_MAX_VALUE_LEN];
} h2_bk3633_gatt_pending_access_t;

static h2_bk3633_gatt_pending_access_t *s_gatt_pending_access;
static const h2_pal_mem_api_t *s_gatt_pending_mem;
static size_t s_gatt_pending_capacity;
static size_t s_gatt_pending_head;
static size_t s_gatt_pending_count;

static h2_pal_result_t ble_send_read_confirmation(
    const h2_bk3633_gatt_pending_access_t *pending,
    h2_pal_result_t result,
    const uint8_t *value,
    size_t length)
{
    struct gattc_read_cfm *cfm = KE_MSG_ALLOC_DYN(
        GATTC_READ_CFM, pending->src_id, pending->dest_id,
        gattc_read_cfm, result == H2_PAL_OK ? length : 0u);
    if (cfm == NULL) return H2_PAL_ERR_NO_MEMORY;
    cfm->handle = pending->attr_handle;
    cfm->status = ble_att_status(result);
    cfm->length = result == H2_PAL_OK ? (uint16_t)length : 0u;
    if (cfm->length != 0u) memcpy(cfm->value, value, cfm->length);
    ke_msg_send(cfm);
    return H2_PAL_OK;
}

static h2_pal_result_t ble_send_write_confirmation(
    const h2_bk3633_gatt_pending_access_t *pending,
    h2_pal_result_t result)
{
    struct gattc_write_cfm *cfm = KE_MSG_ALLOC(
        GATTC_WRITE_CFM, pending->src_id, pending->dest_id,
        gattc_write_cfm);
    if (cfm == NULL) return H2_PAL_ERR_NO_MEMORY;
    cfm->handle = pending->attr_handle;
    cfm->status = ble_att_status(result);
    ke_msg_send(cfm);
    return H2_PAL_OK;
}

static h2_pal_result_t ble_confirm_pending_access(
    h2_bk3633_gatt_pending_access_t *pending)
{
    if (pending->kind == H2_BK3633_GATT_ACCESS_READ) {
        return ble_send_read_confirmation(
            pending, pending->result, pending->value, pending->length);
    }
    return ble_send_write_confirmation(pending, pending->result);
}

static void ble_pop_pending_access(void)
{
    h2_bk3633_gatt_pending_access_t *pending =
        &s_gatt_pending_access[s_gatt_pending_head];
    memset(pending, 0, sizeof(*pending));
    s_gatt_pending_head =
        (s_gatt_pending_head + 1u) % s_gatt_pending_capacity;
    --s_gatt_pending_count;
}

static h2_pal_result_t ble_reject_pending_accesses(h2_pal_result_t result)
{
    while (s_gatt_pending_count != 0u) {
        h2_bk3633_gatt_pending_access_t *pending =
            &s_gatt_pending_access[s_gatt_pending_head];
        pending->completed = true;
        pending->result = result;
        pending->length = 0u;
        h2_pal_result_t confirmation = ble_confirm_pending_access(pending);
        if (confirmation != H2_PAL_OK) return confirmation;
        ble_pop_pending_access();
    }
    return H2_PAL_OK;
}

static bool ble_enqueue_access(
    h2_bk3633_gatt_access_kind_t kind,
    uint16_t dest_id,
    uint16_t src_id,
    uint16_t attr_handle,
    uint16_t offset,
    const uint8_t *value,
    uint16_t length)
{
    if (s_gatt_pending_access == NULL ||
        s_gatt_pending_count == s_gatt_pending_capacity ||
        length > H2_PAL_BLE_ATT_MAX_VALUE_LEN)
        return false;
    size_t tail = (s_gatt_pending_head + s_gatt_pending_count) %
                  s_gatt_pending_capacity;
    h2_bk3633_gatt_pending_access_t *pending =
        &s_gatt_pending_access[tail];
    *pending = (h2_bk3633_gatt_pending_access_t){
        .kind = kind,
        .dest_id = dest_id,
        .src_id = src_id,
        .conn_handle = s_ble_state.connection.conn_handle,
        .attr_handle = attr_handle,
        .offset = offset,
        .length = length,
    };
    if (length != 0u) memcpy(pending->value, value, length);
    ++s_gatt_pending_count;
    return true;
}

static void ble_reset_gatt_registration(void)
{
    memset(s_gatt_chars, 0, sizeof(s_gatt_chars));
    memset(s_gatt_cccd_values, 0, sizeof(s_gatt_cccd_values));
    memset(&s_gatt_scratch, 0, sizeof(s_gatt_scratch));
    memset(s_gatt_cfg_flags, 0, sizeof(s_gatt_cfg_flags));
    memset(s_gatt_attr_counts, 0, sizeof(s_gatt_attr_counts));
    memset(s_gatt_service_handles, 0, sizeof(s_gatt_service_handles));
    s_gatt_service_count = 0u;
    s_gatt_char_count = 0u;
    s_gatt_start_handle = 0u;
}

#if defined(H2_BK3633_BLE_SDK_FAKE)
void h2_bk3633_platform_ble_test_reset(void)
{
    memset(&s_ble_state, 0, sizeof(s_ble_state));
    h2_pal_mem_free(s_gatt_pending_mem, s_gatt_pending_access);
    s_gatt_pending_access = NULL;
    s_gatt_pending_mem = NULL;
    s_gatt_pending_capacity = 0u;
    s_gatt_pending_head = 0u;
    s_gatt_pending_count = 0u;
    s_ble_time = NULL;
    s_ble_bootstrap_timeout_ms = 0u;
    ble_reset_gatt_registration();
}

void h2_bk3633_platform_ble_test_set_indication_sequence(uint16_t sequence)
{
    s_ble_state.indication_seq = sequence;
}
#endif

static h2_pal_result_t ble_post_event(
    h2_pal_system_event_type_t type,
    const void *payload,
    size_t payload_size);
static h2_pal_result_t ble_dispatch_pending_scan_reports(void);
static h2_pal_result_t ble_unregister_gatt_services(void *user);

static void ble_clear_pending_scan_reports(void)
{
    memset(s_ble_state.pending_scan_reports, 0,
           sizeof(s_ble_state.pending_scan_reports));
    s_ble_state.pending_scan_head = 0u;
    s_ble_state.pending_scan_count = 0u;
}

static uint16_t ble_uuid16(const h2_pal_ble_uuid_t *uuid)
{
    return (uint16_t)uuid->data[0] | ((uint16_t)uuid->data[1] << 8);
}

static uint16_t ble_gatt_db128_set_uuid(
    uint8_t out[16], const uint8_t *uuid, size_t uuid_len)
{
    /* The 128-bit table still encodes standard declarations as 16-bit UUIDs. */
    memset(out, 0, 16u);
    memcpy(out, uuid, uuid_len);
    return uuid_len == 16u ? PERM(UUID_LEN, UUID_128) : 0u;
}

static h2_bk3633_gatt_char_t *ble_find_gatt_char(uint16_t handle)
{
    for (uint8_t i = 0u; i < s_gatt_char_count; ++i) {
        if (s_gatt_chars[i].value_handle == handle ||
            s_gatt_chars[i].cccd_handle == handle) {
            return &s_gatt_chars[i];
        }
    }
    return NULL;
}

static bool ble_gatt_subscription_enabled(
    const h2_bk3633_gatt_char_t *characteristic,
    uint16_t required_value)
{
    for (uint8_t i = 0u; i < s_gatt_char_count; ++i) {
        if (&s_gatt_chars[i] == characteristic) {
            return (s_gatt_cccd_values[i] & required_value) != 0u;
        }
    }
    return false;
}

static uint8_t ble_att_status(h2_pal_result_t result)
{
    switch (result) {
    case H2_PAL_OK:
        return ATT_ERR_NO_ERROR;
    case H2_PAL_ERR_INVALID_ARG:
    case H2_PAL_ERR_NO_SPACE:
        return ATT_ERR_INVALID_ATTRIBUTE_VAL_LEN;
    case H2_PAL_ERR_NO_MEMORY:
    case H2_PAL_ERR_FULL:
        return ATT_ERR_INSUFF_RESOURCE;
    case H2_PAL_ERR_IO:
    case H2_PAL_ERR_WRITE:
        return ATT_ERR_UNLIKELY_ERR;
    default:
        return ATT_ERR_REQUEST_NOT_SUPPORTED;
    }
}

h2_pal_result_t h2_bk3633_platform_ble_dispatch_pending(void)
{
    if (s_ble_state.host_stopped_event_pending) {
        h2_pal_result_t post_result = ble_post_event(
            H2_PAL_SYSTEM_EVENT_TYPE_BLE_HOST_STOPPED, NULL, 0u);
        if (post_result != H2_PAL_OK) return post_result;
        s_ble_state.host_stopped_event_pending = false;
    }
    while (s_ble_state.pending_adv_event_count != 0u) {
        h2_bk3633_ble_adv_event_t *event =
            &s_ble_state.pending_adv_events[
                s_ble_state.pending_adv_event_head];
        h2_pal_result_t post_result = ble_post_event(
            event->type, &event->payload, sizeof(event->payload));
        if (post_result != H2_PAL_OK) {
            return post_result;
        }
        memset(event, 0, sizeof(*event));
        s_ble_state.pending_adv_event_head = (uint8_t)(
            (s_ble_state.pending_adv_event_head + 1u) %
            BK3633_BLE_ADV_EVENT_CAPACITY);
        --s_ble_state.pending_adv_event_count;
    }
    if (s_ble_state.pending_adv_event_error != H2_PAL_OK) {
        h2_pal_result_t result = s_ble_state.pending_adv_event_error;
        s_ble_state.pending_adv_event_error = H2_PAL_OK;
        return result;
    }
    h2_pal_result_t scan_result = ble_dispatch_pending_scan_reports();
    if (scan_result != H2_PAL_OK) {
        return scan_result;
    }
    while (s_gatt_pending_count != 0u) {
        h2_bk3633_gatt_pending_access_t *pending =
            &s_gatt_pending_access[s_gatt_pending_head];
        if (!pending->completed) {
            h2_bk3633_gatt_char_t *characteristic =
                ble_find_gatt_char(pending->attr_handle);
            pending->result = H2_PAL_ERR_CLOSED;
            if (s_ble_state.connected &&
                pending->conn_handle == s_ble_state.connection.conn_handle &&
                characteristic != NULL) {
                h2_pal_ble_gatt_access_t access = {
                    .conn_handle = pending->conn_handle,
                    .attr_handle = pending->attr_handle,
                    .offset = pending->offset,
                };
                if (pending->kind == H2_BK3633_GATT_ACCESS_READ &&
                    characteristic->read != NULL) {
                    size_t length = 0u;
                    pending->result = characteristic->read(
                        characteristic->user, &access,
                        s_gatt_scratch.read_value,
                        characteristic->max_value_len, &length);
                    if (pending->result == H2_PAL_OK &&
                        length > characteristic->max_value_len) {
                        pending->result = H2_PAL_ERR_NO_SPACE;
                        length = 0u;
                    }
                    pending->length = (uint16_t)length;
                    if (length != 0u) {
                        memcpy(pending->value,
                               s_gatt_scratch.read_value, length);
                    }
                } else if (pending->kind == H2_BK3633_GATT_ACCESS_WRITE &&
                           characteristic->write != NULL) {
                    pending->result = characteristic->write(
                        characteristic->user, &access, pending->value,
                        pending->length);
                } else {
                    pending->result = H2_PAL_ERR_UNSUPPORTED;
                    if (pending->kind == H2_BK3633_GATT_ACCESS_READ) {
                        pending->length = 0u;
                    }
                }
            } else if (pending->kind == H2_BK3633_GATT_ACCESS_READ) {
                pending->length = 0u;
            }
            pending->completed = true;
        }
        h2_pal_result_t confirmation = ble_confirm_pending_access(pending);
        if (confirmation != H2_PAL_OK) return confirmation;
        ble_pop_pending_access();
    }
    return H2_PAL_OK;
}

static int ble_gatt_read_req_handler(
    ke_msg_id_t const msgid,
    struct gattc_read_req_ind const *param,
    ke_task_id_t const dest_id,
    ke_task_id_t const src_id)
{
    (void)msgid;
    if (param == NULL) return KE_MSG_CONSUMED;
    h2_bk3633_gatt_char_t *characteristic = ble_find_gatt_char(param->handle);
    h2_bk3633_gatt_pending_access_t pending = {
        .kind = H2_BK3633_GATT_ACCESS_READ,
        .dest_id = dest_id,
        .src_id = src_id,
        .conn_handle = s_ble_state.connection.conn_handle,
        .attr_handle = param->handle,
    };
    if (characteristic != NULL && characteristic->cccd_handle == param->handle) {
        for (uint8_t i = 0u; i < s_gatt_char_count; ++i) {
            if (&s_gatt_chars[i] == characteristic) {
                s_gatt_scratch.read_value[0] =
                    (uint8_t)(s_gatt_cccd_values[i] & 0xffu);
                s_gatt_scratch.read_value[1] =
                    (uint8_t)(s_gatt_cccd_values[i] >> 8);
                ble_send_read_confirmation(
                    &pending, H2_PAL_OK, s_gatt_scratch.read_value,
                    sizeof(uint16_t));
                return KE_MSG_CONSUMED;
            }
        }
    } else if (characteristic != NULL && characteristic->read != NULL) {
        if (ble_enqueue_access(
                H2_BK3633_GATT_ACCESS_READ, dest_id, src_id,
                param->handle, 0u, NULL, 0u))
            return KE_MSG_CONSUMED;
    }
    ble_send_read_confirmation(
        &pending,
        characteristic == NULL ? H2_PAL_ERR_NOT_FOUND : H2_PAL_ERR_FULL,
        NULL, 0u);
    return KE_MSG_CONSUMED;
}

static int ble_gatt_write_req_handler(
    ke_msg_id_t const msgid,
    struct gattc_write_req_ind const *param,
    ke_task_id_t const dest_id,
    ke_task_id_t const src_id)
{
    (void)msgid;
    if (param == NULL) return KE_MSG_CONSUMED;
    h2_bk3633_gatt_char_t *characteristic = ble_find_gatt_char(param->handle);
    h2_pal_result_t result = H2_PAL_ERR_UNSUPPORTED;
    if (characteristic != NULL &&
        characteristic->cccd_handle == param->handle) {
        result = H2_PAL_ERR_INVALID_ARG;
        if (param->offset == 0u && param->length == sizeof(uint16_t)) {
            uint16_t cccd =
                (uint16_t)param->value[0] |
                ((uint16_t)param->value[1] << 8);
            for (uint8_t i = 0u; i < s_gatt_char_count; ++i) {
                if (&s_gatt_chars[i] == characteristic) {
                    bool notify_allowed =
                        (characteristic->properties &
                         H2_PAL_BLE_GATT_PROPERTY_NOTIFY) != 0u;
                    bool indicate_allowed =
                        (characteristic->properties &
                         H2_PAL_BLE_GATT_PROPERTY_INDICATE) != 0u;
                    if ((cccd & (uint16_t)~0x0003u) != 0u ||
                        ((cccd & 0x0001u) != 0u && !notify_allowed) ||
                        ((cccd & 0x0002u) != 0u && !indicate_allowed)) {
                        break;
                    }
                    s_gatt_cccd_values[i] = cccd;
                    h2_pal_ble_subscription_state_t state = {
                        .conn_handle =
                            s_ble_state.connection.conn_handle,
                        .value_handle = characteristic->value_handle,
                        .mode = (cccd & 2u) != 0u
                            ? H2_PAL_BLE_SUBSCRIBE_MODE_INDICATE
                            : H2_PAL_BLE_SUBSCRIBE_MODE_NOTIFY,
                        .enabled = cccd != 0u,
                    };
                    ble_post_event(
                        H2_PAL_SYSTEM_EVENT_TYPE_BLE_SUBSCRIPTION_CHANGED,
                        &state,
                        sizeof(state));
                    result = H2_PAL_OK;
                    break;
                }
            }
        }
    } else if (characteristic != NULL && characteristic->write != NULL) {
        if (param->length > characteristic->max_value_len ||
            param->offset >
                characteristic->max_value_len - param->length) {
            result = H2_PAL_ERR_INVALID_ARG;
        } else if (ble_enqueue_access(
                       H2_BK3633_GATT_ACCESS_WRITE, dest_id, src_id,
                       param->handle, param->offset, param->value,
                       param->length)) {
            return KE_MSG_CONSUMED;
        } else {
            result = H2_PAL_ERR_FULL;
        }
    }
    struct gattc_write_cfm *cfm = KE_MSG_ALLOC(
        GATTC_WRITE_CFM, src_id, dest_id, gattc_write_cfm);
    if (cfm != NULL) {
        cfm->handle = param->handle;
        cfm->status = ble_att_status(result);
        ke_msg_send(cfm);
    }
    return KE_MSG_CONSUMED;
}

static int ble_gatt_att_info_req_handler(
    ke_msg_id_t const msgid,
    struct gattc_att_info_req_ind const *param,
    ke_task_id_t const dest_id,
    ke_task_id_t const src_id)
{
    (void)msgid;
    if (param == NULL) return KE_MSG_CONSUMED;

    h2_bk3633_gatt_char_t *characteristic =
        ble_find_gatt_char(param->handle);
    struct gattc_att_info_cfm *cfm = KE_MSG_ALLOC(
        GATTC_ATT_INFO_CFM, src_id, dest_id, gattc_att_info_cfm);
    if (cfm == NULL) return KE_MSG_CONSUMED;

    cfm->handle = param->handle;
    cfm->length = 0u;
    cfm->status = ATT_ERR_WRITE_NOT_PERMITTED;
    if (characteristic != NULL &&
        characteristic->cccd_handle == param->handle) {
        cfm->length = sizeof(uint16_t);
        cfm->status = ATT_ERR_NO_ERROR;
    } else if (characteristic != NULL &&
               characteristic->write != NULL) {
        cfm->length = (uint16_t)characteristic->max_value_len;
        cfm->status = ATT_ERR_NO_ERROR;
    }
    ke_msg_send(cfm);
    return KE_MSG_CONSUMED;
}

static h2_pal_result_t ble_post_event(
    h2_pal_system_event_type_t type,
    const void *payload,
    size_t payload_size)
{
    const h2_pal_system_event_t event = {
        .type = type,
        .payload = payload,
        .payload_size = payload_size,
    };
    return h2_pal_system_event_post(
        h2_bk3633_platform_system_event_api(), &event, 0u);
}

static void ble_post_adv_set_event(
    h2_pal_system_event_type_t type,
    h2_pal_ble_adv_set_t *set,
    h2_pal_result_t result)
{
    const h2_pal_ble_adv_set_event_t event = {
        .set = set,
        .status = result,
    };
    if (s_ble_state.pending_adv_event_count == 0u &&
        ble_post_event(type, &event, sizeof(event)) == H2_PAL_OK) {
        return;
    }
    if (s_ble_state.pending_adv_event_count ==
        BK3633_BLE_ADV_EVENT_CAPACITY) {
        s_ble_state.pending_adv_event_error = H2_PAL_ERR_FULL;
        return;
    }
    uint8_t tail = (uint8_t)(
        (s_ble_state.pending_adv_event_head +
         s_ble_state.pending_adv_event_count) %
        BK3633_BLE_ADV_EVENT_CAPACITY);
    s_ble_state.pending_adv_events[tail] = (h2_bk3633_ble_adv_event_t){
        .type = type,
        .payload = event,
    };
    ++s_ble_state.pending_adv_event_count;
}

static void ble_complete_indication(h2_pal_result_t result)
{
    if (!s_ble_state.indication_pending) return;
    s_ble_state.indication_pending = false;
    s_ble_state.indication_result = result;
    if (s_ble_state.indication_abandoned) {
        s_ble_state.indication_abandoned = false;
        return;
    }
    if (s_ble_state.indication_waiting) {
        (void)h2_bk3633_platform_libco_record_completion(
            (uintptr_t)&s_ble_indication_wait_key);
    }
}

static uint16_t ble_conn_interval_to_ms(uint16_t units)
{
    return (uint16_t)(((uint32_t)units * 125u + 50u) / 100u);
}

static uint32_t ble_ms_to_625us(uint32_t ms)
{
    uint64_t units = ((uint64_t)ms * 1000u + 624u) / 625u;
    if (units < 32u) units = 32u;
    return units > 0x00ffffffu
        ? 0x00ffffffu
        : (uint32_t)units;
}

static uint16_t ble_ms_to_scan_units(uint32_t ms)
{
    uint64_t units = ((uint64_t)ms * 1000u + 624u) / 625u;
    if (units == 0u) units = 1u;
    return units > 0xffffu ? 0xffffu : (uint16_t)units;
}

static uint16_t ble_ms_to_10ms(uint32_t ms)
{
    uint64_t units = ((uint64_t)ms + 9u) / 10u;
    return units > 0xffffu ? 0xffffu : (uint16_t)units;
}

static bool ble_start_queue_push(uint8_t kind)
{
    if (s_ble_state.pending_start_count >=
        sizeof(s_ble_state.pending_start_kind))
        return false;
    uint8_t tail = (uint8_t)(
        (s_ble_state.pending_start_head + s_ble_state.pending_start_count) %
        sizeof(s_ble_state.pending_start_kind));
    s_ble_state.pending_start_kind[tail] = kind;
    ++s_ble_state.pending_start_count;
    return true;
}

static uint8_t ble_start_queue_pop(void)
{
    if (s_ble_state.pending_start_count == 0u) return 0u;
    uint8_t kind =
        s_ble_state.pending_start_kind[s_ble_state.pending_start_head];
    s_ble_state.pending_start_head = (uint8_t)(
        (s_ble_state.pending_start_head + 1u) %
        sizeof(s_ble_state.pending_start_kind));
    --s_ble_state.pending_start_count;
    return kind;
}

static bool ble_stop_queue_push(uint8_t kind)
{
    if (s_ble_state.pending_stop_count >=
        sizeof(s_ble_state.pending_stop_kind))
        return false;
    uint8_t tail = (uint8_t)(
        (s_ble_state.pending_stop_head + s_ble_state.pending_stop_count) %
        sizeof(s_ble_state.pending_stop_kind));
    s_ble_state.pending_stop_kind[tail] = kind;
    ++s_ble_state.pending_stop_count;
    return true;
}

static uint8_t ble_stop_queue_pop(void)
{
    if (s_ble_state.pending_stop_count == 0u) return 0u;
    uint8_t kind =
        s_ble_state.pending_stop_kind[s_ble_state.pending_stop_head];
    s_ble_state.pending_stop_head = (uint8_t)(
        (s_ble_state.pending_stop_head + 1u) %
        sizeof(s_ble_state.pending_stop_kind));
    --s_ble_state.pending_stop_count;
    return kind;
}

static int ble_adv_set_index(const h2_pal_ble_adv_set_t *set)
{
    uintptr_t address = (uintptr_t)set;
    uintptr_t begin = (uintptr_t)&s_ble_state.adv_sets[0];
    uintptr_t end = (uintptr_t)&s_ble_state.adv_sets[BK3633_BLE_ADV_SET_MAX];
    if (address < begin || address >= end ||
        (address - begin) % sizeof(s_ble_state.adv_sets[0]) != 0u)
        return -1;
    size_t index = (address - begin) / sizeof(s_ble_state.adv_sets[0]);
    return s_ble_state.adv_sets[index].used ? (int)index : -1;
}

static h2_pal_ble_adv_set_t *ble_adv_set_from_kind(uint8_t kind)
{
    if (kind < BK3633_BLE_ACTIVITY_KIND_ADV_SET_BASE ||
        kind >= BK3633_BLE_ACTIVITY_KIND_ADV_SET_BASE +
                    BK3633_BLE_ADV_SET_MAX)
        return NULL;
    h2_pal_ble_adv_set_t *set =
        &s_ble_state.adv_sets[
            kind - BK3633_BLE_ACTIVITY_KIND_ADV_SET_BASE];
    return set->used ? set : NULL;
}

static h2_pal_ble_adv_set_t *ble_find_adv_set_by_activity(uint8_t index)
{
    for (size_t i = 0u; i < BK3633_BLE_ADV_SET_MAX; ++i) {
        h2_pal_ble_adv_set_t *set = &s_ble_state.adv_sets[i];
        if (set->used && set->created && set->activity_index == index)
            return set;
    }
    return NULL;
}

static bool ble_adv_queue_push(
    uint8_t *queue, uint8_t head, uint8_t *count, uint8_t value)
{
    if (*count >= BK3633_BLE_ADV_OPERATION_CAPACITY) return false;
    uint8_t tail = (uint8_t)(
        (head + *count) % BK3633_BLE_ADV_OPERATION_CAPACITY);
    queue[tail] = value;
    ++*count;
    return true;
}

static uint8_t ble_adv_queue_pop(
    uint8_t *queue, uint8_t *head, uint8_t *count)
{
    if (*count == 0u) return UINT8_MAX;
    uint8_t value = queue[*head];
    *head = (uint8_t)(
        (*head + 1u) % BK3633_BLE_ADV_OPERATION_CAPACITY);
    --*count;
    return value;
}

#if !defined(CFG_PERIPHERAL)
static h2_pal_result_t ble_send_create_scan(void)
{
    struct gapm_activity_create_cmd *cmd = KE_MSG_ALLOC(
        GAPM_ACTIVITY_CREATE_CMD, TASK_GAPM, TASK_APP, gapm_activity_create_cmd);
    if (cmd == NULL) return H2_PAL_ERR_NO_MEMORY;
    cmd->operation = GAPM_CREATE_SCAN_ACTIVITY;
    cmd->own_addr_type = GAPM_STATIC_ADDR;
    s_ble_state.scan_create_pending = true;
    ke_msg_send(cmd);
    return H2_PAL_OK;
}
#endif

static h2_pal_result_t ble_send_start_scan(const h2_pal_ble_scan_params_t *params)
{
    if (s_ble_state.pending_start_count >=
        sizeof(s_ble_state.pending_start_kind))
        return H2_PAL_ERR_INVALID_STATE;
    struct gapm_activity_start_cmd *cmd = KE_MSG_ALLOC(
        GAPM_ACTIVITY_START_CMD, TASK_GAPM, TASK_APP, gapm_activity_start_cmd);
    if (cmd == NULL) return H2_PAL_ERR_NO_MEMORY;
    memset(cmd, 0, sizeof(*cmd));
    cmd->operation = GAPM_START_ACTIVITY;
    cmd->actv_idx = s_ble_state.scan_actv_idx;
    cmd->u_param.scan_param.type = GAPM_SCAN_TYPE_OBSERVER;
    h2_pal_ble_scan_phy_mask_t phy_mask = params->type == H2_PAL_BLE_SCAN_TYPE_LEGACY
        ? H2_PAL_BLE_SCAN_PHY_1M
        : (params->phy_mask == 0u ? H2_PAL_BLE_SCAN_PHY_1M : params->phy_mask);
    if ((phy_mask & H2_PAL_BLE_SCAN_PHY_1M) != 0u) {
        cmd->u_param.scan_param.prop |= GAPM_SCAN_PROP_PHY_1M_BIT;
        if (params->mode == H2_PAL_BLE_SCAN_MODE_ACTIVE)
            cmd->u_param.scan_param.prop |= GAPM_SCAN_PROP_ACTIVE_1M_BIT;
        cmd->u_param.scan_param.scan_param_1m.scan_intv =
            ble_ms_to_scan_units(params->interval_ms);
        cmd->u_param.scan_param.scan_param_1m.scan_wd =
            ble_ms_to_scan_units(params->window_ms);
        if (cmd->u_param.scan_param.scan_param_1m.scan_wd >
            cmd->u_param.scan_param.scan_param_1m.scan_intv)
            cmd->u_param.scan_param.scan_param_1m.scan_wd =
                cmd->u_param.scan_param.scan_param_1m.scan_intv;
    }
    if ((phy_mask & H2_PAL_BLE_SCAN_PHY_CODED) != 0u) {
        cmd->u_param.scan_param.prop |= GAPM_SCAN_PROP_PHY_CODED_BIT;
        if (params->mode == H2_PAL_BLE_SCAN_MODE_ACTIVE)
            cmd->u_param.scan_param.prop |= GAPM_SCAN_PROP_ACTIVE_CODED_BIT;
        cmd->u_param.scan_param.scan_param_coded.scan_intv =
            ble_ms_to_scan_units(params->interval_ms);
        cmd->u_param.scan_param.scan_param_coded.scan_wd =
            ble_ms_to_scan_units(params->window_ms);
        if (cmd->u_param.scan_param.scan_param_coded.scan_wd >
            cmd->u_param.scan_param.scan_param_coded.scan_intv)
            cmd->u_param.scan_param.scan_param_coded.scan_wd =
                cmd->u_param.scan_param.scan_param_coded.scan_intv;
    }
    cmd->u_param.scan_param.duration = params->timeout_ms == 0u
        ? 0u
        : ble_ms_to_10ms(params->timeout_ms);
    if (!ble_start_queue_push(BK3633_BLE_ACTIVITY_KIND_SCAN))
        return H2_PAL_ERR_INVALID_STATE;
    s_ble_state.scan_start_pending = true;
    ke_msg_send(cmd);
    return H2_PAL_OK;
}

static h2_pal_result_t ble_send_stop(uint8_t actv_idx, uint8_t kind)
{
    if (s_ble_state.pending_stop_count >=
        sizeof(s_ble_state.pending_stop_kind))
        return H2_PAL_ERR_INVALID_STATE;
    struct gapm_activity_stop_cmd *cmd = KE_MSG_ALLOC(
        GAPM_ACTIVITY_STOP_CMD, TASK_GAPM, TASK_APP, gapm_activity_stop_cmd);
    if (cmd == NULL) return H2_PAL_ERR_NO_MEMORY;
    cmd->operation = GAPM_STOP_ACTIVITY;
    cmd->actv_idx = actv_idx;
    if (!ble_stop_queue_push(kind)) return H2_PAL_ERR_INVALID_STATE;
    ke_msg_send(cmd);
    return H2_PAL_OK;
}

static size_t ble_append_ad(uint8_t *out, size_t offset, size_t capacity,
                            uint8_t type, const uint8_t *data, size_t len)
{
    if (out == NULL || len > 254u || (len != 0u && data == NULL) ||
        offset > capacity || len + 2u > capacity - offset)
        return 0u;
    out[offset] = (uint8_t)(len + 1u);
    out[offset + 1u] = type;
    if (len != 0u) memcpy(&out[offset + 2u], data, len);
    return offset + len + 2u;
}

static size_t ble_append_ad_parts(uint8_t *out,
                                  size_t offset,
                                  size_t capacity,
                                  uint8_t type,
                                  const uint8_t *first,
                                  size_t first_len,
                                  const uint8_t *second,
                                  size_t second_len)
{
    if (out == NULL ||
        (first_len != 0u && first == NULL) ||
        (second_len != 0u && second == NULL) ||
        first_len > 254u || second_len > 254u - first_len ||
        offset > capacity ||
        first_len + second_len + 2u > capacity - offset)
        return 0u;

    const size_t payload_len = first_len + second_len;
    out[offset] = (uint8_t)(payload_len + 1u);
    out[offset + 1u] = type;
    if (first_len != 0u)
        memcpy(&out[offset + 2u], first, first_len);
    if (second_len != 0u)
        memcpy(&out[offset + 2u + first_len], second, second_len);
    return offset + payload_len + 2u;
}

static h2_pal_result_t ble_append_uuid_list(
    uint8_t *out,
    size_t *offset,
    size_t capacity,
    const h2_pal_ble_adv_data_t *data,
    size_t uuid_len,
    uint8_t ad_type)
{
    size_t encoded_len = 0u;

    if (out == NULL || offset == NULL) return H2_PAL_ERR_INVALID_ARG;
    for (size_t i = 0u; i < data->service_uuid_count; ++i) {
        const h2_pal_ble_uuid_t *uuid = &data->service_uuids[i];
        if (uuid->data == NULL ||
            (uuid->len != 2u && uuid->len != 4u && uuid->len != 16u))
            return H2_PAL_ERR_INVALID_ARG;
        if (uuid->len != uuid_len) continue;
        if (encoded_len > 254u - uuid_len)
            return H2_PAL_ERR_NO_SPACE;
        encoded_len += uuid_len;
    }
    if (encoded_len == 0u) return H2_PAL_OK;
    if (*offset > capacity || encoded_len + 2u > capacity - *offset)
        return H2_PAL_ERR_NO_SPACE;

    size_t write_offset = *offset;
    out[write_offset++] = (uint8_t)(encoded_len + 1u);
    out[write_offset++] = ad_type;
    for (size_t i = 0u; i < data->service_uuid_count; ++i) {
        const h2_pal_ble_uuid_t *uuid = &data->service_uuids[i];
        if (uuid->len != uuid_len) continue;
        memcpy(&out[write_offset], uuid->data, uuid_len);
        write_offset += uuid_len;
    }
    *offset = write_offset;
    return H2_PAL_OK;
}

static h2_pal_result_t ble_encode_adv_data(
    const h2_pal_ble_adv_data_t *data,
    uint8_t *out,
    size_t out_size,
    bool include_flags,
    uint16_t *out_len,
    uint16_t *out_name_offset,
    uint8_t *out_name_len)
{
    static const uint8_t flags = 0x06u;
    size_t offset = 0u;
    size_t device_name_offset = 0u;
    size_t device_name_len = 0u;

    if (data == NULL ||
        (data->service_uuid_count != 0u && data->service_uuids == NULL) ||
        (data->manufacturer_data.len != 0u &&
         data->manufacturer_data.data == NULL) ||
        (data->service_data_uuid.len != 0u &&
         data->service_data_uuid.data == NULL) ||
        (data->service_data.len != 0u && data->service_data.data == NULL))
        return H2_PAL_ERR_INVALID_ARG;

    if (include_flags) {
        size_t next = ble_append_ad(
            out, offset, out_size,
            BK3633_BLE_AD_TYPE_FLAGS, &flags, sizeof(flags));
        if (next == 0u) return H2_PAL_ERR_NO_SPACE;
        offset = next;
    }

    h2_pal_result_t result = ble_append_uuid_list(
        out, &offset, out_size, data,
        2u, BK3633_BLE_AD_TYPE_UUID16_COMPLETE);
    if (result != H2_PAL_OK) return result;
    result = ble_append_uuid_list(
        out, &offset, out_size, data,
        4u, BK3633_BLE_AD_TYPE_UUID32_COMPLETE);
    if (result != H2_PAL_OK) return result;
    result = ble_append_uuid_list(
        out, &offset, out_size, data,
        16u, BK3633_BLE_AD_TYPE_UUID128_COMPLETE);
    if (result != H2_PAL_OK) return result;

    if (data->manufacturer_data.len != 0u) {
        size_t next = ble_append_ad(out, offset, out_size,
                                    BK3633_BLE_AD_TYPE_MANUFACTURER,
                                    data->manufacturer_data.data,
                                    data->manufacturer_data.len);
        if (next == 0u) return H2_PAL_ERR_NO_SPACE;
        offset = next;
    }
    if (data->service_data.len != 0u) {
        size_t uuid_len = data->service_data_uuid.len;
        uint8_t ad_type = BK3633_BLE_AD_TYPE_SERVICE_DATA16;
        if (uuid_len != 0u && uuid_len != 2u &&
            uuid_len != 4u && uuid_len != 16u)
            return H2_PAL_ERR_INVALID_ARG;
        if (uuid_len > 254u ||
            data->service_data.len > 254u - uuid_len)
            return H2_PAL_ERR_NO_SPACE;
        if (uuid_len == 4u) ad_type = BK3633_BLE_AD_TYPE_SERVICE_DATA32;
        if (uuid_len == 16u) ad_type = BK3633_BLE_AD_TYPE_SERVICE_DATA128;
        size_t next = ble_append_ad_parts(
            out, offset, out_size, ad_type,
            data->service_data_uuid.data, uuid_len,
            data->service_data.data, data->service_data.len);
        if (next == 0u) return H2_PAL_ERR_NO_SPACE;
        offset = next;
    }
    if (data->local_name != NULL) {
        size_t name_len = strlen(data->local_name);
        device_name_offset = offset + 2u;
        device_name_len = name_len;
        size_t next = ble_append_ad(
            out, offset, out_size,
            BK3633_BLE_AD_TYPE_NAME_COMPLETE,
            (const uint8_t *)data->local_name, name_len);
        if (next == 0u) return H2_PAL_ERR_NO_SPACE;
        offset = next;
    }
    *out_len = (uint16_t)offset;
    *out_name_offset = (uint16_t)device_name_offset;
    *out_name_len = (uint8_t)device_name_len;
    return H2_PAL_OK;
}

static h2_pal_result_t ble_build_adv_data(const h2_pal_ble_adv_data_t *data)
{
    return ble_encode_adv_data(
        data, s_ble_state.adv_data, sizeof(s_ble_state.adv_data),
        true,
        &s_ble_state.adv_data_len, &s_ble_state.device_name_offset,
        &s_ble_state.device_name_len);
}

static h2_pal_result_t ble_send_adv_data(void)
{
    if (s_ble_state.adv_data_pending)
        return H2_PAL_ERR_INVALID_STATE;
    if (s_ble_state.pending_adv_data_count >=
        BK3633_BLE_ADV_OPERATION_CAPACITY)
        return H2_PAL_ERR_BUSY;
    if (s_ble_state.adv_params.type == H2_PAL_BLE_ADV_TYPE_LEGACY &&
        s_ble_state.adv_data_len > H2_PAL_BLE_LEGACY_ADV_DATA_MAX_LEN)
        return H2_PAL_ERR_NO_SPACE;
    struct gapm_set_adv_data_cmd *cmd = KE_MSG_ALLOC_DYN(
        GAPM_SET_ADV_DATA_CMD, TASK_GAPM, TASK_APP, gapm_set_adv_data_cmd,
        s_ble_state.adv_data_len);
    if (cmd == NULL) return H2_PAL_ERR_NO_MEMORY;
    cmd->operation = GAPM_SET_ADV_DATA;
    cmd->actv_idx = s_ble_state.adv_actv_idx;
    cmd->length = s_ble_state.adv_data_len;
    memcpy(cmd->data, s_ble_state.adv_data, s_ble_state.adv_data_len);
    if (!ble_adv_queue_push(
            s_ble_state.pending_adv_data,
            s_ble_state.pending_adv_data_head,
            &s_ble_state.pending_adv_data_count,
            BK3633_BLE_ADV_CREATE_LEGACY))
        return H2_PAL_ERR_BUSY;
    s_ble_state.adv_data_pending = true;
    ke_msg_send(cmd);
    return H2_PAL_OK;
}

static h2_pal_result_t ble_send_create_adv(const h2_pal_ble_adv_params_t *params)
{
    if (s_ble_state.pending_adv_create_count >=
        BK3633_BLE_ADV_OPERATION_CAPACITY)
        return H2_PAL_ERR_BUSY;
    struct gapm_activity_create_adv_cmd *cmd = KE_MSG_ALLOC(
        GAPM_ACTIVITY_CREATE_CMD, TASK_GAPM, TASK_APP, gapm_activity_create_adv_cmd);
    if (cmd == NULL) return H2_PAL_ERR_NO_MEMORY;
    memset(cmd, 0, sizeof(*cmd));
    cmd->operation = GAPM_CREATE_ADV_ACTIVITY;
    cmd->own_addr_type = GAPM_STATIC_ADDR;
    cmd->adv_param.type = GAPM_ADV_TYPE_LEGACY;
    cmd->adv_param.disc_mode = GAPM_ADV_MODE_GEN_DISC;
    if (params->type == H2_PAL_BLE_ADV_TYPE_EXTENDED) {
        cmd->adv_param.type = GAPM_ADV_TYPE_EXTENDED;
        cmd->adv_param.prop = params->mode == H2_PAL_BLE_ADV_MODE_CONNECTABLE
            ? GAPM_EXT_ADV_PROP_UNDIR_CONN_MASK
            : GAPM_EXT_ADV_PROP_NON_CONN_NON_SCAN_MASK;
    } else {
        cmd->adv_param.prop = params->mode == H2_PAL_BLE_ADV_MODE_CONNECTABLE
            ? GAPM_ADV_PROP_UNDIR_CONN_MASK : GAPM_ADV_PROP_BROADCAST_NON_SCAN_MASK;
    }
    /* SDK's default policy: accept any scanner/initiator. */
    cmd->adv_param.filter_pol = 0u;
    cmd->adv_param.prim_cfg.chnl_map = 0x07u;
    cmd->adv_param.prim_cfg.phy = params->primary_phy == H2_PAL_BLE_PHY_CODED
        ? GAP_PHY_LE_CODED : GAP_PHY_LE_1MBPS;
    cmd->adv_param.second_cfg.phy = params->secondary_phy == H2_PAL_BLE_PHY_2M
        ? GAP_PHY_LE_2MBPS : params->secondary_phy == H2_PAL_BLE_PHY_CODED
            ? GAP_PHY_LE_CODED : GAP_PHY_LE_1MBPS;
    cmd->adv_param.second_cfg.adv_sid = params->sid & 0x0fu;
    cmd->adv_param.prim_cfg.adv_intv_min = ble_ms_to_625us(params->interval_min_ms);
    cmd->adv_param.prim_cfg.adv_intv_max = ble_ms_to_625us(params->interval_max_ms);
    if (!ble_adv_queue_push(
            s_ble_state.pending_adv_create,
            s_ble_state.pending_adv_create_head,
            &s_ble_state.pending_adv_create_count,
            BK3633_BLE_ADV_CREATE_LEGACY))
        return H2_PAL_ERR_BUSY;
    s_ble_state.adv_create_pending = true;
    ke_msg_send(cmd);
    return H2_PAL_OK;
}

static h2_pal_result_t ble_send_start_adv(void)
{
    if (s_ble_state.pending_start_count >=
        sizeof(s_ble_state.pending_start_kind))
        return H2_PAL_ERR_INVALID_STATE;
    struct gapm_activity_start_cmd *cmd = KE_MSG_ALLOC(
        GAPM_ACTIVITY_START_CMD, TASK_GAPM, TASK_APP, gapm_activity_start_cmd);
    if (cmd == NULL) return H2_PAL_ERR_NO_MEMORY;
    memset(cmd, 0, sizeof(*cmd));
    cmd->operation = GAPM_START_ACTIVITY;
    cmd->actv_idx = s_ble_state.adv_actv_idx;
    cmd->u_param.adv_add_param.duration =
        s_ble_state.adv_params.duration_ms == 0u
            ? 0u
            : ble_ms_to_10ms(s_ble_state.adv_params.duration_ms);
    cmd->u_param.adv_add_param.max_adv_evt = s_ble_state.adv_params.max_adv_events;
    if (!ble_start_queue_push(BK3633_BLE_ACTIVITY_KIND_ADV))
        return H2_PAL_ERR_INVALID_STATE;
    s_ble_state.adv_start_pending = true;
    ke_msg_send(cmd);
    return H2_PAL_OK;
}

static h2_pal_result_t ble_send_create_adv_set(
    h2_pal_ble_adv_set_t *set, uint8_t slot)
{
    if (s_ble_state.pending_adv_create_count >=
        BK3633_BLE_ADV_OPERATION_CAPACITY)
        return H2_PAL_ERR_BUSY;
    struct gapm_activity_create_adv_cmd *cmd = KE_MSG_ALLOC(
        GAPM_ACTIVITY_CREATE_CMD, TASK_GAPM, TASK_APP,
        gapm_activity_create_adv_cmd);
    if (cmd == NULL) return H2_PAL_ERR_NO_MEMORY;
    memset(cmd, 0, sizeof(*cmd));
    cmd->operation = GAPM_CREATE_ADV_ACTIVITY;
    cmd->own_addr_type = GAPM_STATIC_ADDR;
    cmd->adv_param.type = set->params.type == H2_PAL_BLE_ADV_TYPE_EXTENDED
        ? GAPM_ADV_TYPE_EXTENDED : GAPM_ADV_TYPE_LEGACY;
    cmd->adv_param.disc_mode = GAPM_ADV_MODE_GEN_DISC;
    if (set->params.type == H2_PAL_BLE_ADV_TYPE_EXTENDED) {
        cmd->adv_param.prop =
            set->params.mode == H2_PAL_BLE_ADV_MODE_CONNECTABLE
                ? GAPM_EXT_ADV_PROP_UNDIR_CONN_MASK
                : GAPM_EXT_ADV_PROP_NON_CONN_NON_SCAN_MASK;
    } else {
        cmd->adv_param.prop =
            set->params.mode == H2_PAL_BLE_ADV_MODE_CONNECTABLE
                ? GAPM_ADV_PROP_UNDIR_CONN_MASK
                : GAPM_ADV_PROP_BROADCAST_NON_SCAN_MASK;
    }
    cmd->adv_param.filter_pol = 0u;
    cmd->adv_param.prim_cfg.chnl_map = 0x07u;
    cmd->adv_param.prim_cfg.phy =
        set->params.primary_phy == H2_PAL_BLE_PHY_CODED
            ? GAP_PHY_LE_CODED : GAP_PHY_LE_1MBPS;
    cmd->adv_param.second_cfg.phy =
        set->params.secondary_phy == H2_PAL_BLE_PHY_2M
            ? GAP_PHY_LE_2MBPS
            : set->params.secondary_phy == H2_PAL_BLE_PHY_CODED
                  ? GAP_PHY_LE_CODED : GAP_PHY_LE_1MBPS;
    cmd->adv_param.second_cfg.adv_sid = set->params.sid & 0x0fu;
    cmd->adv_param.prim_cfg.adv_intv_min =
        ble_ms_to_625us(set->params.interval_min_ms);
    cmd->adv_param.prim_cfg.adv_intv_max =
        ble_ms_to_625us(set->params.interval_max_ms);
    if (!ble_adv_queue_push(
            s_ble_state.pending_adv_create,
            s_ble_state.pending_adv_create_head,
            &s_ble_state.pending_adv_create_count, slot))
        return H2_PAL_ERR_BUSY;
    set->create_pending = true;
    ke_msg_send(cmd);
    return H2_PAL_OK;
}

static h2_pal_result_t ble_send_adv_set_data(
    h2_pal_ble_adv_set_t *set, uint8_t slot)
{
    if (s_ble_state.pending_adv_data_count >=
        BK3633_BLE_ADV_OPERATION_CAPACITY)
        return H2_PAL_ERR_BUSY;
    struct gapm_set_adv_data_cmd *cmd = KE_MSG_ALLOC_DYN(
        GAPM_SET_ADV_DATA_CMD, TASK_GAPM, TASK_APP,
        gapm_set_adv_data_cmd, set->data_len);
    if (cmd == NULL) return H2_PAL_ERR_NO_MEMORY;
    cmd->operation = GAPM_SET_ADV_DATA;
    cmd->actv_idx = set->activity_index;
    cmd->length = set->data_len;
    if (set->data_len != 0u) memcpy(cmd->data, set->data, set->data_len);
    if (!ble_adv_queue_push(
            s_ble_state.pending_adv_data,
            s_ble_state.pending_adv_data_head,
            &s_ble_state.pending_adv_data_count, slot))
        return H2_PAL_ERR_BUSY;
    set->data_pending = true;
    ke_msg_send(cmd);
    return H2_PAL_OK;
}

static h2_pal_result_t ble_send_adv_set_scan_response_data(
    h2_pal_ble_adv_set_t *set, uint8_t slot)
{
    if (s_ble_state.pending_adv_data_count >=
        BK3633_BLE_ADV_OPERATION_CAPACITY)
        return H2_PAL_ERR_BUSY;
    struct gapm_set_adv_data_cmd *cmd = KE_MSG_ALLOC_DYN(
        GAPM_SET_ADV_DATA_CMD, TASK_GAPM, TASK_APP,
        gapm_set_adv_data_cmd, set->scan_response_data_len);
    if (cmd == NULL) return H2_PAL_ERR_NO_MEMORY;
    cmd->operation = GAPM_SET_SCAN_RSP_DATA;
    cmd->actv_idx = set->activity_index;
    cmd->length = set->scan_response_data_len;
    if (set->scan_response_data_len != 0u)
        memcpy(cmd->data, ble_adv_set_scan_response_data(set),
               set->scan_response_data_len);
    if (!ble_adv_queue_push(
            s_ble_state.pending_adv_data,
            s_ble_state.pending_adv_data_head,
            &s_ble_state.pending_adv_data_count, slot))
        return H2_PAL_ERR_BUSY;
    set->scan_response_pending = true;
    ke_msg_send(cmd);
    return H2_PAL_OK;
}

static h2_pal_result_t ble_send_start_adv_set(
    h2_pal_ble_adv_set_t *set, uint8_t slot)
{
    struct gapm_activity_start_cmd *cmd = KE_MSG_ALLOC(
        GAPM_ACTIVITY_START_CMD, TASK_GAPM, TASK_APP,
        gapm_activity_start_cmd);
    if (cmd == NULL) return H2_PAL_ERR_NO_MEMORY;
    memset(cmd, 0, sizeof(*cmd));
    cmd->operation = GAPM_START_ACTIVITY;
    cmd->actv_idx = set->activity_index;
    cmd->u_param.adv_add_param.duration = set->params.duration_ms == 0u
        ? 0u : ble_ms_to_10ms(set->params.duration_ms);
    cmd->u_param.adv_add_param.max_adv_evt = set->params.max_adv_events;
    if (!ble_start_queue_push(
            (uint8_t)(BK3633_BLE_ACTIVITY_KIND_ADV_SET_BASE + slot)))
        return H2_PAL_ERR_BUSY;
    set->start_pending = true;
    ke_msg_send(cmd);
    return H2_PAL_OK;
}

static h2_pal_result_t ble_send_delete_adv_set(
    h2_pal_ble_adv_set_t *set, uint8_t slot)
{
    if (s_ble_state.pending_adv_delete_count >=
        BK3633_BLE_ADV_OPERATION_CAPACITY)
        return H2_PAL_ERR_BUSY;
    struct gapm_activity_delete_cmd *cmd = KE_MSG_ALLOC(
        GAPM_ACTIVITY_DELETE_CMD, TASK_GAPM, TASK_APP,
        gapm_activity_delete_cmd);
    if (cmd == NULL) return H2_PAL_ERR_NO_MEMORY;
    cmd->operation = GAPM_DELETE_ACTIVITY;
    cmd->actv_idx = set->activity_index;
    if (!ble_adv_queue_push(
            s_ble_state.pending_adv_delete,
            s_ble_state.pending_adv_delete_head,
            &s_ble_state.pending_adv_delete_count, slot))
        return H2_PAL_ERR_BUSY;
    set->delete_pending = true;
    ke_msg_send(cmd);
    return H2_PAL_OK;
}

static size_t scan_copy_ad_data(
    const uint8_t *data,
    size_t length,
    h2_pal_ble_scan_result_t *result,
    h2_pal_ble_uuid_t *uuids,
    size_t uuid_capacity)
{
    size_t offset = 0u;
    size_t uuid_count = 0u;

    while (offset < length) {
        uint8_t ad_len = data[offset];
        const uint8_t *ad;
        uint8_t ad_type;
        size_t value_len;

        if (ad_len == 0u) {
            break;
        }
        if ((size_t)ad_len + 1u > length - offset) {
            break;
        }

        ad = &data[offset + 1u];
        ad_type = ad[0];
        value_len = (size_t)ad_len - 1u;

        switch (ad_type) {
        case BK3633_BLE_AD_TYPE_NAME_SHORT:
        case BK3633_BLE_AD_TYPE_NAME_COMPLETE:
            result->local_name = (const char *)&ad[1];
            result->local_name_len = value_len;
            break;
        case BK3633_BLE_AD_TYPE_MANUFACTURER:
            if (value_len >= 2u) {
                result->manufacturer_data.data = &ad[1];
                result->manufacturer_data.len = value_len;
            }
            break;
        case BK3633_BLE_AD_TYPE_SERVICE_DATA16:
        case BK3633_BLE_AD_TYPE_SERVICE_DATA32:
        case BK3633_BLE_AD_TYPE_SERVICE_DATA128:
            if (value_len != 0u) {
                result->service_data.data = &ad[1];
                result->service_data.len = value_len;
            }
            break;
        case BK3633_BLE_AD_TYPE_UUID16_INCOMPLETE:
        case BK3633_BLE_AD_TYPE_UUID16_COMPLETE:
            for (size_t i = 0u; i + 1u < value_len && uuid_count < uuid_capacity;
                 i += 2u) {
                uuids[uuid_count++] = (h2_pal_ble_uuid_t){
                    .data = &ad[1u + i],
                    .len = 2u,
                };
            }
            break;
        case BK3633_BLE_AD_TYPE_UUID32_INCOMPLETE:
        case BK3633_BLE_AD_TYPE_UUID32_COMPLETE:
            for (size_t i = 0u; i + 3u < value_len && uuid_count < uuid_capacity;
                 i += 4u) {
                uuids[uuid_count++] = (h2_pal_ble_uuid_t){
                    .data = &ad[1u + i],
                    .len = 4u,
                };
            }
            break;
        case BK3633_BLE_AD_TYPE_UUID128_INCOMPLETE:
        case BK3633_BLE_AD_TYPE_UUID128_COMPLETE:
            for (size_t i = 0u; i + 15u < value_len && uuid_count < uuid_capacity;
                 i += 16u) {
                uuids[uuid_count++] = (h2_pal_ble_uuid_t){
                    .data = &ad[1u + i],
                    .len = 16u,
                };
            }
            break;
        default:
            break;
        }

        offset += (size_t)ad_len + 1u;
    }

    return uuid_count;
}

static void ble_enqueue_scan_report(
    const struct gapm_ext_adv_report_ind *report)
{
    if (report == NULL || s_ble_state.scan_callback == NULL) {
        return;
    }
    if (s_ble_state.pending_scan_count == BK3633_BLE_SCAN_REPORT_CAPACITY) {
        /* Passive scan reports are an observational stream, not lifecycle
         * facts. A dense RF environment can deliver more reports in one
         * rwip_schedule() turn than root can project immediately. Keep the
         * newest bounded window instead of turning expected scan loss into a
         * fatal SDK Runtime dispatch error. */
        memset(&s_ble_state.pending_scan_reports[
                   s_ble_state.pending_scan_head],
               0, sizeof(s_ble_state.pending_scan_reports[0]));
        s_ble_state.pending_scan_head = (uint8_t)(
            (s_ble_state.pending_scan_head + 1u) %
            BK3633_BLE_SCAN_REPORT_CAPACITY);
        --s_ble_state.pending_scan_count;
    }
    uint8_t tail = (uint8_t)(
        (s_ble_state.pending_scan_head + s_ble_state.pending_scan_count) %
        BK3633_BLE_SCAN_REPORT_CAPACITY);
    h2_bk3633_ble_scan_report_t *pending =
        &s_ble_state.pending_scan_reports[tail];
    memset(pending, 0, sizeof(*pending));
    memcpy(pending->addr.value, report->trans_addr.addr.addr,
           H2_PAL_BLE_ADDR_LEN);
    pending->addr.type = report->trans_addr.addr_type != 0u
        ? H2_PAL_BLE_ADDR_TYPE_RANDOM
        : H2_PAL_BLE_ADDR_TYPE_PUBLIC;
    pending->rssi = (int)report->rssi;
    pending->connectable =
        (report->info & GAPM_REPORT_INFO_CONN_ADV_BIT) != 0u;
    uint8_t report_type =
        report->info & GAPM_REPORT_INFO_REPORT_TYPE_MASK;
    pending->scan_response =
        report_type == GAPM_REPORT_TYPE_SCAN_RSP_EXT ||
        report_type == GAPM_REPORT_TYPE_SCAN_RSP_LEG;
    bool legacy_report = report_type == GAPM_REPORT_TYPE_ADV_LEG ||
        report_type == GAPM_REPORT_TYPE_SCAN_RSP_LEG;
    pending->adv_type = legacy_report ? H2_PAL_BLE_ADV_TYPE_LEGACY
                                      : H2_PAL_BLE_ADV_TYPE_EXTENDED;
    pending->primary_phy = report->phy_prim == GAPM_PHY_TYPE_LE_CODED
        ? H2_PAL_BLE_PHY_CODED : H2_PAL_BLE_PHY_1M;
    pending->secondary_phy = legacy_report ? H2_PAL_BLE_PHY_UNKNOWN
        : report->phy_second == GAPM_PHY_TYPE_LE_2M ? H2_PAL_BLE_PHY_2M
        : report->phy_second == GAPM_PHY_TYPE_LE_CODED ? H2_PAL_BLE_PHY_CODED
        : H2_PAL_BLE_PHY_1M;
    pending->sid = legacy_report ? 0u : report->adv_sid;
    pending->data_status =
        (report->info & GAPM_REPORT_INFO_COMPLETE_BIT) != 0u
        ? H2_PAL_BLE_ADV_DATA_COMPLETE : H2_PAL_BLE_ADV_DATA_INCOMPLETE;
    pending->tx_power = report->tx_pwr;
    pending->data_len = report->length;
    if (pending->data_len > sizeof(pending->data)) {
        pending->data_len = sizeof(pending->data);
        pending->data_status = H2_PAL_BLE_ADV_DATA_TRUNCATED;
    }
    if (pending->data_len != 0u) {
        memcpy(pending->data, report->data, pending->data_len);
    }
    ++s_ble_state.pending_scan_count;
}

static h2_pal_result_t ble_dispatch_pending_scan_reports(void)
{
    while (s_ble_state.pending_scan_count != 0u &&
           s_ble_state.scan_callback != NULL) {
        h2_bk3633_ble_scan_report_t *pending =
            &s_ble_state.pending_scan_reports[
                s_ble_state.pending_scan_head];
        h2_pal_ble_scan_result_t result = {
            .addr = pending->addr,
            .rssi = pending->rssi,
            .connectable = pending->connectable,
            .scan_response = pending->scan_response,
            .adv_type = pending->adv_type,
            .primary_phy = pending->primary_phy,
            .secondary_phy = pending->secondary_phy,
            .sid = pending->sid,
            .data_status = pending->data_status,
            .tx_power = pending->tx_power,
            .raw_data = {
                .data = pending->data,
                .len = pending->data_len,
            },
        };
        h2_pal_ble_uuid_t uuids[8];
        size_t uuid_count = scan_copy_ad_data(
            pending->data, pending->data_len, &result, uuids, 8u);
        result.service_uuids = uuids;
        result.service_uuid_count = uuid_count;
        h2_pal_ble_scan_result_fn callback = s_ble_state.scan_callback;
        void *callback_user = s_ble_state.scan_user;

        s_ble_state.pending_scan_head = (uint8_t)(
            (s_ble_state.pending_scan_head + 1u) %
            BK3633_BLE_SCAN_REPORT_CAPACITY);
        --s_ble_state.pending_scan_count;
        bool stop_requested = callback(callback_user, &result);
        memset(pending, 0, sizeof(*pending));

        if (stop_requested) {
            ble_clear_pending_scan_reports();
            s_ble_state.scan_callback = NULL;
            s_ble_state.scan_user = NULL;
            if (s_ble_state.scan_started &&
                !s_ble_state.scan_stop_pending) {
                h2_pal_result_t stop_result = ble_send_stop(
                    s_ble_state.scan_actv_idx,
                    BK3633_BLE_ACTIVITY_KIND_SCAN);
                if (stop_result == H2_PAL_OK)
                    s_ble_state.scan_stop_pending = true;
            }
            break;
        }
    }
    return H2_PAL_OK;
}

static h2_pal_result_t ble_start(void *user)
{
    uint64_t started_ms = 0u;
    (void)user;
    if (s_ble_state.host_stop_pending ||
        s_ble_state.host_stopped_event_pending) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    h2_pal_result_t result = h2_bk3633_platform_ble_host_status();
    if (result == H2_PAL_ERR_WOULD_BLOCK) {
        result = h2_pal_time_get_monotonic_ms(s_ble_time, &started_ms);
        if (result != H2_PAL_OK) {
            return result;
        }
        result = H2_PAL_ERR_WOULD_BLOCK;
    }
    while (result == H2_PAL_ERR_WOULD_BLOCK) {
        result = ble_wait_until(
            (uintptr_t)&s_ble_bootstrap_wait_key,
            started_ms,
            s_ble_bootstrap_timeout_ms);
        if (result != H2_PAL_OK) {
            return result;
        }
        result = h2_bk3633_platform_ble_host_status();
    }
    if (result == H2_PAL_OK && !s_ble_state.host_started_event_posted) {
        s_ble_state.host_started_event_posted = true;
        ble_post_event(H2_PAL_SYSTEM_EVENT_TYPE_BLE_HOST_STARTED, NULL, 0u);
    }
    return result;
}

static h2_pal_result_t ble_stop(void *user)
{
    (void)user;
    if (s_ble_state.host_stop_pending ||
        s_ble_state.host_stopped_event_pending) {
        return H2_PAL_OK;
    }
    if (!s_ble_state.host_started_event_posted)
        return H2_PAL_OK;

    h2_pal_result_t result =
        ble_reject_pending_accesses(H2_PAL_ERR_CLOSED);
    if (result != H2_PAL_OK) return result;
    ble_complete_indication(H2_PAL_ERR_CLOSED);

    struct gapm_reset_cmd *cmd = KE_MSG_ALLOC(
        GAPM_RESET_CMD, TASK_GAPM, TASK_APP, gapm_reset_cmd);
    if (cmd == NULL) return H2_PAL_ERR_NO_MEMORY;
    cmd->operation = GAPM_RESET;

    (void)ble_unregister_gatt_services(NULL);
    h2_pal_result_t host_status = s_ble_state.host_status;
    bool host_bootstrap_started = s_ble_state.host_bootstrap_started;
    s_ble_state.scan_callback = NULL;
    s_ble_state.scan_user = NULL;
    ble_clear_pending_scan_reports();
    memset(&s_ble_state, 0, sizeof(s_ble_state));
    s_ble_state.host_status = host_status;
    s_ble_state.host_bootstrap_started = host_bootstrap_started;
    s_ble_state.host_stop_pending = true;
    ke_msg_send(cmd);
    return H2_PAL_OK;
}

h2_pal_result_t h2_bk3633_platform_ble_host_bootstrap_begin(void)
{
    if (s_ble_state.host_bootstrap_started) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    s_ble_state.host_bootstrap_started = true;
    s_ble_state.host_status = H2_PAL_ERR_WOULD_BLOCK;
    return H2_PAL_OK;
}

void h2_bk3633_platform_ble_host_bootstrap_complete(
    h2_pal_result_t result)
{
    if (!s_ble_state.host_bootstrap_started ||
        s_ble_state.host_status != H2_PAL_ERR_WOULD_BLOCK) {
        return;
    }
    s_ble_state.host_status =
        result == H2_PAL_OK ? H2_PAL_OK : result;
    (void)h2_bk3633_platform_libco_record_completion(
        (uintptr_t)&s_ble_bootstrap_wait_key);
}

h2_pal_result_t h2_bk3633_platform_ble_host_status(void)
{
    if (!s_ble_state.host_bootstrap_started) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    return s_ble_state.host_status;
}

static h2_pal_result_t ble_set_adv_data(
    void *user, const h2_pal_ble_adv_data_t *data)
{
    (void)user;
    if (h2_bk3633_platform_ble_host_status() != H2_PAL_OK)
        return H2_PAL_ERR_INVALID_STATE;
    if (s_ble_state.adv_data_pending)
        return H2_PAL_ERR_INVALID_STATE;
    h2_pal_result_t result = ble_build_adv_data(data);
    if (result != H2_PAL_OK) return result;
    return s_ble_state.adv_created ? ble_send_adv_data() : H2_PAL_OK;
}

static bool ble_adv_activity_params_equal(
    const h2_pal_ble_adv_params_t *left,
    const h2_pal_ble_adv_params_t *right)
{
    return left->mode == right->mode &&
           left->interval_min_ms == right->interval_min_ms &&
           left->interval_max_ms == right->interval_max_ms &&
           left->type == right->type &&
           left->primary_phy == right->primary_phy &&
           left->secondary_phy == right->secondary_phy &&
           left->sid == right->sid;
}

static h2_pal_result_t ble_validate_adv_params(
    const h2_pal_ble_adv_params_t *params)
{
    if (params == NULL || params->interval_min_ms == 0u ||
        params->interval_max_ms < params->interval_min_ms)
        return H2_PAL_ERR_INVALID_ARG;
    if (params->type == H2_PAL_BLE_ADV_TYPE_EXTENDED) {
#if !CFG_BLE_EXT_ADV
        return H2_PAL_ERR_UNSUPPORTED;
#endif
        if (params->primary_phy == H2_PAL_BLE_PHY_2M)
            return H2_PAL_ERR_INVALID_ARG;
    } else if (params->type != H2_PAL_BLE_ADV_TYPE_LEGACY ||
               params->primary_phy == H2_PAL_BLE_PHY_CODED ||
               params->primary_phy == H2_PAL_BLE_PHY_2M ||
               (params->secondary_phy != H2_PAL_BLE_PHY_UNKNOWN &&
                params->secondary_phy != H2_PAL_BLE_PHY_1M)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    return H2_PAL_OK;
}

static h2_pal_result_t ble_start_advertising(
    void *user, const h2_pal_ble_adv_params_t *params)
{
    (void)user;
    if (h2_bk3633_platform_ble_host_status() != H2_PAL_OK)
        return H2_PAL_ERR_INVALID_STATE;
    h2_pal_result_t validation = ble_validate_adv_params(params);
    if (validation != H2_PAL_OK) return validation;
    if (params->type == H2_PAL_BLE_ADV_TYPE_LEGACY &&
        s_ble_state.adv_data_len > H2_PAL_BLE_LEGACY_ADV_DATA_MAX_LEN)
        return H2_PAL_ERR_NO_SPACE;
    if (s_ble_state.adv_create_pending || s_ble_state.adv_started ||
        s_ble_state.adv_start_pending || s_ble_state.adv_stop_pending ||
        s_ble_state.adv_data_pending)
        return H2_PAL_ERR_INVALID_STATE;
    if (s_ble_state.adv_created) {
        if (!ble_adv_activity_params_equal(&s_ble_state.adv_params, params))
            return H2_PAL_ERR_INVALID_STATE;
        s_ble_state.adv_params.duration_ms = params->duration_ms;
        s_ble_state.adv_params.max_adv_events = params->max_adv_events;
        return ble_send_start_adv();
    }
    s_ble_state.adv_params = *params;
    return ble_send_create_adv(params);
}

static h2_pal_result_t ble_stop_advertising(void *user)
{
    (void)user;
    if (h2_bk3633_platform_ble_host_status() != H2_PAL_OK)
        return H2_PAL_ERR_INVALID_STATE;
    if (!s_ble_state.adv_created || !s_ble_state.adv_started ||
        s_ble_state.adv_stop_pending)
        return H2_PAL_ERR_INVALID_STATE;
    h2_pal_result_t result = ble_send_stop(
        s_ble_state.adv_actv_idx, BK3633_BLE_ACTIVITY_KIND_ADV);
    if (result == H2_PAL_OK) s_ble_state.adv_stop_pending = true;
    return result;
}

static h2_pal_result_t ble_adv_set_create(
    void *user,
    const h2_pal_ble_adv_params_t *params,
    h2_pal_ble_adv_set_t **out_set)
{
    (void)user;
    if (out_set != NULL) *out_set = NULL;
    if (h2_bk3633_platform_ble_host_status() != H2_PAL_OK)
        return H2_PAL_ERR_INVALID_STATE;
    if (out_set == NULL)
        return H2_PAL_ERR_INVALID_ARG;
    h2_pal_result_t validation = ble_validate_adv_params(params);
    if (validation != H2_PAL_OK) return validation;
    for (size_t i = 0u; i < BK3633_BLE_ADV_SET_MAX; ++i) {
        h2_pal_ble_adv_set_t *set = &s_ble_state.adv_sets[i];
        if (!set->used) {
            memset(set, 0, sizeof(*set));
            set->used = true;
            set->params = *params;
            *out_set = set;
            return H2_PAL_OK;
        }
    }
    return H2_PAL_ERR_NO_SPACE;
}

static h2_pal_result_t ble_adv_set_set_data(
    void *user,
    h2_pal_ble_adv_set_t *set,
    const h2_pal_ble_adv_data_t *data)
{
    (void)user;
    int index = ble_adv_set_index(set);
    if (index < 0 || data == NULL)
        return H2_PAL_ERR_INVALID_ARG;
    if (set->started || set->start_pending || set->data_pending ||
        set->scan_response_pending ||
        set->delete_pending || set->destroy_requested)
        return H2_PAL_ERR_BUSY;
    size_t data_capacity = set->params.type == H2_PAL_BLE_ADV_TYPE_LEGACY
        ? H2_PAL_BLE_LEGACY_ADV_DATA_MAX_LEN
        : sizeof(set->data);
    h2_pal_result_t result = ble_encode_adv_data(
        data, set->data, data_capacity, true, &set->data_len,
        &set->device_name_offset, &set->device_name_len);
    if (result != H2_PAL_OK) return result;
    return set->created
        ? ble_send_adv_set_data(set, (uint8_t)index) : H2_PAL_OK;
}

static h2_pal_result_t ble_adv_set_set_scan_response_data(
    void *user,
    h2_pal_ble_adv_set_t *set,
    const h2_pal_ble_adv_data_t *data)
{
    (void)user;
    int index = ble_adv_set_index(set);
    if (index < 0 || data == NULL)
        return H2_PAL_ERR_INVALID_ARG;
    if (set->params.type != H2_PAL_BLE_ADV_TYPE_LEGACY ||
        set->params.mode != H2_PAL_BLE_ADV_MODE_CONNECTABLE)
        return H2_PAL_ERR_UNSUPPORTED;
    if (set->start_pending || set->data_pending ||
        set->scan_response_pending || set->stop_pending ||
        set->delete_pending || set->destroy_requested)
        return H2_PAL_ERR_BUSY;
    uint8_t encoded[H2_PAL_BLE_LEGACY_ADV_DATA_MAX_LEN];
    uint16_t encoded_len;
    uint16_t name_offset;
    uint8_t name_len;
    h2_pal_result_t result = ble_encode_adv_data(
        data, encoded,
        H2_PAL_BLE_LEGACY_ADV_DATA_MAX_LEN,
        false, &encoded_len, &name_offset, &name_len);
    if (result != H2_PAL_OK) return result;
    if (encoded_len != 0u) {
        memcpy(ble_adv_set_scan_response_data(set), encoded, encoded_len);
    }
    set->scan_response_data_len = encoded_len;
    set->scan_response_set = true;
    set->scan_response_configured = false;
    return set->created
        ? ble_send_adv_set_scan_response_data(set, (uint8_t)index)
        : H2_PAL_OK;
}

static h2_pal_result_t ble_adv_set_start(
    void *user, h2_pal_ble_adv_set_t *set)
{
    (void)user;
    int index = ble_adv_set_index(set);
    if (index < 0) return H2_PAL_ERR_INVALID_ARG;
    set->start_requested = true;
    if (set->started) return H2_PAL_OK;
    if (set->create_pending || set->data_pending ||
        set->scan_response_pending)
        return H2_PAL_OK;
    if (set->start_pending || set->stop_pending || set->delete_pending)
        return H2_PAL_ERR_BUSY;
    if (!set->created)
        return ble_send_create_adv_set(set, (uint8_t)index);
    if (set->scan_response_set && !set->scan_response_configured)
        return ble_send_adv_set_scan_response_data(set, (uint8_t)index);
    return ble_send_start_adv_set(set, (uint8_t)index);
}

static h2_pal_result_t ble_adv_set_stop(
    void *user, h2_pal_ble_adv_set_t *set)
{
    (void)user;
    int index = ble_adv_set_index(set);
    if (index < 0) return H2_PAL_ERR_INVALID_ARG;
    set->start_requested = false;
    if (!set->started && !set->start_pending) return H2_PAL_OK;
    if (set->start_pending || set->stop_pending) return H2_PAL_ERR_BUSY;
    h2_pal_result_t result = ble_send_stop(
        set->activity_index,
        (uint8_t)(BK3633_BLE_ACTIVITY_KIND_ADV_SET_BASE + index));
    if (result == H2_PAL_OK) set->stop_pending = true;
    return result;
}

static h2_pal_result_t ble_adv_set_destroy(
    void *user, h2_pal_ble_adv_set_t *set)
{
    (void)user;
    int index = ble_adv_set_index(set);
    if (index < 0) return H2_PAL_ERR_INVALID_ARG;
    set->start_requested = false;
    set->destroy_requested = true;
    if (set->delete_pending) return H2_PAL_OK;
    if (set->started && !set->stop_pending) {
        h2_pal_result_t result = ble_adv_set_stop(user, set);
        if (result != H2_PAL_OK) return result;
    }
    if (set->start_pending || set->stop_pending || set->create_pending ||
        set->data_pending || set->scan_response_pending) {
        return H2_PAL_OK;
    }
    if (set->created)
        return ble_send_delete_adv_set(set, (uint8_t)index);
    memset(set, 0, sizeof(*set));
    return H2_PAL_OK;
}

static h2_pal_result_t ble_start_scan(
    void *user,
    const h2_pal_ble_scan_params_t *params,
    h2_pal_ble_scan_result_fn on_result,
    void *scan_user)
{
#if defined(CFG_PERIPHERAL)
    (void)user;
    (void)params;
    (void)on_result;
    (void)scan_user;
    return H2_PAL_ERR_UNSUPPORTED;
#else
    (void)user;
    if (h2_bk3633_platform_ble_host_status() != H2_PAL_OK)
        return H2_PAL_ERR_INVALID_STATE;
    if (on_result == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (params == NULL || params->interval_ms == 0u || params->window_ms == 0u ||
        params->window_ms > params->interval_ms)
        return H2_PAL_ERR_INVALID_ARG;
    if (params->type != H2_PAL_BLE_SCAN_TYPE_LEGACY &&
        params->type != H2_PAL_BLE_SCAN_TYPE_EXTENDED)
        return H2_PAL_ERR_INVALID_ARG;
    if (params->type == H2_PAL_BLE_SCAN_TYPE_LEGACY &&
        params->phy_mask != 0u && params->phy_mask != H2_PAL_BLE_SCAN_PHY_1M)
        return H2_PAL_ERR_INVALID_ARG;
    if (params->type == H2_PAL_BLE_SCAN_TYPE_EXTENDED &&
        (params->phy_mask & (h2_pal_ble_scan_phy_mask_t)~H2_PAL_BLE_SCAN_PHY_ALL) != 0u)
        return H2_PAL_ERR_INVALID_ARG;
    if (s_ble_state.scan_create_pending || s_ble_state.scan_started ||
        s_ble_state.scan_start_pending || s_ble_state.scan_stop_pending)
        return H2_PAL_ERR_INVALID_STATE;
    s_ble_state.scan_callback = on_result;
    s_ble_state.scan_user = scan_user;
    s_ble_state.scan_params = *params;
    h2_pal_result_t result = s_ble_state.scan_created
        ? ble_send_start_scan(params)
        : ble_send_create_scan();
    if (result != H2_PAL_OK) {
        s_ble_state.scan_callback = NULL;
        s_ble_state.scan_user = NULL;
    }
    return result;
#endif
}

static h2_pal_result_t ble_stop_scan(void *user)
{
#if defined(CFG_PERIPHERAL)
    (void)user;
    return H2_PAL_ERR_UNSUPPORTED;
#else
    (void)user;
    if (h2_bk3633_platform_ble_host_status() != H2_PAL_OK)
        return H2_PAL_ERR_INVALID_STATE;
    if (!s_ble_state.scan_created || !s_ble_state.scan_started ||
        s_ble_state.scan_stop_pending) {
        s_ble_state.scan_callback = NULL;
        s_ble_state.scan_user = NULL;
        ble_clear_pending_scan_reports();
        return H2_PAL_OK;
    }
    h2_pal_result_t result = ble_send_stop(
        s_ble_state.scan_actv_idx, BK3633_BLE_ACTIVITY_KIND_SCAN);
    if (result == H2_PAL_OK) {
        s_ble_state.scan_stop_pending = true;
        s_ble_state.scan_callback = NULL;
        s_ble_state.scan_user = NULL;
        ble_clear_pending_scan_reports();
    }
    return result;
#endif
}

static uint16_t ble_gatt_read_security(uint32_t permissions)
{
    if ((permissions & H2_PAL_BLE_GATT_PERMISSION_READ_AUTHENTICATED) != 0u)
        return PERM(RP, AUTH);
    if ((permissions & H2_PAL_BLE_GATT_PERMISSION_READ_ENCRYPTED) != 0u)
        return PERM(RP, UNAUTH);
    return PERM(RP, NO_AUTH);
}

static uint16_t ble_gatt_write_security(uint32_t permissions)
{
    if ((permissions & H2_PAL_BLE_GATT_PERMISSION_WRITE_AUTHENTICATED) != 0u)
        return PERM(WP, AUTH);
    if ((permissions & H2_PAL_BLE_GATT_PERMISSION_WRITE_ENCRYPTED) != 0u)
        return PERM(WP, UNAUTH);
    return PERM(WP, NO_AUTH);
}

static uint16_t ble_gatt_properties(
    const h2_pal_ble_gatt_characteristic_t *ch)
{
    uint16_t props = 0u;
    if ((ch->properties & H2_PAL_BLE_GATT_PROPERTY_READ) != 0u)
        props |= PERM(RD, ENABLE) | ble_gatt_read_security(ch->permissions);
    if ((ch->properties & H2_PAL_BLE_GATT_PROPERTY_WRITE) != 0u)
        props |= PERM(WRITE_REQ, ENABLE) |
                 ble_gatt_write_security(ch->permissions);
    if ((ch->properties & H2_PAL_BLE_GATT_PROPERTY_WRITE_NO_RSP) != 0u)
        props |= PERM(WRITE_COMMAND, ENABLE) |
                 ble_gatt_write_security(ch->permissions);
    if ((ch->properties & H2_PAL_BLE_GATT_PROPERTY_NOTIFY) != 0u)
        props |= PERM(NTF, ENABLE) |
                 (ble_gatt_read_security(ch->permissions) <<
                  (PERM_POS_NP - PERM_POS_RP));
    if ((ch->properties & H2_PAL_BLE_GATT_PROPERTY_INDICATE) != 0u)
        props |= PERM(IND, ENABLE) |
                 (ble_gatt_read_security(ch->permissions) <<
                  (PERM_POS_IP - PERM_POS_RP));
    return props;
}

static void ble_hide_gatt_services(void)
{
    for (uint8_t i = 0u; i < s_gatt_service_count; ++i) {
        if (s_gatt_service_handles[i] != 0u)
            (void)attmdb_svc_visibility_set(
                s_gatt_service_handles[i], true);
    }
}

static h2_pal_result_t ble_register_gatt_services(
    void *user, const h2_pal_ble_gatt_service_t *services, size_t count)
{
    static const uint32_t valid_properties =
        H2_PAL_BLE_GATT_PROPERTY_READ |
        H2_PAL_BLE_GATT_PROPERTY_WRITE |
        H2_PAL_BLE_GATT_PROPERTY_WRITE_NO_RSP |
        H2_PAL_BLE_GATT_PROPERTY_NOTIFY |
        H2_PAL_BLE_GATT_PROPERTY_INDICATE;
    static const uint32_t valid_permissions =
        H2_PAL_BLE_GATT_PERMISSION_READ |
        H2_PAL_BLE_GATT_PERMISSION_WRITE |
        H2_PAL_BLE_GATT_PERMISSION_READ_ENCRYPTED |
        H2_PAL_BLE_GATT_PERMISSION_WRITE_ENCRYPTED |
        H2_PAL_BLE_GATT_PERMISSION_READ_AUTHENTICATED |
        H2_PAL_BLE_GATT_PERMISSION_WRITE_AUTHENTICATED;

    (void)user;
    if (h2_bk3633_platform_ble_host_status() != H2_PAL_OK)
        return H2_PAL_ERR_INVALID_STATE;
    if (count == 0u) return H2_PAL_OK;
    if (services == NULL || count > BK3633_BLE_MAX_GATT_SERVICES ||
        s_gatt_service_count != 0u)
        return H2_PAL_ERR_INVALID_ARG;
    ble_reset_gatt_registration();
    size_t total_chars = 0u;
    bool use_uuid128_db[BK3633_BLE_MAX_GATT_SERVICES] = {false};
    for (size_t service_index = 0u; service_index < count; ++service_index) {
        const h2_pal_ble_gatt_service_t *service = &services[service_index];
        if (service->uuid.data == NULL ||
            (service->uuid.len != 2u && service->uuid.len != 16u) ||
            service->characteristics == NULL ||
            service->characteristic_count == 0u) return H2_PAL_ERR_UNSUPPORTED;
        size_t attr_count = 1u;
        for (size_t i = 0u; i < service->characteristic_count; ++i) {
            const h2_pal_ble_gatt_characteristic_t *ch =
                &service->characteristics[i];
            if (ch->uuid.data == NULL ||
                (ch->uuid.len != 2u && ch->uuid.len != 16u) ||
                ch->max_value_len == 0u ||
                ch->max_value_len > H2_PAL_BLE_ATT_MAX_VALUE_LEN ||
                ch->initial_value_len > ch->max_value_len ||
                (ch->initial_value_len != 0u && ch->initial_value == NULL) ||
                (ch->properties & ~valid_properties) != 0u ||
                (ch->permissions & ~valid_permissions) != 0u)
                return H2_PAL_ERR_UNSUPPORTED;
            if (ch->uuid.len == 16u) use_uuid128_db[service_index] = true;
            attr_count += 2u;
            if ((ch->properties & (H2_PAL_BLE_GATT_PROPERTY_NOTIFY |
                                   H2_PAL_BLE_GATT_PROPERTY_INDICATE)) != 0u)
                ++attr_count;
        }
        if (attr_count > BK3633_BLE_MAX_GATT_ATTRS_PER_SERVICE ||
            total_chars + service->characteristic_count >
                BK3633_BLE_MAX_GATT_CHARS)
            return H2_PAL_ERR_NO_SPACE;
        s_gatt_attr_counts[service_index] = (uint8_t)attr_count;
        if (service->uuid.len == 16u) use_uuid128_db[service_index] = true;
        total_chars += service->characteristic_count;
    }

    for (size_t service_index = 0u; service_index < count; ++service_index) {
        const h2_pal_ble_gatt_service_t *service = &services[service_index];
        if (service->out_service_handle != NULL)
            *service->out_service_handle = H2_PAL_BLE_INVALID_ATTR_HANDLE;
        for (size_t i = 0u; i < service->characteristic_count; ++i) {
            const h2_pal_ble_gatt_characteristic_t *ch =
                &service->characteristics[i];
            if (ch->out_value_handle != NULL)
                *ch->out_value_handle = H2_PAL_BLE_INVALID_ATTR_HANDLE;
            if (ch->out_cccd_handle != NULL)
                *ch->out_cccd_handle = H2_PAL_BLE_INVALID_ATTR_HANDLE;
        }
    }

    s_gatt_service_count = (uint8_t)count;
    s_gatt_char_count = (uint8_t)total_chars;
    size_t global_char = 0u;
    for (size_t service_index = 0u; service_index < count; ++service_index) {
        const h2_pal_ble_gatt_service_t *service = &services[service_index];
        bool uuid128 = use_uuid128_db[service_index];
        memset(&s_gatt_scratch.service_db, 0,
               sizeof(s_gatt_scratch.service_db));
        s_gatt_cfg_flags[service_index] = 0xffffffffu;
        uint8_t attr = 1u;
        if (uuid128) {
            const uint8_t declaration[2] = {
                (uint8_t)(service->primary
                              ? ATT_DECL_PRIMARY_SERVICE
                              : ATT_DECL_SECONDARY_SERVICE),
                (uint8_t)((service->primary
                               ? ATT_DECL_PRIMARY_SERVICE
                               : ATT_DECL_SECONDARY_SERVICE) >> 8),
            };
            (void)ble_gatt_db128_set_uuid(
                s_gatt_scratch.service_db.db128[0].uuid,
                declaration, sizeof(declaration));
            s_gatt_scratch.service_db.db128[0].perm = PERM(RD, ENABLE);
            s_gatt_scratch.service_db.db128[0].ext_perm = 0u;
        } else {
            s_gatt_scratch.service_db.db16[0] = (struct attm_desc){
                .uuid = service->primary
                            ? ATT_DECL_PRIMARY_SERVICE
                            : ATT_DECL_SECONDARY_SERVICE,
                .perm = PERM(RD, ENABLE),
            };
        }
        size_t service_char_start = global_char;
        for (size_t i = 0u; i < service->characteristic_count; ++i) {
            const h2_pal_ble_gatt_characteristic_t *ch =
                &service->characteristics[i];
            uint16_t props = ble_gatt_properties(ch);
            uint8_t value_attr = (uint8_t)(attr + 1u);
            uint8_t cccd_attr = 0u;
            if (uuid128) {
                const uint8_t declaration[] = {
                    (uint8_t)ATT_DECL_CHARACTERISTIC,
                    (uint8_t)(ATT_DECL_CHARACTERISTIC >> 8),
                };
                (void)ble_gatt_db128_set_uuid(
                    s_gatt_scratch.service_db.db128[attr].uuid,
                    declaration, sizeof(declaration));
                s_gatt_scratch.service_db.db128[attr].perm =
                    PERM(RD, ENABLE);
                s_gatt_scratch.service_db.db128[attr].ext_perm = 0u;
                ++attr;
                uint16_t uuid_permission = ble_gatt_db128_set_uuid(
                    s_gatt_scratch.service_db.db128[attr].uuid,
                    ch->uuid.data, ch->uuid.len);
                s_gatt_scratch.service_db.db128[attr].perm = props;
                s_gatt_scratch.service_db.db128[attr].ext_perm =
                    uuid_permission |
                    ((ch->read != NULL || ch->write != NULL)
                         ? PERM(RI, ENABLE) : 0u);
                s_gatt_scratch.service_db.db128[attr].max_size =
                    (uint16_t)ch->max_value_len;
                ++attr;
            } else {
                s_gatt_scratch.service_db.db16[attr++] =
                    (struct attm_desc){
                    .uuid = ATT_DECL_CHARACTERISTIC,
                    .perm = PERM(RD, ENABLE),
                };
                s_gatt_scratch.service_db.db16[attr++] =
                    (struct attm_desc){
                    .uuid = ble_uuid16(&ch->uuid),
                    .perm = props,
                    .ext_perm = ch->read != NULL || ch->write != NULL
                        ? PERM(RI, ENABLE) : 0u,
                    .max_size = (uint16_t)ch->max_value_len,
                };
            }
            if ((ch->properties & (H2_PAL_BLE_GATT_PROPERTY_NOTIFY |
                                   H2_PAL_BLE_GATT_PROPERTY_INDICATE)) != 0u) {
                cccd_attr = attr;
                if (uuid128) {
                    const uint8_t cccd_uuid[] = {
                        (uint8_t)ATT_DESC_CLIENT_CHAR_CFG,
                        (uint8_t)(ATT_DESC_CLIENT_CHAR_CFG >> 8),
                    };
                    (void)ble_gatt_db128_set_uuid(
                        s_gatt_scratch.service_db.db128[attr].uuid,
                        cccd_uuid, sizeof(cccd_uuid));
                    s_gatt_scratch.service_db.db128[attr].perm =
                        PERM(RD, ENABLE) | PERM(WRITE_REQ, ENABLE) |
                        ble_gatt_read_security(ch->permissions) |
                        ble_gatt_write_security(ch->permissions);
                    s_gatt_scratch.service_db.db128[attr].ext_perm = 0u;
                } else {
                    s_gatt_scratch.service_db.db16[attr] =
                        (struct attm_desc){
                        .uuid = ATT_DESC_CLIENT_CHAR_CFG,
                        .perm = PERM(RD, ENABLE) | PERM(WRITE_REQ, ENABLE) |
                                ble_gatt_read_security(ch->permissions) |
                                ble_gatt_write_security(ch->permissions),
                    };
                }
                ++attr;
            }
            s_gatt_chars[global_char++] = (h2_bk3633_gatt_char_t){
                .value_handle = value_attr,
                .cccd_handle = cccd_attr,
                .properties = ch->properties,
                .permissions = ch->permissions,
                .max_value_len = ch->max_value_len,
                .read = ch->read,
                .write = ch->write,
                .user = ch->user,
            };
        }

        uint16_t shdl = 0u;
        uint8_t service_uuid128[16];
        const uint8_t *sdk_service_uuid = service->uuid.data;
        if (uuid128 && service->uuid.len == 2u) {
            /* The SDK's 128-bit descriptor table is required when any
             * characteristic is 128-bit. Preserve the 16-bit service identity
             * by expanding it into the Bluetooth Base UUID. */
            attm_convert_to128(
                service_uuid128, service->uuid.data,
                (uint8_t)service->uuid.len);
            sdk_service_uuid = service_uuid128;
        }
        /* Create the service directly in its requested visible state. The
         * SDK visibility helper cannot provide transactional schema setup. */
        uint8_t service_permission = 0u;
        if (uuid128)
            service_permission |= PERM(SVC_UUID_LEN, UUID_128);
        if (!service->primary)
            service_permission |= PERM(SVC_SECONDARY, ENABLE);
        uint8_t status = uuid128
            ? attm_svc_create_db_128(
                  &shdl, sdk_service_uuid,
                   (uint8_t *)&s_gatt_cfg_flags[service_index],
                   s_gatt_attr_counts[service_index], NULL, TASK_APP,
                   s_gatt_scratch.service_db.db128, service_permission)
            : attm_svc_create_db(
                  &shdl, ble_uuid16(&service->uuid),
                   (uint8_t *)&s_gatt_cfg_flags[service_index],
                   s_gatt_attr_counts[service_index], NULL, TASK_APP,
                   s_gatt_scratch.service_db.db16, service_permission);
        if (status != ATT_ERR_NO_ERROR) {
            ble_hide_gatt_services();
            ble_reset_gatt_registration();
            s_ble_state.host_status = H2_PAL_ERR_IO;
            return H2_PAL_ERR_IO;
        }
        s_gatt_service_handles[service_index] = shdl;
        if (s_gatt_start_handle == 0u) s_gatt_start_handle = shdl;
        attr = 1u;
        for (size_t i = 0u; i < service->characteristic_count; ++i) {
            h2_bk3633_gatt_char_t *mapped =
                &s_gatt_chars[service_char_start + i];
            mapped->value_handle = (uint16_t)(shdl + attr + 1u);
            if (mapped->cccd_handle != 0u)
                mapped->cccd_handle =
                    (uint16_t)(shdl + mapped->cccd_handle);
            const h2_pal_ble_gatt_characteristic_t *ch =
                &service->characteristics[i];
            if (ch->read == NULL && ch->initial_value_len != 0u &&
                attm_att_set_value(
                    mapped->value_handle, (att_size_t)ch->initial_value_len,
                    0u, (uint8_t *)ch->initial_value) != ATT_ERR_NO_ERROR) {
                ble_hide_gatt_services();
                ble_reset_gatt_registration();
                s_ble_state.host_status = H2_PAL_ERR_IO;
                return H2_PAL_ERR_IO;
            }
            attr = (uint8_t)(attr + 2u +
                (mapped->cccd_handle != 0u ? 1u : 0u));
        }
    }

    global_char = 0u;
    for (size_t service_index = 0u; service_index < count; ++service_index) {
        const h2_pal_ble_gatt_service_t *service = &services[service_index];
        if (service->out_service_handle != NULL)
            *service->out_service_handle =
                s_gatt_service_handles[service_index];
        for (size_t i = 0u; i < service->characteristic_count; ++i) {
            const h2_pal_ble_gatt_characteristic_t *ch =
                &service->characteristics[i];
            if (ch->out_value_handle != NULL)
                *ch->out_value_handle = s_gatt_chars[global_char].value_handle;
            if (ch->out_cccd_handle != NULL)
                *ch->out_cccd_handle = s_gatt_chars[global_char].cccd_handle;
            ++global_char;
        }
    }
    return H2_PAL_OK;
}

static h2_pal_result_t ble_unregister_gatt_services(void *user)
{
    (void)user;
    ble_reject_pending_accesses(H2_PAL_ERR_CLOSED);
    if (s_gatt_service_count == 0u)
        return H2_PAL_OK;
    h2_pal_result_t result = H2_PAL_OK;
    for (uint8_t i = 0u; i < s_gatt_service_count; ++i) {
        if (attmdb_svc_visibility_set(
                s_gatt_service_handles[i], true) != ATT_ERR_NO_ERROR) {
            result = H2_PAL_ERR_IO;
        }
    }
    ble_reset_gatt_registration();
    return result;
}

static h2_pal_result_t ble_notify(
    void *user, uint16_t conn_handle, uint16_t attr_handle,
    const uint8_t *data, size_t len)
{
    (void)user;
    if (h2_bk3633_platform_ble_host_status() != H2_PAL_OK)
        return H2_PAL_ERR_INVALID_STATE;
    if (!s_ble_state.connected ||
        conn_handle != s_ble_state.connection.conn_handle ||
        ble_find_gatt_char(attr_handle) == NULL ||
        (len != 0u && data == NULL))
        return H2_PAL_ERR_INVALID_ARG;
    h2_bk3633_gatt_char_t *characteristic = ble_find_gatt_char(attr_handle);
    if (characteristic->value_handle != attr_handle ||
        (characteristic->properties & H2_PAL_BLE_GATT_PROPERTY_NOTIFY) == 0u ||
        len > characteristic->max_value_len ||
        len > (size_t)s_ble_state.connection.mtu - H2_PAL_BLE_ATT_HEADER_LEN)
        return H2_PAL_ERR_INVALID_ARG;
    if (!ble_gatt_subscription_enabled(characteristic, 0x0001u))
        return H2_PAL_ERR_INVALID_STATE;
    struct gattc_send_evt_cmd *cmd = KE_MSG_ALLOC_DYN(
        GATTC_SEND_EVT_CMD,
        KE_BUILD_ID(TASK_GATTC, s_ble_state.connection_index),
        KE_BUILD_ID(TASK_APP, s_ble_state.connection_index),
        gattc_send_evt_cmd, len);
    if (cmd == NULL) return H2_PAL_ERR_NO_MEMORY;
    cmd->operation = GATTC_NOTIFY;
    cmd->seq_num = 0u;
    cmd->handle = attr_handle;
    cmd->length = (uint16_t)len;
    if (len != 0u) memcpy(cmd->value, data, len);
    ke_msg_send(cmd);
    return H2_PAL_OK;
}

static h2_pal_result_t ble_indicate_common(
    void *user, uint16_t conn_handle, uint16_t attr_handle,
    const uint8_t *data, size_t len, uint32_t timeout_ms)
{
    uint64_t started_ms;
    (void)user;
    if (h2_bk3633_platform_ble_host_status() != H2_PAL_OK)
        return H2_PAL_ERR_INVALID_STATE;
    if (!s_ble_state.connected ||
        conn_handle != s_ble_state.connection.conn_handle ||
        ble_find_gatt_char(attr_handle) == NULL ||
        (len != 0u && data == NULL))
        return H2_PAL_ERR_INVALID_ARG;
    h2_bk3633_gatt_char_t *characteristic = ble_find_gatt_char(attr_handle);
    if (characteristic->value_handle != attr_handle ||
        (characteristic->properties & H2_PAL_BLE_GATT_PROPERTY_INDICATE) == 0u ||
        len > characteristic->max_value_len ||
        len > (size_t)s_ble_state.connection.mtu - H2_PAL_BLE_ATT_HEADER_LEN)
        return H2_PAL_ERR_INVALID_ARG;
    if (!ble_gatt_subscription_enabled(characteristic, 0x0002u))
        return H2_PAL_ERR_INVALID_STATE;
    if (timeout_ms == 0u)
        return H2_PAL_ERR_WOULD_BLOCK;
    if (s_ble_state.indication_pending ||
        s_ble_state.indication_abandoned)
        return H2_PAL_ERR_BUSY;
    if (timeout_ms != UINT32_MAX) {
        h2_pal_result_t time_result = h2_pal_time_get_monotonic_ms(
            s_ble_time, &started_ms);
        if (time_result != H2_PAL_OK)
            return time_result;
    }
    struct gattc_send_evt_cmd *cmd = KE_MSG_ALLOC_DYN(
        GATTC_SEND_EVT_CMD,
        KE_BUILD_ID(TASK_GATTC, s_ble_state.connection_index),
        KE_BUILD_ID(TASK_APP, s_ble_state.connection_index),
        gattc_send_evt_cmd, len);
    if (cmd == NULL) return H2_PAL_ERR_NO_MEMORY;
    cmd->operation = GATTC_INDICATE;
    ++s_ble_state.indication_seq;
    if (s_ble_state.indication_seq == 0u)
        ++s_ble_state.indication_seq;
    cmd->seq_num = s_ble_state.indication_seq;
    cmd->handle = attr_handle;
    cmd->length = (uint16_t)len;
    if (len != 0u) memcpy(cmd->value, data, len);
    s_ble_state.indication_pending = true;
    s_ble_state.indication_waiting = true;
    s_ble_state.indication_result = H2_PAL_ERR_WOULD_BLOCK;
    ke_msg_send(cmd);

    while (s_ble_state.indication_pending) {
        h2_pal_result_t wait_result = ble_wait_until(
            (uintptr_t)&s_ble_indication_wait_key, started_ms, timeout_ms);
        if (wait_result != H2_PAL_OK) {
            s_ble_state.indication_waiting = false;
            if (s_ble_state.indication_pending) {
                s_ble_state.indication_abandoned = true;
            }
            return wait_result;
        }
    }
    s_ble_state.indication_waiting = false;
    return s_ble_state.indication_result;
}

static h2_pal_result_t ble_indicate(
    void *user, uint16_t conn_handle, uint16_t attr_handle,
    const uint8_t *data, size_t len, uint32_t timeout_ms)
{
    return ble_indicate_common(
        user, conn_handle, attr_handle, data, len, timeout_ms);
}

static h2_pal_result_t ble_connect(
    void *user, const h2_pal_ble_addr_t *addr,
    const h2_pal_ble_connect_params_t *params, uint16_t *out_conn_handle)
{
    (void)user;
    (void)addr;
    (void)params;
    if (out_conn_handle != NULL) {
        *out_conn_handle = H2_PAL_BLE_INVALID_CONN_HANDLE;
    }
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t ble_disconnect(void *user, uint16_t conn_handle)
{
    (void)user;
    if (h2_bk3633_platform_ble_host_status() != H2_PAL_OK) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    if (!s_ble_state.connected ||
        conn_handle != s_ble_state.connection.conn_handle) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (s_ble_state.disconnect_pending) {
        return H2_PAL_ERR_INVALID_STATE;
    }

    struct gapc_disconnect_cmd *cmd = KE_MSG_ALLOC(
        GAPC_DISCONNECT_CMD,
        KE_BUILD_ID(TASK_GAPC, s_ble_state.connection_index),
        KE_BUILD_ID(TASK_APP, s_ble_state.connection_index),
        gapc_disconnect_cmd);
    if (cmd == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    cmd->operation = GAPC_DISCONNECT;
    cmd->reason = CO_ERROR_REMOTE_USER_TERM_CON;
    s_ble_state.disconnect_pending = true;
    ke_msg_send(cmd);
    return H2_PAL_OK;
}

static h2_pal_result_t ble_configure_pairing(
    void *user, const h2_pal_ble_pairing_config_t *config)
{
    (void)user;
    (void)config;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t ble_pair(
    void *user, uint16_t conn_handle, uint32_t timeout_ms)
{
    (void)user;
    (void)conn_handle;
    (void)timeout_ms;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t ble_update_connection(
    void *user, uint16_t conn_handle,
    const h2_pal_ble_connection_params_t *params)
{
    (void)user;
    (void)conn_handle;
    (void)params;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t ble_exchange_mtu(
    void *user, uint16_t conn_handle, uint16_t *out_mtu, uint32_t timeout_ms)
{
    (void)user;
    (void)conn_handle;
    (void)timeout_ms;
    if (out_mtu != NULL) {
        *out_mtu = 0u;
    }
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t ble_set_preferred_phy(
    void *user, uint16_t conn_handle, h2_pal_ble_phy_t tx_phy,
    h2_pal_ble_phy_t rx_phy, uint32_t timeout_ms)
{
    (void)user;
    (void)conn_handle;
    (void)tx_phy;
    (void)rx_phy;
    (void)timeout_ms;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t ble_read_phy(
    void *user, uint16_t conn_handle, h2_pal_ble_phy_info_t *out_phy,
    uint32_t timeout_ms)
{
    (void)user;
    (void)conn_handle;
    (void)timeout_ms;
    if (out_phy != NULL) {
        *out_phy = (h2_pal_ble_phy_info_t){
            .conn_handle = H2_PAL_BLE_INVALID_CONN_HANDLE,
            .tx_phy = H2_PAL_BLE_PHY_UNKNOWN,
            .rx_phy = H2_PAL_BLE_PHY_UNKNOWN,
        };
    }
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t ble_gatt_discover(
    void *user, uint16_t conn_handle,
    const h2_pal_ble_gatt_discovery_request_t *request,
    h2_pal_ble_gatt_discovery_entry_t *entries, size_t max_entries,
    size_t *out_count, uint32_t timeout_ms)
{
    (void)user;
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

static h2_pal_result_t ble_gatt_read(
    void *user, uint16_t conn_handle, uint16_t attr_handle, uint16_t offset,
    uint8_t *out, size_t out_size, size_t *out_len, uint32_t timeout_ms)
{
    (void)user;
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

static h2_pal_result_t ble_gatt_write(
    void *user, uint16_t conn_handle, uint16_t attr_handle,
    const uint8_t *data, size_t len, bool with_response, uint32_t timeout_ms)
{
    (void)user;
    (void)conn_handle;
    (void)attr_handle;
    (void)data;
    (void)len;
    (void)with_response;
    (void)timeout_ms;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t ble_gatt_subscribe(
    void *user, uint16_t conn_handle,
    const h2_pal_ble_gatt_subscribe_t *subscribe, uint32_t timeout_ms)
{
    (void)user;
    (void)conn_handle;
    (void)subscribe;
    (void)timeout_ms;
    return H2_PAL_ERR_UNSUPPORTED;
}

h2_pal_result_t h2_bk3633_platform_ble_configure(
    const h2_bk3633_platform_ble_config_t *config)
{
    if (config == NULL || config->mem == NULL || config->time == NULL ||
        config->gatt_pending_access_capacity == 0u ||
        config->gatt_pending_access_capacity >
            SIZE_MAX / sizeof(*s_gatt_pending_access) ||
        config->bootstrap_timeout_ms == 0u) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (s_gatt_pending_access != NULL) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    size_t storage_size = config->gatt_pending_access_capacity *
                          sizeof(*s_gatt_pending_access);
    s_gatt_pending_access = h2_pal_mem_alloc(config->mem, storage_size);
    if (s_gatt_pending_access == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    memset(s_gatt_pending_access, 0, storage_size);
    s_gatt_pending_mem = config->mem;
    s_gatt_pending_capacity = config->gatt_pending_access_capacity;
    s_gatt_pending_head = 0u;
    s_gatt_pending_count = 0u;
    s_ble_time = config->time;
    s_ble_bootstrap_timeout_ms = config->bootstrap_timeout_ms;
    return H2_PAL_OK;
}

const h2_pal_ble_host_api_t *h2_bk3633_platform_ble_api(void)
{
    static const h2_pal_ble_vtable_t vtable = {
        .start = ble_start,
        .stop = ble_stop,
        .set_adv_data = ble_set_adv_data,
        .start_advertising = ble_start_advertising,
        .stop_advertising = ble_stop_advertising,
        .adv_set_create = ble_adv_set_create,
        .adv_set_set_data = ble_adv_set_set_data,
        .adv_set_set_scan_response_data =
            ble_adv_set_set_scan_response_data,
        .adv_set_start = ble_adv_set_start,
        .adv_set_stop = ble_adv_set_stop,
        .adv_set_destroy = ble_adv_set_destroy,
        .start_scan = ble_start_scan,
        .stop_scan = ble_stop_scan,
        .register_gatt_services = ble_register_gatt_services,
        .unregister_gatt_services = ble_unregister_gatt_services,
        .notify = ble_notify,
        .indicate = ble_indicate,
        .connect = ble_connect,
        .configure_pairing = ble_configure_pairing,
        .pair = ble_pair,
        .disconnect = ble_disconnect,
        .update_connection = ble_update_connection,
        .exchange_mtu = ble_exchange_mtu,
        .set_preferred_phy = ble_set_preferred_phy,
        .read_phy = ble_read_phy,
        .gatt_discover = ble_gatt_discover,
        .gatt_read = ble_gatt_read,
        .gatt_write = ble_gatt_write,
        .gatt_subscribe = ble_gatt_subscribe,
    };
    static h2_pal_ble_host_api_t api = {
        .user = NULL,
        .vtable = &vtable,
    };
    if (api.allocator == NULL) {
        api.allocator = h2_bk3633_platform_mem_api();
    }
    return &api;
}

int h2_bk3633_platform_ble_dispatch(
    uint16_t msgid,
    const void *param,
    uint16_t dest_id,
    uint16_t src_id)
{
    (void)dest_id;
    (void)src_id;

    if (msgid == GAPC_CONNECTION_REQ_IND) {
        const struct gapc_connection_req_ind *ind = param;
        if (ind == NULL) return KE_MSG_CONSUMED;
        struct gapc_connection_cfm *cfm = KE_MSG_ALLOC(
            GAPC_CONNECTION_CFM, src_id, dest_id, gapc_connection_cfm);
        if (cfm == NULL) {
            return KE_MSG_CONSUMED;
        }
        memset(cfm, 0, sizeof(*cfm));
        cfm->auth = GAP_AUTH_REQ_NO_MITM_NO_BOND;
        ke_msg_send(cfm);

        memset(&s_ble_state.connection, 0, sizeof(s_ble_state.connection));
        s_ble_state.connection.conn_handle = ind->conhdl;
        s_ble_state.connection.role = ind->role == 0u
            ? H2_PAL_BLE_ROLE_CENTRAL : H2_PAL_BLE_ROLE_PERIPHERAL;
        s_ble_state.connection.peer_addr.type = ind->peer_addr_type != 0u
            ? H2_PAL_BLE_ADDR_TYPE_RANDOM : H2_PAL_BLE_ADDR_TYPE_PUBLIC;
        memcpy(s_ble_state.connection.peer_addr.value, ind->peer_addr.addr,
               H2_PAL_BLE_ADDR_LEN);
        s_ble_state.connection.mtu = 23u;
        s_ble_state.connection_index = KE_IDX_GET(src_id);
        s_ble_state.disconnect_pending = false;
        s_ble_state.indication_pending = false;
        memset(s_gatt_cccd_values, 0, sizeof(s_gatt_cccd_values));
        s_ble_state.connected = true;
        if (s_ble_state.connection.role == H2_PAL_BLE_ROLE_PERIPHERAL) {
            for (size_t i = 0u; i < BK3633_BLE_ADV_SET_MAX; ++i) {
                h2_pal_ble_adv_set_t *set = &s_ble_state.adv_sets[i];
                if (set->used &&
                    set->params.mode == H2_PAL_BLE_ADV_MODE_CONNECTABLE) {
                    /* The controller consumes connectable advertising when it
                     * accepts a peripheral connection. GAPM does not
                     * consistently emit ACTIVITY_STOPPED for this implicit
                     * transition, so keep the provider handle restartable. */
                    set->started = false;
                }
            }
        }
        ble_post_event(H2_PAL_SYSTEM_EVENT_TYPE_BLE_CONNECTED,
                       &s_ble_state.connection, sizeof(s_ble_state.connection));
        return KE_MSG_CONSUMED;
    }
    if (msgid == GAPC_DISCONNECT_IND) {
        const struct gapc_disconnect_ind *ind = param;
        h2_pal_ble_disconnected_info_t info;
        if (ind == NULL) return KE_MSG_CONSUMED;
        memset(&info, 0, sizeof(info));
        info.conn_handle = ind->conhdl;
        info.peer_addr = s_ble_state.connection.peer_addr;
        info.reason = ind->reason;
        ble_complete_indication(H2_PAL_ERR_CLOSED);
        ble_reject_pending_accesses(H2_PAL_ERR_CLOSED);
        s_ble_state.connected = false;
        s_ble_state.disconnect_pending = false;
        memset(s_gatt_cccd_values, 0, sizeof(s_gatt_cccd_values));
        ble_post_event(H2_PAL_SYSTEM_EVENT_TYPE_BLE_DISCONNECTED,
                       &info, sizeof(info));
        return KE_MSG_CONSUMED;
    }
    if (msgid == GAPC_PARAM_UPDATED_IND) {
        const struct gapc_param_updated_ind *ind = param;
        h2_pal_ble_connection_params_t params;
        if (ind == NULL) return KE_MSG_CONSUMED;
        params.interval_min_ms = ble_conn_interval_to_ms(ind->con_interval);
        params.interval_max_ms = params.interval_min_ms;
        params.latency = ind->con_latency;
        params.supervision_timeout_ms = (uint16_t)ind->sup_to * 10u;
        ble_post_event(H2_PAL_SYSTEM_EVENT_TYPE_BLE_CONNECTION_UPDATED,
                       &params, sizeof(params));
        return KE_MSG_CONSUMED;
    }
    if (msgid == GAPC_PARAM_UPDATE_REQ_IND) {
        const struct gapc_param_update_req_ind *ind = param;
        if (ind == NULL) return KE_MSG_CONSUMED;
        struct gapc_param_update_cfm *cfm = KE_MSG_ALLOC(
            GAPC_PARAM_UPDATE_CFM, src_id, dest_id,
            gapc_param_update_cfm);
        if (cfm != NULL) {
            cfm->accept = true;
            cfm->ce_len_min = 2u;
            cfm->ce_len_max = 4u;
            ke_msg_send(cfm);
        }
        return KE_MSG_CONSUMED;
    }
    if (msgid == GAPC_GET_DEV_INFO_REQ_IND) {
        const struct gapc_get_dev_info_req_ind *ind = param;
        if (ind == NULL) return KE_MSG_CONSUMED;
        size_t extra_length = ind->req == GAPC_DEV_NAME
            ? s_ble_state.device_name_len
            : 0u;
        struct gapc_get_dev_info_cfm *cfm = KE_MSG_ALLOC_DYN(
            GAPC_GET_DEV_INFO_CFM, src_id, dest_id,
            gapc_get_dev_info_cfm, extra_length);
        if (cfm == NULL) return KE_MSG_CONSUMED;
        memset(cfm, 0, sizeof(*cfm));
        cfm->req = ind->req;
        if (ind->req == GAPC_DEV_NAME) {
            cfm->info.name.length = s_ble_state.device_name_len;
            if (s_ble_state.device_name_len != 0u) {
                memcpy(
                    cfm->info.name.value,
                    &s_ble_state.adv_data[s_ble_state.device_name_offset],
                    s_ble_state.device_name_len);
            }
        } else if (ind->req == GAPC_DEV_SLV_PREF_PARAMS) {
            cfm->info.slv_pref_params.con_intv_min = 60u;
            cfm->info.slv_pref_params.con_intv_max = 100u;
            cfm->info.slv_pref_params.slave_latency = 0u;
            cfm->info.slv_pref_params.conn_timeout = 600u;
        }
        ke_msg_send(cfm);
        return KE_MSG_CONSUMED;
    }
    if (msgid == GAPC_SET_DEV_INFO_REQ_IND) {
        const struct gapc_set_dev_info_req_ind *ind = param;
        if (ind == NULL) return KE_MSG_CONSUMED;
        struct gapc_set_dev_info_cfm *cfm = KE_MSG_ALLOC(
            GAPC_SET_DEV_INFO_CFM, src_id, dest_id,
            gapc_set_dev_info_cfm);
        if (cfm != NULL) {
            cfm->req = ind->req;
            cfm->status = GAP_ERR_REJECTED;
            ke_msg_send(cfm);
        }
        return KE_MSG_CONSUMED;
    }
    if (msgid == GATTC_MTU_CHANGED_IND) {
        const struct gattc_mtu_changed_ind *ind = param;
        h2_pal_ble_mtu_info_t mtu;
        if (ind == NULL) return KE_MSG_CONSUMED;
        mtu.conn_handle = s_ble_state.connection.conn_handle;
        mtu.mtu = ind->mtu;
        s_ble_state.connection.mtu = ind->mtu;
        ble_post_event(H2_PAL_SYSTEM_EVENT_TYPE_BLE_MTU_CHANGED,
                       &mtu, sizeof(mtu));
        return KE_MSG_CONSUMED;
    }
    if (msgid == GATTC_CMP_EVT) {
        const struct gattc_cmp_evt *evt = param;
        if (evt != NULL && evt->operation == GATTC_INDICATE &&
            s_ble_state.indication_pending &&
            evt->seq_num == s_ble_state.indication_seq) {
            ble_complete_indication(
                evt->status == ATT_ERR_NO_ERROR
                    ? H2_PAL_OK : H2_PAL_ERR_IO);
        }
        return KE_MSG_CONSUMED;
    }
    if (msgid == GATTC_READ_REQ_IND)
        return ble_gatt_read_req_handler(msgid, param, dest_id, src_id);
    if (msgid == GATTC_WRITE_REQ_IND)
        return ble_gatt_write_req_handler(msgid, param, dest_id, src_id);
    if (msgid == GATTC_ATT_INFO_REQ_IND)
        return ble_gatt_att_info_req_handler(
            msgid, param, dest_id, src_id);

    if (msgid == GAPM_ACTIVITY_CREATED_IND) {
        const struct gapm_activity_created_ind *ind = param;
        if (ind == NULL) return KE_MSG_CONSUMED;
        if (ind->actv_type == GAPM_ACTV_TYPE_SCAN) {
            s_ble_state.scan_actv_idx = ind->actv_idx;
            s_ble_state.scan_created = true;
            s_ble_state.scan_create_pending = false;
            if (s_ble_state.scan_callback != NULL &&
                ble_send_start_scan(&s_ble_state.scan_params) != H2_PAL_OK) {
                s_ble_state.scan_callback = NULL;
                s_ble_state.scan_user = NULL;
            }
        } else if (ind->actv_type == GAPM_ACTV_TYPE_ADV) {
            uint8_t slot = ble_adv_queue_pop(
                s_ble_state.pending_adv_create,
                &s_ble_state.pending_adv_create_head,
                &s_ble_state.pending_adv_create_count);
            if (slot < BK3633_BLE_ADV_SET_MAX &&
                s_ble_state.adv_sets[slot].used) {
                h2_pal_ble_adv_set_t *set = &s_ble_state.adv_sets[slot];
                set->activity_index = ind->actv_idx;
                set->created = true;
                set->create_pending = false;
                if (set->destroy_requested) {
                    (void)ble_send_delete_adv_set(set, slot);
                } else if (set->data_len != 0u)
                    (void)ble_send_adv_set_data(set, slot);
                else if (set->scan_response_set)
                    (void)ble_send_adv_set_scan_response_data(set, slot);
                else if (set->start_requested)
                    (void)ble_send_start_adv_set(set, slot);
            } else {
                s_ble_state.adv_actv_idx = ind->actv_idx;
                s_ble_state.adv_created = true;
                s_ble_state.adv_create_pending = false;
                if (s_ble_state.adv_data_len != 0u) {
                    s_ble_state.adv_start_after_data = true;
                    if (ble_send_adv_data() != H2_PAL_OK)
                        s_ble_state.adv_start_after_data = false;
                } else {
                    (void)ble_send_start_adv();
                }
            }
        }
    } else if (msgid == GAPM_ACTIVITY_STOPPED_IND) {
        const struct gapm_activity_stopped_ind *ind = param;
        if (ind != NULL && ind->actv_type == GAPM_ACTV_TYPE_SCAN) {
            s_ble_state.scan_started = false;
            s_ble_state.scan_stop_pending = false;
            s_ble_state.scan_callback = NULL;
            s_ble_state.scan_user = NULL;
            ble_post_event(H2_PAL_SYSTEM_EVENT_TYPE_BLE_SCAN_STOPPED,
                           NULL, 0u);
        } else if (ind != NULL && ind->actv_type == GAPM_ACTV_TYPE_ADV) {
            h2_pal_ble_adv_set_t *set =
                ble_find_adv_set_by_activity(ind->actv_idx);
            if (set != NULL) {
                set->started = false;
                set->stop_pending = false;
                ble_post_adv_set_event(
                    H2_PAL_SYSTEM_EVENT_TYPE_BLE_ADVERTISING_STOPPED,
                    set, H2_PAL_OK);
                if (set->destroy_requested)
                    (void)ble_send_delete_adv_set(
                        set, (uint8_t)ble_adv_set_index(set));
            } else {
                s_ble_state.adv_started = false;
                s_ble_state.adv_stop_pending = false;
                ble_post_event(
                    H2_PAL_SYSTEM_EVENT_TYPE_BLE_ADVERTISING_STOPPED,
                    NULL, 0u);
            }
        }
    } else if (msgid == GAPM_CMP_EVT) {
        const struct gapm_cmp_evt *evt = param;
        if (evt != NULL) {
            if (evt->operation == GAPM_RESET &&
                s_ble_state.host_stop_pending) {
                s_ble_state.host_stop_pending = false;
                if (evt->status != GAP_ERR_NO_ERROR) {
                    s_ble_state.host_status = H2_PAL_ERR_IO;
                }
                s_ble_state.host_stopped_event_pending = true;
            } else if (evt->operation == GAPM_START_ACTIVITY) {
                uint8_t kind = ble_start_queue_pop();
                if (kind == BK3633_BLE_ACTIVITY_KIND_SCAN) {
                    s_ble_state.scan_start_pending = false;
                    if (evt->status == GAP_ERR_NO_ERROR) {
                        s_ble_state.scan_started = true;
                        ble_post_event(
                            H2_PAL_SYSTEM_EVENT_TYPE_BLE_SCAN_STARTED,
                            NULL, 0u);
                    } else {
                        s_ble_state.scan_callback = NULL;
                        s_ble_state.scan_user = NULL;
                    }
                } else if (kind == BK3633_BLE_ACTIVITY_KIND_ADV) {
                    s_ble_state.adv_start_pending = false;
                    if (evt->status == GAP_ERR_NO_ERROR) {
                        s_ble_state.adv_started = true;
                        ble_post_event(
                            H2_PAL_SYSTEM_EVENT_TYPE_BLE_ADVERTISING_STARTED,
                            NULL, 0u);
                    }
                } else {
                    h2_pal_ble_adv_set_t *set =
                        ble_adv_set_from_kind(kind);
                    if (set != NULL) {
                        set->start_pending = false;
                        if (evt->status == GAP_ERR_NO_ERROR) {
                            set->started = true;
                        } else {
                            set->start_requested = false;
                        }
                        ble_post_adv_set_event(
                            H2_PAL_SYSTEM_EVENT_TYPE_BLE_ADVERTISING_STARTED,
                            set, evt->status == GAP_ERR_NO_ERROR
                                     ? H2_PAL_OK : H2_PAL_ERR_IO);
                        if (set->started && set->destroy_requested)
                            (void)ble_adv_set_stop(NULL, set);
                        else if (set->destroy_requested)
                            (void)ble_send_delete_adv_set(
                                set,
                                (uint8_t)ble_adv_set_index(set));
                    }
                }
            } else if (evt->operation == GAPM_SET_ADV_DATA) {
                uint8_t slot = ble_adv_queue_pop(
                    s_ble_state.pending_adv_data,
                    &s_ble_state.pending_adv_data_head,
                    &s_ble_state.pending_adv_data_count);
                if (slot < BK3633_BLE_ADV_SET_MAX &&
                    s_ble_state.adv_sets[slot].used) {
                    h2_pal_ble_adv_set_t *set =
                        &s_ble_state.adv_sets[slot];
                    set->data_pending = false;
                    if (set->destroy_requested) {
                        (void)ble_send_delete_adv_set(set, slot);
                    } else if (evt->status == GAP_ERR_NO_ERROR &&
                               set->scan_response_set &&
                               !set->scan_response_configured) {
                        (void)ble_send_adv_set_scan_response_data(set, slot);
                    } else if (evt->status == GAP_ERR_NO_ERROR &&
                               set->start_requested && !set->started) {
                        (void)ble_send_start_adv_set(set, slot);
                    } else if (evt->status != GAP_ERR_NO_ERROR &&
                               set->start_requested && !set->started) {
                        set->start_requested = false;
                        ble_post_adv_set_event(
                            H2_PAL_SYSTEM_EVENT_TYPE_BLE_ADVERTISING_STARTED,
                            set, H2_PAL_ERR_IO);
                    }
                } else {
                    s_ble_state.adv_data_pending = false;
                    if (evt->status == GAP_ERR_NO_ERROR &&
                        s_ble_state.adv_start_after_data) {
                        s_ble_state.adv_start_after_data = false;
                        (void)ble_send_start_adv();
                    } else if (evt->status != GAP_ERR_NO_ERROR) {
                        s_ble_state.adv_start_after_data = false;
                    }
                }
            } else if (evt->operation == GAPM_SET_SCAN_RSP_DATA) {
                uint8_t slot = ble_adv_queue_pop(
                    s_ble_state.pending_adv_data,
                    &s_ble_state.pending_adv_data_head,
                    &s_ble_state.pending_adv_data_count);
                if (slot < BK3633_BLE_ADV_SET_MAX &&
                    s_ble_state.adv_sets[slot].used) {
                    h2_pal_ble_adv_set_t *set =
                        &s_ble_state.adv_sets[slot];
                    set->scan_response_pending = false;
                    set->scan_response_configured =
                        evt->status == GAP_ERR_NO_ERROR;
                    if (set->destroy_requested) {
                        (void)ble_send_delete_adv_set(set, slot);
                    } else if (evt->status == GAP_ERR_NO_ERROR &&
                               set->start_requested && !set->started) {
                        (void)ble_send_start_adv_set(set, slot);
                    } else if (evt->status != GAP_ERR_NO_ERROR &&
                               set->start_requested && !set->started) {
                        set->start_requested = false;
                        ble_post_adv_set_event(
                            H2_PAL_SYSTEM_EVENT_TYPE_BLE_ADVERTISING_STARTED,
                            set, H2_PAL_ERR_IO);
                    }
                }
            } else if (evt->operation == GAPM_CREATE_SCAN_ACTIVITY &&
                       evt->status != GAP_ERR_NO_ERROR) {
                s_ble_state.scan_create_pending = false;
                s_ble_state.scan_callback = NULL;
                s_ble_state.scan_user = NULL;
            } else if (evt->operation == GAPM_CREATE_ADV_ACTIVITY &&
                       evt->status != GAP_ERR_NO_ERROR) {
                uint8_t slot = ble_adv_queue_pop(
                    s_ble_state.pending_adv_create,
                    &s_ble_state.pending_adv_create_head,
                    &s_ble_state.pending_adv_create_count);
                if (slot < BK3633_BLE_ADV_SET_MAX) {
                    s_ble_state.adv_sets[slot].create_pending = false;
                    s_ble_state.adv_sets[slot].start_requested = false;
                    ble_post_adv_set_event(
                        H2_PAL_SYSTEM_EVENT_TYPE_BLE_ADVERTISING_STARTED,
                        &s_ble_state.adv_sets[slot], H2_PAL_ERR_IO);
                } else {
                    s_ble_state.adv_create_pending = false;
                }
            } else if (evt->operation == GAPM_STOP_ACTIVITY) {
                uint8_t kind = ble_stop_queue_pop();
                if (evt->status != GAP_ERR_NO_ERROR) {
                    if (kind == BK3633_BLE_ACTIVITY_KIND_SCAN)
                        s_ble_state.scan_stop_pending = false;
                    if (kind == BK3633_BLE_ACTIVITY_KIND_ADV)
                        s_ble_state.adv_stop_pending = false;
                    h2_pal_ble_adv_set_t *set =
                        ble_adv_set_from_kind(kind);
                    if (set != NULL) {
                        set->stop_pending = false;
                        ble_post_adv_set_event(
                            H2_PAL_SYSTEM_EVENT_TYPE_BLE_ADVERTISING_STOPPED,
                            set, H2_PAL_ERR_IO);
                    }
                }
            } else if (evt->operation == GAPM_DELETE_ACTIVITY) {
                uint8_t slot = ble_adv_queue_pop(
                    s_ble_state.pending_adv_delete,
                    &s_ble_state.pending_adv_delete_head,
                    &s_ble_state.pending_adv_delete_count);
                if (slot < BK3633_BLE_ADV_SET_MAX) {
                    h2_pal_ble_adv_set_t *set =
                        &s_ble_state.adv_sets[slot];
                    set->delete_pending = false;
                    if (evt->status == GAP_ERR_NO_ERROR)
                        memset(set, 0, sizeof(*set));
                }
            }
        }
    } else if (msgid == GAPM_EXT_ADV_REPORT_IND) {
        ble_enqueue_scan_report(
            (const struct gapm_ext_adv_report_ind *)param);
    }
    return KE_MSG_CONSUMED;
}
