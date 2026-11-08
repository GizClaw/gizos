#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hal/h2_pal_ble.h"
#include "h2_bk_ble_gatt_schema.h"

typedef struct {
  uint16_t len;
  union {
    uint8_t uuid128[16];
  } uuid;
} bk_bt_uuid_t;

static bk_bt_uuid_t s_h2_bk_ble_service_uuid[H2_BK_BLE_MAX_GATT_SERVICES];
static bk_bt_uuid_t s_h2_bk_ble_char_uuid[H2_BK_BLE_MAX_GATT_CHARACTERISTICS];
static uint8_t* s_h2_bk_ble_value[H2_BK_BLE_MAX_GATT_CHARACTERISTICS];
static uint16_t s_h2_bk_ble_value_len[H2_BK_BLE_MAX_GATT_CHARACTERISTICS];
static uint16_t s_h2_bk_ble_value_max_len[H2_BK_BLE_MAX_GATT_CHARACTERISTICS];
static uint32_t s_h2_bk_ble_properties[H2_BK_BLE_MAX_GATT_CHARACTERISTICS];
static uint32_t s_h2_bk_ble_permissions[H2_BK_BLE_MAX_GATT_CHARACTERISTICS];
static uint16_t s_h2_bk_ble_value_handle[H2_BK_BLE_MAX_GATT_CHARACTERISTICS];
static uint16_t s_h2_bk_ble_cccd_handle[H2_BK_BLE_MAX_GATT_CHARACTERISTICS];
enum {
  BK_OK = 0,
  kNoErr = 0,
  BK_BLE_HOST_STACK_TYPE_ETHERMIND = 1,
  BK_UUID_LEN_16 = 2,
  BK_UUID_LEN_32 = 4,
  BK_UUID_LEN_128 = 16,
  H2_BK_BLE_MAX_VALUE_LEN = H2_PAL_BLE_ATT_MAX_VALUE_LEN,
  H2_BK_BLE_ADV_SET_COUNT = 2,
  H2_BK_BLE_TIMEOUT_MS = 100,
  H2_BK_BLE_GATTS_TX_IDLE = 0,
  H2_PAL_SYSTEM_EVENT_TYPE_BLE_HOST_STARTED = 1,
  H2_PAL_SYSTEM_EVENT_TYPE_BLE_HOST_STOPPED = 2
};

static int mutex_storage;
static void* s_h2_bk_ble_gatt_mutex = &mutex_storage;
static int mutex_depth, fail_lock;
static size_t s_h2_bk_ble_service_count = 2;
static int s_h2_bk_ble_service_started[H2_BK_BLE_MAX_GATT_SERVICES];
static size_t
    s_h2_bk_ble_service_characteristic_count[H2_BK_BLE_MAX_GATT_SERVICES] = {1,
                                                                             3};
static int s_h2_bk_ble_legacy_service_created, s_h2_bk_ble_legacy_db_cfg;
static int s_h2_bk_ble_attr_db[H2_BK_BLE_MAX_GATT_SERVICES]
                              [H2_BK_BLE_ATTR_COUNT];
static uint16_t s_h2_bk_ble_service_handle[H2_BK_BLE_MAX_GATT_SERVICES];
static size_t s_h2_bk_ble_pending_service_index;
static int s_h2_bk_ble_gatts_if = 1;
static int s_h2_bk_ble_started, s_h2_bk_ble_gatt_attached;
static h2_pal_ble_gatt_read_fn
    s_h2_bk_ble_read[H2_BK_BLE_MAX_GATT_CHARACTERISTICS];
static h2_pal_ble_gatt_write_fn
    s_h2_bk_ble_write[H2_BK_BLE_MAX_GATT_CHARACTERISTICS];
static void* s_h2_bk_ble_gatt_user[H2_BK_BLE_MAX_GATT_CHARACTERISTICS];
static uint16_t* s_h2_bk_ble_out_service_handle[H2_BK_BLE_MAX_GATT_SERVICES];
static uint16_t*
    s_h2_bk_ble_out_value_handle[H2_BK_BLE_MAX_GATT_CHARACTERISTICS];
static uint16_t*
    s_h2_bk_ble_out_cccd_handle[H2_BK_BLE_MAX_GATT_CHARACTERISTICS];
static struct {
  int kind, conn_handle;
} s_h2_bk_ble_gatts_tx;
typedef struct {
  int allocated;
} adv_set_t;
static adv_set_t s_h2_bk_ble_adv_sets[H2_BK_BLE_ADV_SET_COUNT];
static int s_h2_bk_ble_conn_handle, s_h2_bk_ble_peripheral_conn_handle;
static int s_h2_bk_ble_hci_handle, s_h2_bk_ble_peripheral_hci_handle;
static int s_h2_bk_ble_gattc_connected, s_h2_bk_ble_adv_data_staged;
static int s_h2_bk_ble_legacy_adv_data_valid;
static size_t s_h2_bk_ble_adv_data_len, s_h2_bk_ble_legacy_adv_data_len;
static size_t s_h2_bk_ble_legacy_scan_rsp_data_len;
static unsigned sdk_tables, sdk_starts, host_starts, host_stops;

