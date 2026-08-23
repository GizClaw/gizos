#ifndef H2_BK3633_BLE_SDK_FAKE_H
#define H2_BK3633_BLE_SDK_FAKE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CFG_BLE_EXT_ADV 1

typedef uint16_t ke_msg_id_t;
typedef uint16_t ke_task_id_t;
typedef uint16_t att_size_t;

#define TASK_APP 1u
#define TASK_GAPM 2u
#define TASK_GAPC 3u
#define TASK_GATTC 4u
#define KE_BUILD_ID(task, index) ((uint16_t)((task) | ((index) << 8)))
#define KE_IDX_GET(task) ((uint8_t)((task) >> 8))
#define KE_MSG_CONSUMED 0

#define GAP_ERR_NO_ERROR 0u
#define GAP_ERR_REJECTED 1u
#define GAP_AUTH_REQ_NO_MITM_NO_BOND 0u
#define CO_ERROR_REMOTE_USER_TERM_CON 0x13u
#define GAPM_STATIC_ADDR 0u
#define GAP_PHY_LE_1MBPS 1u
#define GAP_PHY_LE_2MBPS 2u
#define GAP_PHY_LE_CODED 3u
#define GAPM_PHY_TYPE_LE_2M 2u
#define GAPM_PHY_TYPE_LE_CODED 3u

#define GAPM_CREATE_ADV_ACTIVITY 10u
#define GAPM_CREATE_SCAN_ACTIVITY 11u
#define GAPM_START_ACTIVITY 12u
#define GAPM_STOP_ACTIVITY 13u
#define GAPM_DELETE_ACTIVITY 14u
#define GAPM_SET_ADV_DATA 15u
#define GAPM_SET_SCAN_RSP_DATA 16u
#define GAPM_ADV_TYPE_LEGACY 1u
#define GAPM_ADV_TYPE_EXTENDED 2u
#define GAPM_ADV_MODE_GEN_DISC 1u
#define GAPM_ADV_PROP_UNDIR_CONN_MASK 1u
#define GAPM_ADV_PROP_BROADCAST_NON_SCAN_MASK 2u
#define GAPM_EXT_ADV_PROP_UNDIR_CONN_MASK 3u
#define GAPM_EXT_ADV_PROP_NON_CONN_NON_SCAN_MASK 4u
#define GAPM_SCAN_TYPE_OBSERVER 1u
#define GAPM_SCAN_PROP_PHY_1M_BIT 0x01u
#define GAPM_SCAN_PROP_PHY_CODED_BIT 0x02u
#define GAPM_SCAN_PROP_ACTIVE_1M_BIT 0x04u
#define GAPM_SCAN_PROP_ACTIVE_CODED_BIT 0x08u
#define GAPM_ACTV_TYPE_ADV 1u
#define GAPM_ACTV_TYPE_SCAN 2u

#define GAPM_REPORT_INFO_CONN_ADV_BIT 0x01u
#define GAPM_REPORT_INFO_COMPLETE_BIT 0x02u
#define GAPM_REPORT_INFO_REPORT_TYPE_MASK 0x1cu
#define GAPM_REPORT_TYPE_ADV_LEG 0x04u
#define GAPM_REPORT_TYPE_SCAN_RSP_EXT 0x08u
#define GAPM_REPORT_TYPE_SCAN_RSP_LEG 0x0cu

#define GATTC_NOTIFY 1u
#define GATTC_INDICATE 2u

#define ATT_DECL_PRIMARY_SERVICE 0x2800u
#define ATT_DECL_SECONDARY_SERVICE 0x2801u
#define ATT_DECL_CHARACTERISTIC 0x2803u
#define ATT_DESC_CLIENT_CHAR_CFG 0x2902u
#define ATT_ERR_NO_ERROR 0u
#define ATT_ERR_INVALID_ATTRIBUTE_VAL_LEN 1u
#define ATT_ERR_INSUFF_RESOURCE 2u
#define ATT_ERR_UNLIKELY_ERR 3u
#define ATT_ERR_REQUEST_NOT_SUPPORTED 4u
#define ATT_ERR_WRITE_NOT_PERMITTED 5u

#define ENABLE 1u
#define UUID_128 2u
#define NO_AUTH 0u
#define UNAUTH 1u
#define AUTH 2u
#define RD 9u
#define WRITE_REQ 11u
#define WRITE_COMMAND 10u
#define NTF 12u
#define IND 13u
#define RP 0u
#define WP 2u
#define UUID_LEN 13u
#define RI 15u
#define SVC_DIS 4u
#define SVC_UUID_LEN 5u
#define SVC_SECONDARY 7u
#define PERM(a, b) ((uint16_t)((b) << ((a) & 15u)))
#define PERM_POS_RP 0u
#define PERM_POS_NP 6u
#define PERM_POS_IP 4u

