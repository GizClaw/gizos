#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "hal/h2_pal_ble.h"
#include "h2_bk_ble_gatt_schema.h"

typedef struct {
  uint16_t len;
  union {
    uint8_t uuid128[16];
  } uuid;
} bk_bt_uuid_t;

typedef struct {
  uint8_t uuid[16];
  uint16_t perm, ext_perm, max_size;
} ble_attm_desc_t;

static struct {
  uint16_t prf_task_id;
  uint8_t uuid[16], att_db_nb;
  uint16_t start_hdl;
  ble_attm_desc_t* att_db;
  uint8_t svc_perm;
} s_h2_bk_ble_legacy_db_cfg;

/* Public pinned SDK permission layout, independent of the encoded test data. */
#define BK_BLE_PERM_SET(access, right)                    \
  ((BK_BLE_PERM_RIGHT_##right << BK_BLE_##access##_POS) & \
   BK_BLE_##access##_MASK)
enum {
  BK_BLE_PERM_RIGHT_ENABLE = 1,
  BK_BLE_PERM_RIGHT_UNAUTH = 1,
  BK_BLE_PERM_RIGHT_SEC_CON = 3,
  BK_BLE_PERM_RIGHT_UUID_16 = 0,
  BK_BLE_PERM_RIGHT_UUID_32 = 1,
  BK_BLE_PERM_RIGHT_UUID_128 = 2,
  BK_BLE_RD_POS = 9,
  BK_BLE_RD_MASK = 0x0200,
  BK_BLE_WRITE_COMMAND_POS = 10,
  BK_BLE_WRITE_COMMAND_MASK = 0x0400,
  BK_BLE_WRITE_REQ_POS = 11,
  BK_BLE_WRITE_REQ_MASK = 0x0800,
  BK_BLE_NTF_POS = 12,
  BK_BLE_NTF_MASK = 0x1000,
  BK_BLE_RP_POS = 0,
  BK_BLE_RP_MASK = 0x0003,
  BK_BLE_WP_POS = 2,
  BK_BLE_WP_MASK = 0x000c,
  BK_BLE_RI_POS = 15,
  BK_BLE_RI_MASK = 0x8000,
  BK_BLE_UUID_LEN_POS = 13,
  BK_BLE_UUID_LEN_MASK = 0x6000,
  BK_BLE_SVC_UUID_LEN_POS = 5,
  BK_BLE_SVC_UUID_LEN_MASK = 0x60,
  BK_BLE_HOST_STACK_TYPE_ETHERMIND = 1,
  BK_UUID_LEN_16 = 2,
  BK_UUID_LEN_32 = 4,
  BK_UUID_LEN_128 = 16,
  H2_BK_BLE_LEGACY_IDX_SVC = H2_BK_BLE_LEGACY_SERVICE_INDEX,
  H2_BK_BLE_LEGACY_PRF_ID = 5,
  H2_BK_BLE_LOCAL_MAX_MTU = H2_PAL_BLE_ATT_MAX_MTU,
  H2_BK_BLE_TIMEOUT_MS = 5000
};

static bk_bt_uuid_t s_h2_bk_ble_service_uuid[H2_BK_BLE_MAX_GATT_SERVICES];
static bk_bt_uuid_t s_h2_bk_ble_char_uuid[H2_BK_BLE_MAX_GATT_CHARACTERISTICS];
static size_t s_h2_bk_ble_service_count;
static size_t
    s_h2_bk_ble_service_characteristic_count[H2_BK_BLE_MAX_GATT_SERVICES];
static uint32_t s_h2_bk_ble_properties[H2_BK_BLE_MAX_GATT_CHARACTERISTICS];
static uint32_t s_h2_bk_ble_permissions[H2_BK_BLE_MAX_GATT_CHARACTERISTICS];
static uint16_t s_h2_bk_ble_value_max_len[H2_BK_BLE_MAX_GATT_CHARACTERISTICS];
static ble_attm_desc_t s_h2_bk_ble_legacy_attr_db[H2_BK_BLE_LEGACY_ATTR_COUNT];
static int host_type, mtu_result, wait_result;
static unsigned legacy_mtu_calls, dm_mtu_calls;

static int bk_ble_get_host_stack_type(void) { return host_type; }
static void h2_bk_ble_notice_cb(void) {}
static void bk_ble_set_notice_cb(void (*callback)(void)) {
  assert(callback == h2_bk_ble_notice_cb);
}
static int h2_bk_ble_ensure_sem(void) { return H2_PAL_OK; }
static void h2_bk_ble_drain_signals(void) {}
static int h2_bk_ble_map_error(int result) { return result; }
static int bk_ble_set_max_mtu(uint16_t mtu) {
  assert(mtu == H2_PAL_BLE_ATT_MAX_MTU);
  ++legacy_mtu_calls;
  return mtu_result;
}
static int bk_ble_gatt_set_local_mtu(uint16_t mtu) {
  assert(mtu == H2_PAL_BLE_ATT_MAX_MTU);
  ++dm_mtu_calls;
  return mtu_result;
}
static int h2_bk_ble_wait(unsigned timeout) {
  assert(timeout == H2_BK_BLE_TIMEOUT_MS);
  return wait_result;
}
#define os_printf printf

/* PRODUCTION_FUNCTIONS */

int main(void) {
  const uint8_t uuid16[] = {0x0d, 0x18};
  const uint8_t uuid32[] = {0x78, 0x56, 0x34, 0x12};
  const uint8_t uuid128[] = {0x1d, 0x72, 0xa1, 0x6b, 0x3a, 0xaf, 0x0b, 0xaa,
                             0xe2, 0x53, 0xd8, 0x3e, 0x70, 0xb5, 0xa4, 0x71};
  const h2_pal_ble_uuid_t uuids[] = {
      {.data = uuid16, .len = sizeof(uuid16)},
      {.data = uuid32, .len = sizeof(uuid32)},
      {.data = uuid128, .len = sizeof(uuid128)},
  };
  const uint16_t widths[] = {0x0000, 0x2000, 0x4000};
  s_h2_bk_ble_service_count = 1;
  s_h2_bk_ble_service_characteristic_count[0] = 3;
  for (size_t i = 0; i < 3; ++i) {
    assert(h2_bk_ble_uuid_from_pal(&uuids[i], &s_h2_bk_ble_char_uuid[i]));
    s_h2_bk_ble_properties[i] =
        H2_PAL_BLE_GATT_PROPERTY_READ | H2_PAL_BLE_GATT_PROPERTY_WRITE |
        H2_PAL_BLE_GATT_PROPERTY_WRITE_NO_RSP | H2_PAL_BLE_GATT_PROPERTY_NOTIFY;
    s_h2_bk_ble_permissions[i] = H2_PAL_BLE_GATT_PERMISSION_READ_AUTHENTICATED |
                                 H2_PAL_BLE_GATT_PERMISSION_WRITE_ENCRYPTED;
    s_h2_bk_ble_value_max_len[i] = (uint16_t)(17u + i * 200u);
  }
  for (size_t service = 0; service < 3; ++service) {
    assert(
        h2_bk_ble_uuid_from_pal(&uuids[service], &s_h2_bk_ble_service_uuid[0]));
    assert(h2_bk_ble_set_local_mtu() == H2_PAL_OK);
    h2_bk_ble_build_legacy_attr_db();
    assert(memcmp(s_h2_bk_ble_legacy_db_cfg.uuid, uuids[service].data,
                  uuids[service].len) == 0);
    assert(s_h2_bk_ble_legacy_db_cfg.svc_perm == service * 0x20u);
    assert(s_h2_bk_ble_legacy_db_cfg.prf_task_id == 5);
    assert(s_h2_bk_ble_legacy_db_cfg.att_db_nb == 10);
    assert(s_h2_bk_ble_legacy_db_cfg.start_hdl == 0);
    assert(s_h2_bk_ble_legacy_db_cfg.att_db == s_h2_bk_ble_legacy_attr_db);
    for (size_t i = 0; i < 3; ++i) {
      const ble_attm_desc_t* value =
          &s_h2_bk_ble_legacy_attr_db[h2_bk_ble_legacy_value_slot(i)];
      assert(memcmp(value->uuid, uuids[i].data, uuids[i].len) == 0);
      for (size_t j = uuids[i].len; j < sizeof(value->uuid); ++j)
        assert(value->uuid[j] == 0);
      assert(value->ext_perm == (0x8000u | widths[i]));
      assert(value->perm == 0x1e07u);
      assert(value->max_size == s_h2_bk_ble_value_max_len[i]);
      const ble_attm_desc_t* declaration =
          &s_h2_bk_ble_legacy_attr_db[h2_bk_ble_legacy_declaration_index(i)];
      const ble_attm_desc_t* cccd =
          &s_h2_bk_ble_legacy_attr_db[h2_bk_ble_legacy_cccd_slot(i)];
      assert(declaration->uuid[0] == 0x03 && declaration->uuid[1] == 0x28);
      assert(cccd->uuid[0] == 0x02 && cccd->uuid[1] == 0x29);
    }
  }
  assert(legacy_mtu_calls == 3 && dm_mtu_calls == 0);
  s_h2_bk_ble_service_count = 2;
  assert(h2_bk_ble_set_local_mtu() == H2_PAL_ERR_UNSUPPORTED);
  assert(legacy_mtu_calls == 3);
  s_h2_bk_ble_service_count = 1;
  mtu_result = H2_PAL_ERR_IO;
  assert(h2_bk_ble_set_local_mtu() == H2_PAL_ERR_IO);
  mtu_result = H2_PAL_OK;
  wait_result = H2_PAL_ERR_TIMEOUT;
  assert(h2_bk_ble_set_local_mtu() == H2_PAL_ERR_TIMEOUT);
  wait_result = H2_PAL_OK;
  host_type = BK_BLE_HOST_STACK_TYPE_ETHERMIND;
  s_h2_bk_ble_service_count = 2;
  assert(h2_bk_ble_set_local_mtu() == H2_PAL_OK);
  assert(dm_mtu_calls == 1);
  puts("PASS production legacy GATT UUID widths and MTU guard/error paths");
  return 0;
}