static int rtos_lock_mutex(void** mutex) {
  assert(mutex == &s_h2_bk_ble_gatt_mutex && *mutex != NULL);
  if (fail_lock) return -1;
  assert(mutex_depth == 0);
  ++mutex_depth;
  return kNoErr;
}
static int rtos_unlock_mutex(void** mutex) {
  assert(mutex == &s_h2_bk_ble_gatt_mutex && mutex_depth == 1);
  --mutex_depth;
  return kNoErr;
}
static int bk_ble_get_host_stack_type(void) {
  return BK_BLE_HOST_STACK_TYPE_ETHERMIND;
}
static int h2_bk_ble_ensure_gatt_mutex(void) { return H2_PAL_OK; }
static void* psram_malloc(size_t size) { return malloc(size); }
static void h2_bk_ble_rollback_service_slot(size_t index) {
  (void)index;
  assert(!"unexpected registration rollback");
}
static h2_pal_result_t probe_read(void* user,
                                  const h2_pal_ble_gatt_access_t* access,
                                  uint8_t* out, size_t size, size_t* out_len) {
  (void)access;
  assert(size > 0 && user != NULL);
  out[0] = (uint8_t)*(int*)user;
  *out_len = 1;
  return H2_PAL_OK;
}
static h2_pal_result_t probe_write(void* user,
                                   const h2_pal_ble_gatt_access_t* access,
                                   const uint8_t* data, size_t size) {
  (void)access;
  assert(size == 1 && user != NULL);
  *(int*)user = data[0];
  return H2_PAL_OK;
}
static int h2_bk_ble_ensure_sem(void) { return H2_PAL_OK; }
static int h2_bk_ble_ensure_gap_registered(void) { return H2_PAL_OK; }
static int h2_bk_ble_ensure_gatts_registered(void) { return H2_PAL_OK; }
static int h2_bk_ble_ensure_adv_mutex(void) { return H2_PAL_OK; }
static int h2_bk_ble_set_local_mtu(void) { return H2_PAL_OK; }
static int h2_bk_ble_map_error(int rc) { return rc; }
static int h2_bk_ble_wait(int timeout) {
  assert(timeout == H2_BK_BLE_TIMEOUT_MS);
  return H2_PAL_OK;
}
static void h2_bk_ble_build_legacy_attr_db(void) {}
static void h2_bk_ble_notice_cb(void) {}
static void bk_ble_set_notice_cb(void (*callback)(void)) { (void)callback; }
static int bk_ble_create_db(void* config) {
  (void)config;
  return BK_OK;
}
static uint8_t h2_bk_ble_build_attr_db(size_t index) {
  assert(index < s_h2_bk_ble_service_count);
  return (uint8_t)(1 + s_h2_bk_ble_service_characteristic_count[index]);
}
static int bk_ble_gatts_create_attr_tab(const int* db, int interface,
                                        uint8_t count, uint32_t maximum) {
  size_t index = s_h2_bk_ble_pending_service_index;
  assert(index < s_h2_bk_ble_service_count);
  assert(db == s_h2_bk_ble_attr_db[index] && interface == 1);
  assert(maximum == count + s_h2_bk_ble_service_characteristic_count[index]);
  /* SDK service tables survive the provider's logical Host stop. */
  if (sdk_tables == 6) return -1;
  s_h2_bk_ble_service_handle[index] = (uint16_t)(100 + ++sdk_tables);
  size_t first = h2_bk_ble_gatt_schema_first_slot(index);
  for (size_t i = 0; i < s_h2_bk_ble_service_characteristic_count[index]; ++i) {
    s_h2_bk_ble_value_handle[first + i] = (uint16_t)(200 + first + i);
    s_h2_bk_ble_cccd_handle[first + i] = (uint16_t)(300 + first + i);
  }
  return BK_OK;
}
static int bk_ble_gatts_start_service(uint16_t handle) {
  assert(handle > 100 && handle <= 100 + sdk_tables);
  ++sdk_starts;
  return BK_OK;
}
static void h2_bk_ble_post(int event, const void* data, size_t size) {
  assert(data == NULL && size == 0);
  if (event == H2_PAL_SYSTEM_EVENT_TYPE_BLE_HOST_STARTED)
    ++host_starts;
  else {
    assert(event == H2_PAL_SYSTEM_EVENT_TYPE_BLE_HOST_STOPPED);
    ++host_stops;
  }
}
static void h2_bk_ble_finish_gatts_tx_unlocked(int connection, int rc) {
  (void)connection;
  assert(mutex_depth == 1 && rc == H2_PAL_ERR_CLOSED);
  s_h2_bk_ble_gatts_tx.kind = H2_BK_BLE_GATTS_TX_IDLE;
}
static int h2_bk_ble_cleanup_scan(void) { return H2_PAL_OK; }
static int h2_bk_ble_lock_adv(void) { return H2_PAL_OK; }
static void h2_bk_ble_unlock_adv(void) {}
static int h2_bk_ble_cleanup_adv_set(adv_set_t* set) {
  set->allocated = 0;
  return H2_PAL_OK;
}
static int h2_bk_ble_cleanup_advertising(void) { return H2_PAL_OK; }
static int h2_bk_ble_cleanup_initiator(void) { return H2_PAL_OK; }