enum {
    GAPM_RESET_CMD = 99u,
    GAPM_ACTIVITY_CREATE_CMD,
    GAPM_ACTIVITY_START_CMD,
    GAPM_ACTIVITY_STOP_CMD,
    GAPM_ACTIVITY_DELETE_CMD,
    GAPM_SET_ADV_DATA_CMD,
    GAPC_DISCONNECT_CMD,
    GAPC_CONNECTION_CFM,
    GAPC_PARAM_UPDATE_CFM,
    GAPC_GET_DEV_INFO_CFM,
    GAPC_SET_DEV_INFO_CFM,
    GATTC_READ_CFM,
    GATTC_WRITE_CFM,
    GATTC_ATT_INFO_CFM,
    GATTC_SEND_EVT_CMD,
    GAPC_CONNECTION_REQ_IND = 200u,
    GAPC_DISCONNECT_IND,
    GAPC_PARAM_UPDATED_IND,
    GAPC_PARAM_UPDATE_REQ_IND,
    GAPC_GET_DEV_INFO_REQ_IND,
    GAPC_SET_DEV_INFO_REQ_IND,
    GATTC_MTU_CHANGED_IND,
    GATTC_CMP_EVT,
    GATTC_READ_REQ_IND,
    GATTC_WRITE_REQ_IND,
    GATTC_ATT_INFO_REQ_IND,
    GAPM_ACTIVITY_CREATED_IND,
    GAPM_ACTIVITY_STOPPED_IND,
    GAPM_CMP_EVT,
    GAPM_EXT_ADV_REPORT_IND,
};

#define GAPM_RESET 1u

#define GAPC_DISCONNECT 1u
#define GAPC_DEV_NAME 1u
#define GAPC_DEV_SLV_PREF_PARAMS 2u

typedef struct bd_addr {
    uint8_t addr[6];
} bd_addr_t;

struct gapm_adv_primary_cfg {
    uint32_t adv_intv_min;
    uint32_t adv_intv_max;
    uint8_t chnl_map;
    uint8_t phy;
};

struct gapm_adv_secondary_cfg {
    uint8_t phy;
    uint8_t adv_sid;
};

struct gapm_adv_create_param {
    uint8_t type;
    uint8_t disc_mode;
    uint16_t prop;
    uint8_t filter_pol;
    struct gapm_adv_primary_cfg prim_cfg;
    struct gapm_adv_secondary_cfg second_cfg;
};

struct gapm_activity_create_adv_cmd {
    uint8_t operation;
    uint8_t own_addr_type;
    bd_addr_t own_addr;
    struct gapm_adv_create_param adv_param;
};

struct gapm_activity_create_cmd {
    uint8_t operation;
    uint8_t own_addr_type;
};

struct gapm_reset_cmd { uint8_t operation; };

struct gapm_scan_phy_param {
    uint16_t scan_intv;
    uint16_t scan_wd;
};

struct gapm_scan_param {
    uint8_t type;
    uint8_t prop;
    uint16_t dup_filt_pol;
    struct gapm_scan_phy_param scan_param_1m;
    struct gapm_scan_phy_param scan_param_coded;
    uint16_t duration;
};

struct gapm_adv_add_param {
    uint16_t duration;
    uint8_t max_adv_evt;
};

union gapm_u_param {
    struct gapm_scan_param scan_param;
    struct gapm_adv_add_param adv_add_param;
};

struct gapm_activity_start_cmd {
    uint8_t operation;
    uint8_t actv_idx;
    union gapm_u_param u_param;
};

struct gapm_activity_stop_cmd {
    uint8_t operation;
    uint8_t actv_idx;
};

struct gapm_activity_delete_cmd {
    uint8_t operation;
    uint8_t actv_idx;
};

struct gapm_set_adv_data_cmd {
    uint8_t operation;
    uint8_t actv_idx;
    uint16_t length;
    uint8_t data[];
};

struct gapm_activity_created_ind {
    uint8_t actv_idx;
    uint8_t actv_type;
    int8_t tx_pwr;
};

struct gapm_activity_stopped_ind {
    uint8_t actv_idx;
    uint8_t actv_type;
    uint8_t reason;
    uint8_t per_adv_stop;
};

struct gapm_cmp_evt {
    uint8_t operation;
    uint8_t status;
};

struct gap_bdaddr {
    bd_addr_t addr;
    uint8_t addr_type;
};

struct gapm_ext_adv_report_ind {
    struct gap_bdaddr trans_addr;
    int8_t rssi;
    uint16_t info;
    uint8_t phy_prim;
    uint8_t phy_second;
    uint8_t adv_sid;
    int8_t tx_pwr;
    uint16_t length;
    uint8_t data[];
};

struct gapc_connection_req_ind {
    uint16_t conhdl;
    uint8_t role;
    uint8_t peer_addr_type;
    bd_addr_t peer_addr;
};

