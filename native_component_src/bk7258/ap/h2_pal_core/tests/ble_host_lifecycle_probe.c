#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "h2/pal/core/h2_pal_errors.h"
#include "h2_bk_ble_gatt_schema.h"

typedef int h2_pal_ble_t;
enum {
  BK_OK = 0,
  kNoErr = 0,
  BK_BLE_HOST_STACK_TYPE_ETHERMIND = 1,
  BK_UUID_LEN_16 = 2,
  H2_BK_BLE_ADV_SET_COUNT = 2,
  H2_BK_BLE_TIMEOUT_MS = 100,
  H2_BK_BLE_GATTS_TX_IDLE = 0,
  H2_PAL_BLE_INVALID_CONN_HANDLE = 65535,
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
static const void* s_h2_bk_ble_read[H2_BK_BLE_MAX_GATT_CHARACTERISTICS];
static const void* s_h2_bk_ble_write[H2_BK_BLE_MAX_GATT_CHARACTERISTICS];
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

int main(int argc, char** argv) {
  (void)h2_bk_ble_unregister_gatt_services;
  assert(argc == 2);
  int scenario = atoi(argv[1]);
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
  s_h2_bk_ble_read[0] = &borrowed;
  s_h2_bk_ble_write[0] = &borrowed;
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