/* PRODUCTION_FUNCTIONS */

static void probe_retained_schema(int scenario) {
  const uint8_t service_uuid[] = {0x00, 0x18};
  const uint8_t char_uuid[] = {0x00, 0x2a};
  const uint8_t initial[] = {42};
  int old_user = 11, new_user = 22;
  uint16_t service_handle = 0, value_handle = 0, cccd_handle = 0;
  h2_pal_ble_gatt_characteristic_t chars[2] = {{
      .uuid = {.data = char_uuid, .len = sizeof(char_uuid)},
      .properties =
          H2_PAL_BLE_GATT_PROPERTY_READ | H2_PAL_BLE_GATT_PROPERTY_WRITE,
      .permissions =
          H2_PAL_BLE_GATT_PERMISSION_READ | H2_PAL_BLE_GATT_PERMISSION_WRITE,
      .initial_value = initial,
      .initial_value_len = sizeof(initial),
      .max_value_len = 0,
      .read = probe_read,
      .write = probe_write,
      .user = &old_user,
      .out_value_handle = &value_handle,
      .out_cccd_handle = &cccd_handle,
  }};
  chars[1] = chars[0];
  h2_pal_ble_gatt_service_t service = {
      .uuid = {.data = service_uuid, .len = sizeof(service_uuid)},
      .primary = true,
      .characteristics = chars,
      .characteristic_count = 2,
      .out_service_handle = &service_handle,
  };
  s_h2_bk_ble_service_count = 0;
  assert(h2_bk_ble_register_gatt_services(NULL, &service, 1) == H2_PAL_OK);
  assert(h2_bk_ble_start(NULL) == H2_PAL_OK);
  assert(h2_bk_ble_stop(NULL) == H2_PAL_OK);
  const uint32_t properties = chars[1].properties;
  const uint32_t permissions = chars[1].permissions;
  chars[0].user = chars[1].user = &new_user;
  if (scenario < 6) {
    /* A mismatch in the last characteristic must not partially rebind the
     * first. */
    unsigned cases = scenario == 3 ? 8 : scenario == 4 ? 5 : 2;
    for (unsigned i = 0; i < cases; ++i) {
      chars[1].permissions = permissions;
      chars[1].properties = properties;
      chars[1].max_value_len = 0;
      if (scenario == 3) {
        /* Include permission removal and encrypted/authenticated tightening. */
        if (i < 6) {
          chars[1].permissions = permissions ^ (1u << i);
        } else {
          chars[1].permissions =
              i == 6 ? H2_PAL_BLE_GATT_PERMISSION_READ_ENCRYPTED |
                           H2_PAL_BLE_GATT_PERMISSION_WRITE_ENCRYPTED
                     : H2_PAL_BLE_GATT_PERMISSION_READ_AUTHENTICATED |
                           H2_PAL_BLE_GATT_PERMISSION_WRITE_AUTHENTICATED;
        }
      } else if (scenario == 4) {
        /* Includes adding notify/indicate (CCCD layout) and write-no-response.
         */
        chars[1].properties = properties ^ (1u << i);
      } else {
        assert(scenario == 5);
        chars[1].max_value_len = i == 0 ? 16 : 128;
      }
      service_handle = value_handle = cccd_handle = 999;
      assert(h2_bk_ble_register_gatt_services(NULL, &service, 1) ==
             H2_PAL_ERR_INVALID_STATE);
      assert(mutex_depth == 0 && !s_h2_bk_ble_gatt_attached);
      assert(service_handle == 999 && value_handle == 999 &&
             cccd_handle == 999);
      for (size_t j = 0; j < 2; ++j) {
        assert(s_h2_bk_ble_read[j] == NULL && s_h2_bk_ble_write[j] == NULL);
        assert(s_h2_bk_ble_gatt_user[j] == NULL);
        assert(s_h2_bk_ble_out_value_handle[j] == NULL);
        assert(s_h2_bk_ble_properties[j] == properties);
        assert(s_h2_bk_ble_permissions[j] == permissions);
        assert(s_h2_bk_ble_value_max_len[j] == H2_BK_BLE_MAX_VALUE_LEN);
        assert(s_h2_bk_ble_value[j][0] == initial[0]);
      }
      assert(sdk_tables == 1 && sdk_starts == 1);
    }
  } else {
    assert(scenario == 6);
  }
  chars[1].permissions = permissions;
  chars[1].properties = properties;
  /* Explicit full capacity and default capacity describe the same schema. */
  chars[1].max_value_len = H2_BK_BLE_MAX_VALUE_LEN;
  chars[0].read = NULL;
  for (unsigned cycle = 0; cycle < 128; ++cycle) {
    assert(h2_bk_ble_register_gatt_services(NULL, &service, 1) == H2_PAL_OK);
    assert(service_handle == 101 && value_handle == 201 && cccd_handle == 301);
    assert(h2_bk_ble_start(NULL) == H2_PAL_OK);
    assert(s_h2_bk_ble_read[0] == NULL);
    uint8_t out = 0;
    size_t out_len = 0;
    assert(s_h2_bk_ble_read[1](s_h2_bk_ble_gatt_user[1], NULL, &out, 1,
                               &out_len) == H2_PAL_OK);
    assert(out == (uint8_t)new_user && out_len == 1);
    const uint8_t value = (uint8_t)cycle;
    assert(s_h2_bk_ble_write[1](s_h2_bk_ble_gatt_user[1], NULL, &value, 1) ==
           H2_PAL_OK);
    assert(new_user == (int)cycle && old_user == 11);
    assert(h2_bk_ble_stop(NULL) == H2_PAL_OK);
    assert(s_h2_bk_ble_read[1] == NULL && s_h2_bk_ble_gatt_user[1] == NULL);
  }
  assert(sdk_tables == 1 && sdk_starts == 1);
  for (size_t j = 0; j < 2; ++j) free(s_h2_bk_ble_value[j]);
  puts(
      "PASS retained schema validation and callback rebinding across Host stop");
}