struct gapc_connection_cfm { uint8_t auth; };
struct gapc_disconnect_cmd { uint8_t operation; uint8_t reason; };
struct gapc_disconnect_ind { uint16_t conhdl; uint8_t reason; };
struct gapc_param_updated_ind {
    uint16_t con_interval;
    uint16_t con_latency;
    uint16_t sup_to;
};
struct gapc_param_update_req_ind { uint8_t unused; };
struct gapc_param_update_cfm {
    bool accept;
    uint16_t ce_len_min;
    uint16_t ce_len_max;
};
struct gapc_get_dev_info_req_ind { uint8_t req; };
struct gapc_get_dev_info_cfm {
    uint8_t req;
    union {
        struct { uint16_t length; uint8_t value[1]; } name;
        struct {
            uint16_t con_intv_min;
            uint16_t con_intv_max;
            uint16_t slave_latency;
            uint16_t conn_timeout;
        } slv_pref_params;
    } info;
};
struct gapc_set_dev_info_req_ind { uint8_t req; };
struct gapc_set_dev_info_cfm { uint8_t req; uint8_t status; };

struct gattc_mtu_changed_ind { uint16_t mtu; };
struct gattc_cmp_evt { uint8_t operation; uint8_t status; uint16_t seq_num; };
struct gattc_read_req_ind { uint16_t handle; };
struct gattc_read_cfm {
    uint16_t handle;
    uint8_t status;
    uint16_t length;
    uint8_t value[];
};
struct gattc_write_req_ind {
    uint16_t handle;
    uint16_t offset;
    uint16_t length;
    uint8_t value[];
};
struct gattc_write_cfm { uint16_t handle; uint8_t status; };
struct gattc_att_info_req_ind { uint16_t handle; };
struct gattc_att_info_cfm {
    uint16_t handle;
    uint16_t length;
    uint8_t status;
};
struct gattc_send_evt_cmd {
    uint8_t operation;
    uint16_t seq_num;
    uint16_t handle;
    uint16_t length;
    uint8_t value[];
};

struct attm_desc {
    uint16_t uuid;
    uint16_t perm;
    uint16_t ext_perm;
    uint16_t max_size;
};

struct attm_desc_128 {
    uint8_t uuid[16];
    uint16_t perm;
    uint16_t ext_perm;
    uint16_t max_size;
};

void *h2_bk3633_ble_sdk_fake_alloc(
    size_t size, size_t extra, uint16_t id, uint16_t dest, uint16_t src);
void h2_bk3633_ble_sdk_fake_send(void *message);
void h2_bk3633_ble_sdk_fake_fail_next_allocations(size_t count);

#define KE_MSG_ALLOC(id, dest, src, type) \
    ((struct type *)h2_bk3633_ble_sdk_fake_alloc( \
        sizeof(struct type), 0u, (id), (dest), (src)))
#define KE_MSG_ALLOC_DYN(id, dest, src, type, extra) \
    ((struct type *)h2_bk3633_ble_sdk_fake_alloc( \
        sizeof(struct type), (extra), (id), (dest), (src)))
#define ke_msg_send(message) h2_bk3633_ble_sdk_fake_send(message)

uint8_t attm_svc_create_db(
    uint16_t *start_handle, uint16_t uuid, uint8_t *cfg_flag,
    uint8_t max_nb_att, uint8_t *att_tbl, uint16_t dest_id,
    const struct attm_desc *att_db, uint8_t sec_lvl);
uint8_t attm_svc_create_db_128(
    uint16_t *start_handle, const uint8_t uuid[16], uint8_t *cfg_flag,
    uint8_t max_nb_att, uint8_t *att_tbl, uint16_t dest_id,
    const struct attm_desc_128 *att_db, uint8_t sec_lvl);
uint8_t attmdb_svc_visibility_set(uint16_t handle, bool hide);
uint8_t attm_att_set_value(
    uint16_t handle, att_size_t length, uint16_t offset, uint8_t *value);
void attm_convert_to128(
    uint8_t out[16], const uint8_t *uuid, uint8_t uuid_len);

typedef struct h2_bk3633_ble_sdk_fake_message {
    uint16_t id;
    uint16_t dest;
    uint16_t src;
    const void *payload;
    size_t payload_size;
} h2_bk3633_ble_sdk_fake_message_t;

typedef struct h2_bk3633_ble_sdk_fake_service {
    uint16_t handle;
    size_t uuid_len;
    uint8_t uuid[16];
    uint8_t attribute_count;
    size_t attribute_uuid_len[16];
    uint8_t attribute_uuid[16][16];
    uint16_t attribute_ext_permissions[16];
    uint8_t permissions;
    bool hidden;
} h2_bk3633_ble_sdk_fake_service_t;

void h2_bk3633_ble_sdk_fake_reset(void);
size_t h2_bk3633_ble_sdk_fake_message_count(void);
const h2_bk3633_ble_sdk_fake_message_t *
h2_bk3633_ble_sdk_fake_message(size_t index);
size_t h2_bk3633_ble_sdk_fake_service_count(void);
const h2_bk3633_ble_sdk_fake_service_t *
h2_bk3633_ble_sdk_fake_service(size_t index);
void h2_bk3633_ble_sdk_fake_fail_service_create(size_t call_index);
void h2_bk3633_ble_sdk_fake_fail_service_visibility(size_t call_index);
void h2_bk3633_platform_ble_test_reset(void);
void h2_bk3633_platform_ble_test_set_indication_sequence(uint16_t sequence);

#endif