int main(int argc, char** argv) {
  (void)h2_bk_ble_unregister_gatt_services;
  assert(argc == 2);
  int scenario = atoi(argv[1]);
  if (scenario >= 3) {
    probe_retained_schema(scenario);
    return 0;
  }
  uint16_t output = 0;
  int borrowed = 123;
  if (scenario == 0) {
    for (unsigned cycle = 0; cycle < 128; ++cycle) {
      int rc = h2_bk_ble_start(NULL);
      if (rc != H2_PAL_OK) {
        fprintf(stderr,
                "Host restart failed: cycle=%u rc=%d retained_tables=%u\n",
                cycle, rc, sdk_tables);
        return 2;
      }
      assert(h2_bk_ble_stop(NULL) == H2_PAL_OK);
      assert(!s_h2_bk_ble_started);
    }
    assert(sdk_tables == 2 && sdk_starts == 2);
    assert(host_starts == 128 && host_stops == 128);
    puts("PASS 128 Host cycles reuse the two installed SDK service tables");
    return 0;
  }
  assert(h2_bk_ble_start(NULL) == H2_PAL_OK);
  s_h2_bk_ble_gatt_attached = 1;
  s_h2_bk_ble_read[0] = probe_read;
  s_h2_bk_ble_write[0] = probe_write;
  s_h2_bk_ble_gatt_user[0] = &borrowed;
  s_h2_bk_ble_out_service_handle[0] = &output;
  s_h2_bk_ble_out_value_handle[0] = &output;
  s_h2_bk_ble_out_cccd_handle[0] = &output;
  if (scenario == 1) {
    assert(h2_bk_ble_stop(NULL) == H2_PAL_OK);
    assert(!s_h2_bk_ble_gatt_attached);
    assert(!s_h2_bk_ble_read[0] && !s_h2_bk_ble_write[0]);
    assert(!s_h2_bk_ble_gatt_user[0]);
    assert(!s_h2_bk_ble_out_service_handle[0]);
    assert(!s_h2_bk_ble_out_value_handle[0] && !s_h2_bk_ble_out_cccd_handle[0]);
    puts("PASS Host stop releases borrowed GATT bindings");
  } else {
    assert(scenario == 2);
    fail_lock = 1;
    assert(h2_bk_ble_stop(NULL) == H2_PAL_ERR_IO);
    assert(s_h2_bk_ble_started && s_h2_bk_ble_gatt_attached);
    assert(s_h2_bk_ble_gatt_user[0] == &borrowed);
    assert(host_stops == 0);
    fail_lock = 0;
    assert(h2_bk_ble_stop(NULL) == H2_PAL_OK);
    assert(!s_h2_bk_ble_gatt_user[0]);
    puts("PASS failed detach retains ownership and permits retry");
  }
  return 0;
}
