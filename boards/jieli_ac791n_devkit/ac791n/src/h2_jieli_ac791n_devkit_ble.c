#include "app_config.h"

#ifdef H2_JIELI_BLE_ENABLE

#include "system/includes.h"
#include "bt_common.h"
#include "btcontroller_config.h"
#include "btctrler/btctrler_task.h"
#include "btstack/avctp_user.h"
#include "btstack/btstack_task.h"
#include "btstack/btstack_error.h"
#include "btstack/le/att.h"
#include "btstack/le/ble_api.h"
#include "btstack/le/le_common_define.h"
#include "btstack/le/le_user.h"
#include "btstack/le/sm.h"
#include "event/bt_event.h"
#include "syscfg/syscfg_id.h"

#include "h2_jieli_ac791n_devkit.h"
#include "h2_jieli_wl82_platform_core.h"
#include "h2_jieli_wl82_atomic.h"
#include "h2_jieli_wl82_sdk_port.h"

#include <string.h>
#include <stdarg.h>

/* The composition root lends a firmware-lifetime sink. Publish it once so
 * SDK callbacks never race replacement or outlive the borrowed object. */
static const h2_pal_log_api_t *h2_ble_log_api;

static int h2_ble_log_bind(const h2_pal_log_api_t *log) {
  if (log == NULL || log->vtable == NULL || log->vtable->write == NULL) return 0;
  const h2_pal_log_api_t *expected = NULL;
  return __atomic_compare_exchange_n(&h2_ble_log_api, &expected, log, 0,
                                     __ATOMIC_RELEASE, __ATOMIC_RELAXED) || expected == log;
}

static void h2_ble_log(const char *format, ...) {
  const h2_pal_log_api_t *log = __atomic_load_n(&h2_ble_log_api, __ATOMIC_ACQUIRE);
  if (log == NULL) return;
  char message[H2_PAL_LOG_MESSAGE_MAX + 1u];
  va_list args;
  va_start(args, format);
  int length = vsnprintf(message, sizeof(message), format, args);
  va_end(args);
  if (length < 0) return;
  size_t used = (size_t)length < sizeof(message) ? (size_t)length : sizeof(message) - 1u;
  while (used != 0u && (message[used - 1u] == '\r' || message[used - 1u] == '\n')) --used;
  message[used] = '\0';
  (void)h2_pal_log_write(log, H2_PAL_LOG_DEBUG, "jieli/ble", message);
}


/* Required by JieLi's BLE-only and Wi-Fi+BLE reference entrypoints.  The
 * controller library owns the channel-update hook, while a peripheral-only
 * application must explicitly leave it unbound. */
void (*lmp_ch_update_resume_hdl)(void *priv) = NULL;

enum {
  H2_JIELI_GATT_DEVICE_NAME_HANDLE = 3,
  H2_JIELI_GATT_SERVICE_HANDLE = 4,
  H2_JIELI_GATT_TX_VALUE_HANDLE = 6,
  H2_JIELI_GATT_TX_CCCD_HANDLE = 7,
  H2_JIELI_GATT_RX_VALUE_HANDLE = 9,
  /* The SDK reference peripherals stop at a 200-byte MTU with a fixed
   * 512-byte send cbuf. A full 509-byte notification does not fit that cbuf
   * and stalls every later notification, so the cbuf scales with the MTU. */
  H2_JIELI_ATT_MTU = 512,
  H2_JIELI_ATT_SEND_CBUF_SIZE = 4 * H2_JIELI_ATT_MTU,
  H2_JIELI_ATT_BUFFER_SIZE =
      ATT_CTRL_BLOCK_SIZE + H2_JIELI_ATT_MTU + H2_JIELI_ATT_SEND_CBUF_SIZE,
  H2_JIELI_ADV_DATA_MAX = 251,
};

static const uint8_t h2_service_uuid[16] = {
    0x1d, 0x72, 0xa1, 0x6b, 0x3a, 0xaf, 0x0b, 0xaa,
    0xe2, 0x53, 0xd8, 0x3e, 0x70, 0xb5, 0xa4, 0x71,
};
static const uint8_t h2_tx_uuid[16] = {
    0x1e, 0xcf, 0xd2, 0xbc, 0x8f, 0xd3, 0xb0, 0x98,
    0x70, 0x51, 0xfb, 0x56, 0x55, 0xa0, 0xd3, 0x46,
};
static const uint8_t h2_rx_uuid[16] = {
    0xfe, 0x0e, 0xbc, 0xc9, 0xd6, 0x87, 0x36, 0xa5,
    0x8d, 0x5b, 0xf2, 0x05, 0x15, 0xad, 0x62, 0x8f,
};

/* Static H2Loader GATT schema. The PAL registration call binds the borrowed
 * callbacks and returns these deterministic handles. */
static const uint8_t h2_profile_data[] = {
    /* JieLi's ATT server requires the standard GAP service to lead the
     * generated database.  In particular, its connection path reads the
     * dynamic Device Name handle before serving application attributes. */
    0x0a, 0x00, 0x02, 0x00, 0x01, 0x00, 0x00, 0x28, 0x00, 0x18,
    0x0d, 0x00, 0x02, 0x00, 0x02, 0x00, 0x03, 0x28,
    0x02, 0x03, 0x00, 0x00, 0x2a,
    0x08, 0x00, 0x02, 0x01, 0x03, 0x00, 0x00, 0x2a,

    0x18, 0x00, 0x02, 0x00, 0x04, 0x00, 0x00, 0x28,
    0x1d, 0x72, 0xa1, 0x6b, 0x3a, 0xaf, 0x0b, 0xaa,
    0xe2, 0x53, 0xd8, 0x3e, 0x70, 0xb5, 0xa4, 0x71,
    0x1b, 0x00, 0x02, 0x00, 0x05, 0x00, 0x03, 0x28,
    0x10, 0x06, 0x00,
    0x1e, 0xcf, 0xd2, 0xbc, 0x8f, 0xd3, 0xb0, 0x98,
    0x70, 0x51, 0xfb, 0x56, 0x55, 0xa0, 0xd3, 0x46,
    0x16, 0x00, 0x10, 0x02, 0x06, 0x00,
    0x1e, 0xcf, 0xd2, 0xbc, 0x8f, 0xd3, 0xb0, 0x98,
    0x70, 0x51, 0xfb, 0x56, 0x55, 0xa0, 0xd3, 0x46,
    0x0a, 0x00, 0x0a, 0x01, 0x07, 0x00, 0x02, 0x29, 0x00, 0x00,
    0x1b, 0x00, 0x02, 0x00, 0x08, 0x00, 0x03, 0x28,
    0x0c, 0x09, 0x00,
    0xfe, 0x0e, 0xbc, 0xc9, 0xd6, 0x87, 0x36, 0xa5,
    0x8d, 0x5b, 0xf2, 0x05, 0x15, 0xad, 0x62, 0x8f,
    0x16, 0x00, 0x0c, 0x03, 0x09, 0x00,
    0xfe, 0x0e, 0xbc, 0xc9, 0xd6, 0x87, 0x36, 0xa5,
    0x8d, 0x5b, 0xf2, 0x05, 0x15, 0xad, 0x62, 0x8f,
    0x00, 0x00,
};

/* Diagnostic control copied byte-for-byte from JieLi's generated
 * bt_gatt_server profile.  Keeping the rest of the H2 provider unchanged
 * isolates the vendor ATT parser from our 128-bit H2 service definition. */
static const uint8_t h2_jieli_reference_profile_data[] = {
    0x0a, 0x00, 0x02, 0x00, 0x01, 0x00, 0x00, 0x28, 0x00, 0x18,
    0x0d, 0x00, 0x02, 0x00, 0x02, 0x00, 0x03, 0x28,
    0x02, 0x03, 0x00, 0x00, 0x2a,
    0x08, 0x00, 0x02, 0x01, 0x03, 0x00, 0x00, 0x2a,
    0x0a, 0x00, 0x02, 0x00, 0x04, 0x00, 0x00, 0x28, 0x00, 0xff,
    0x0d, 0x00, 0x02, 0x00, 0x05, 0x00, 0x03, 0x28,
    0x02, 0x06, 0x00, 0x01, 0xff,
    0x08, 0x00, 0x02, 0x01, 0x06, 0x00, 0x01, 0xff,
    0x0d, 0x00, 0x02, 0x00, 0x07, 0x00, 0x03, 0x28,
    0x0a, 0x08, 0x00, 0x02, 0xff,
    0x08, 0x00, 0x0a, 0x01, 0x08, 0x00, 0x02, 0xff,
    0x0d, 0x00, 0x02, 0x00, 0x09, 0x00, 0x03, 0x28,
    0x10, 0x0a, 0x00, 0x03, 0xff,
    0x08, 0x00, 0x10, 0x00, 0x0a, 0x00, 0x03, 0xff,
    0x0a, 0x00, 0x0a, 0x01, 0x0b, 0x00, 0x02, 0x29, 0x00, 0x00,
    0x00, 0x00,
};

struct h2_pal_ble_adv_set {
  h2_pal_ble_adv_params_t params;
  uint8_t data[H2_JIELI_ADV_DATA_MAX];
  uint8_t data_len;
  uint8_t scan_response_data[H2_PAL_BLE_LEGACY_ADV_DATA_MAX_LEN];
  uint8_t scan_response_data_len;
  int used;
  int started;
  int start_requested;
};

typedef struct h2_jieli_ble_state {
  int starting;
  int started;
  int stopping;
  int stop_worker;
  int native_created;
  int start_failed;
  uint16_t conn_handle;
  uint16_t retiring_connection;
  uint16_t mtu;
  h2_pal_ble_gatt_characteristic_t characteristics[2];
  int gatt_registered;
  struct h2_pal_ble_adv_set adv;
  struct conn_update_param_t conn_params;
  unsigned conn_pending;
  unsigned conn_submitting;
  unsigned conn_hook_skipped;
  unsigned command_rearm_needed;
  unsigned command_rearm_active;
  uint32_t conn_generation;
} h2_jieli_ble_state_t;

static h2_jieli_ble_state_t h2_ble;
/* GATT lifetime helpers. */
/* The gate protects binding publication and stack-owned callback references,
 * never an SDK call or a borrowed user callback. It outlives host restarts. */
static volatile uint32_t h2_gatt_gate;
typedef struct h2_gatt_call {
  struct h2_gatt_call *next;
  const void *task;
} h2_gatt_call_t;
static h2_gatt_call_t *h2_gatt_calls;
static unsigned h2_gatt_unregistering;

static void h2_gatt_lock(void) {
  for (;;) {
    uint32_t expected = 0u;
    if (h2_jieli_atomic_cas_u32(&h2_gatt_gate, &expected, 1u)) return;
    os_time_dly(1);
  }
}

static void h2_gatt_unlock(void) {
  h2_jieli_atomic_store_u32(&h2_gatt_gate, 0u);
}

static void h2_gatt_release(h2_gatt_call_t *call) {
  h2_gatt_lock();
  h2_gatt_call_t **link = &h2_gatt_calls;
  while (*link != call) link = &(*link)->next;
  *link = call->next;
  h2_gatt_unlock();
}
/* End GATT lifetime helpers. */

/* Host lifetime helpers. */
typedef struct h2_ble_call {
  struct h2_ble_call *next;
  const void *task;
} h2_ble_call_t;
static h2_ble_call_t *h2_ble_calls;

static int h2_ble_call_begin(h2_ble_call_t *call) {
  call->task = h2_jieli_sdk_task_current();
  h2_gatt_lock();
  if (h2_ble.stopping) {
    h2_gatt_unlock();
    return H2_PAL_ERR_BUSY;
  }
  call->next = h2_ble_calls;
  h2_ble_calls = call;
  h2_gatt_unlock();
  return H2_PAL_OK;
}

static void h2_ble_call_end(h2_ble_call_t *call) {
  h2_gatt_lock();
  h2_ble_call_t **link = &h2_ble_calls;
  while (*link != call) link = &(*link)->next;
  *link = call->next;
  h2_gatt_unlock();
}
/* End host lifetime helpers. */

static uint8_t h2_att_buffer[H2_JIELI_ATT_BUFFER_SIZE] __attribute__((aligned(4)));

typedef struct h2_jieli_att_trace {
  uint16_t handle;
  uint16_t offset;
  uint16_t size;
  uint8_t kind;
  uint8_t has_buffer;
} h2_jieli_att_trace_t;

static h2_jieli_att_trace_t h2_att_trace[16];
static uint8_t h2_att_trace_count;

static void h2_att_trace_record(
    uint8_t kind, uint16_t handle, uint16_t offset,
    const uint8_t *buffer, uint16_t size) {
  h2_gatt_lock();
  const uint8_t index = h2_att_trace_count < 16u
                            ? h2_att_trace_count
                            : (uint8_t)(h2_att_trace_count % 16u);
  h2_att_trace[index] = (h2_jieli_att_trace_t){
      .handle = handle,
      .offset = offset,
      .size = size,
      .kind = kind,
      .has_buffer = buffer != NULL,
  };
  h2_att_trace_count++;
  h2_gatt_unlock();
}

static void h2_att_trace_dump(void) {
  h2_jieli_att_trace_t snapshot[16];
  h2_gatt_lock();
  const uint8_t recorded = h2_att_trace_count;
  memcpy(snapshot, h2_att_trace, sizeof(snapshot));
  h2_gatt_unlock();
  const uint8_t count = recorded < 16u ? recorded : 16u;
  const uint8_t start = recorded <= 16u
                            ? 0u
                            : (uint8_t)(recorded % 16u);
  h2_ble_log("H2_JIELI_ATT_TRACE count=%u\r\n", (unsigned)recorded);
  for (uint8_t i = 0u; i < count; ++i) {
    const h2_jieli_att_trace_t *entry =
        &snapshot[(uint8_t)((start + i) % 16u)];
    h2_ble_log("H2_JIELI_ATT_ACCESS kind=%c handle=%u offset=%u size=%u buffer=%u\r\n",
           entry->kind == 0u ? 'R' : 'W', (unsigned)entry->handle,
           (unsigned)entry->offset, (unsigned)entry->size,
           (unsigned)entry->has_buffer);
  }
}

static int h2_unregister_gatt(void *user);
static int h2_adv_set_stop(void *user, h2_pal_ble_adv_set_t *set);
static int h2_adv_apply(struct h2_pal_ble_adv_set *set);
static int h2_adv_apply_with_params(struct h2_pal_ble_adv_set *set,
    const h2_pal_ble_adv_params_t *params, int automatic, int *submitted);

static void h2_restart_legacy_advertising(void) {
  const int rc = h2_adv_apply_with_params(&h2_ble.adv, NULL, 1, NULL);
  h2_ble_log("H2_JIELI_BLE_ADV_RESTART code=%d\r\n", rc);
}

struct h2_ext_adv_param {
  uint8_t handle;
  uint16_t properties;
  uint8_t interval_min[3];
  uint8_t interval_max[3];
  uint8_t channel_map;
  uint8_t own_addr_type;
  uint8_t peer_addr_type;
  uint8_t peer_addr[6];
  uint8_t filter_policy;
  uint8_t tx_power;
  uint8_t primary_phy;
  uint8_t secondary_max_skip;
  uint8_t secondary_phy;
  uint8_t sid;
  uint8_t scan_request_notification;
} __attribute__((packed));

struct h2_ext_adv_data {
  uint8_t handle;
  uint8_t operation;
  uint8_t fragment_preference;
  uint8_t length;
  uint8_t data[H2_JIELI_ADV_DATA_MAX];
} __attribute__((packed));

struct h2_ext_adv_enable {
  uint8_t enable;
  uint8_t number_of_sets;
  uint8_t handle;
  uint16_t duration;
  uint8_t max_events;
} __attribute__((packed));

/* Advertising command storage outlives both native pointer queues. */
static struct {
  struct h2_pal_ble_adv_set snapshot;
  struct h2_ext_adv_param params;
  struct h2_ext_adv_data data;
  struct h2_ext_adv_enable enable;
  uint32_t generation;
  unsigned phase; /* 0 reusable, 1 btstack, 2 controller, 3 failed fence */
  unsigned submitting;
  unsigned hook_skipped;
  unsigned restart;
} h2_adv_commands;

static void h2_ble_post(
    h2_pal_system_event_type_t type, const void *payload, size_t payload_size) {
  const h2_pal_system_event_t event = {
      .type = type,
      .source_id = 0u,
      .timestamp_ms = timer_get_ms(),
      .payload = payload,
      .payload_size = payload_size,
  };
  /* Use the SDK mutex path rather than the PAL's zero-timeout fast path.
   * OS_MUTEX is an SDK-owned static object and cannot safely be consumed as a
   * raw FreeRTOS queue and then released through os_mutex_post(). */
  const int rc = h2_pal_system_event_post(
      h2_jieli_wl82_platform_system_event_api(), &event, 1u);
  if (rc != H2_PAL_OK) {
    h2_ble_log("H2_JIELI_BLE_EVENT_POST type=%u code=%d\r\n",
           (unsigned)type, rc);
  }
}

static int h2_ble_cmd_result(int result) {
  if (result == BLE_CMD_RET_SUCESS) return H2_PAL_OK;
  if (result == BLE_CMD_RET_BUSY || result == BLE_BUFFER_FULL)
    return H2_PAL_ERR_WOULD_BLOCK;
  if (result == BLE_CMD_STACK_NOT_RUN) return H2_PAL_ERR_INVALID_STATE;
  return H2_PAL_ERR_IO;
}

static int h2_ble_cmd_trace(const char *operation, int vendor_result) {
  const int pal_result = h2_ble_cmd_result(vendor_result);
  h2_ble_log("H2_JIELI_BLE_VENDOR op=%s vendor=%d pal=%d\r\n",
         operation, vendor_result, pal_result);
  return pal_result;
}

/* The SDK's generic bt_get_mac_addr() waits for wifi_get_mac() whenever
 * CONFIG_WIFI_ENABLE is set.  Loader brings BLE up independently of Wi-Fi, so
 * use the BLE-only address path from the SDK's demo_matter example instead. */
static const uint8_t *h2_ble_base_mac(void) {
  static uint8_t mac[6];
  static const uint8_t erased[6] = {
      0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu,
  };
  if (syscfg_read(CFG_BT_MAC_ADDR, mac, sizeof(mac)) == (int)sizeof(mac) &&
      memcmp(mac, erased, sizeof(mac)) != 0) {
    return mac;
  }
  uint8_t flash_uid[16];
  memcpy(flash_uid, get_norflash_uuid(), sizeof(flash_uid));
  do {
    const uint32_t crc32 = rand32() ^ CRC32(flash_uid, sizeof(flash_uid));
    const uint16_t crc16 = rand32() ^ CRC16(flash_uid, sizeof(flash_uid));
    memcpy(mac, &crc32, sizeof(crc32));
    memcpy(mac + sizeof(crc32), &crc16, sizeof(crc16));
  } while (!bytecmp(mac, 0, sizeof(mac)));
  mac[0] &= (uint8_t)~((1u << 0u) | (1u << 1u));
  (void)syscfg_write(CFG_BT_MAC_ADDR, mac, sizeof(mac));
  return mac;
}

const char *h2_jieli_ac791n_devkit_device_uid(void) {
  static char uid[13];
  uint8_t address[6];
  extern void lib_make_ble_address(uint8_t *ble_address, uint8_t *edr_address);
  /* Use precisely the same identity derivation as controller startup. This
   * does not require starting Wi-Fi or the Bluetooth scheduler. */
  lib_make_ble_address(address, (uint8_t *)h2_ble_base_mac());
  static const char hex[] = "0123456789abcdef";
  for (unsigned i = 0; i < 6u; ++i) {
    uid[2u * i] = hex[address[5u - i] >> 4u];
    uid[2u * i + 1u] = hex[address[5u - i] & 15u];
  }
  uid[12] = '\0';
  return uid;
}

static int h2_uuid_equal(
    const h2_pal_ble_uuid_t *uuid, const uint8_t expected[16]) {
  return uuid != NULL && uuid->len == 16u && uuid->data != NULL &&
         memcmp(uuid->data, expected, 16u) == 0;
}

static int h2_adv_append(
    uint8_t *out, size_t capacity, size_t *used, uint8_t type,
    const uint8_t *data, size_t len) {
  if (len > 254u || *used + len + 2u > capacity) return H2_PAL_ERR_NO_SPACE;
  out[(*used)++] = (uint8_t)(len + 1u);
  out[(*used)++] = type;
  if (len != 0u) memcpy(out + *used, data, len);
  *used += len;
  return H2_PAL_OK;
}

static int h2_encode_adv_candidate(
    const h2_pal_ble_adv_data_t *data, uint8_t *out, size_t capacity,
    uint8_t *out_len) {
  if (data == NULL || (data->service_uuid_count != 0u && data->service_uuids == NULL))
    return H2_PAL_ERR_INVALID_ARG;
  size_t used = 0u;
  const uint8_t flags = 0x06u;
  int rc = h2_adv_append(out, capacity, &used, 0x01u, &flags, 1u);
  if (rc != H2_PAL_OK) return rc;
  for (size_t i = 0u; i < data->service_uuid_count; ++i) {
    const h2_pal_ble_uuid_t *uuid = &data->service_uuids[i];
    uint8_t type = uuid->len == 16u ? 0x07u : uuid->len == 2u ? 0x03u : 0u;
    if (type == 0u || uuid->data == NULL) return H2_PAL_ERR_INVALID_ARG;
    rc = h2_adv_append(out, capacity, &used, type, uuid->data, uuid->len);
    if (rc != H2_PAL_OK) return rc;
  }
  if (data->service_data.len != 0u || data->service_data_uuid.len != 0u) {
    uint8_t service[H2_JIELI_ADV_DATA_MAX];
    const size_t uuid_len = data->service_data_uuid.len;
    if ((uuid_len != 0u && uuid_len != 2u && uuid_len != 16u) ||
        (uuid_len != 0u && data->service_data_uuid.data == NULL) ||
        (uuid_len == 0u && data->service_data.len < 2u) ||
        data->service_data.len > sizeof(service) - uuid_len ||
        (data->service_data.len != 0u && data->service_data.data == NULL))
      return H2_PAL_ERR_INVALID_ARG;
    if (uuid_len != 0u)
      memcpy(service, data->service_data_uuid.data, uuid_len);
    if (data->service_data.len != 0u)
      memcpy(service + uuid_len, data->service_data.data, data->service_data.len);
    rc = h2_adv_append(
        out, capacity, &used, uuid_len == 16u ? 0x21u : 0x16u,
        service, uuid_len + data->service_data.len);
    if (rc != H2_PAL_OK) return rc;
  }
  if (data->manufacturer_data.len != 0u) {
    if (data->manufacturer_data.data == NULL) return H2_PAL_ERR_INVALID_ARG;
    rc = h2_adv_append(out, capacity, &used, 0xffu,
                       data->manufacturer_data.data,
                       data->manufacturer_data.len);
    if (rc != H2_PAL_OK) return rc;
  }
  if (data->local_name != NULL) {
    rc = h2_adv_append(out, capacity, &used, 0x09u,
                       (const uint8_t *)data->local_name,
                       strlen(data->local_name));
    if (rc != H2_PAL_OK) return rc;
  }
  *out_len = (uint8_t)used;
  return H2_PAL_OK;
}

/* Failed updates must preserve both the previous payload and its length. */
static int h2_encode_adv(
    const h2_pal_ble_adv_data_t *data, uint8_t *out, size_t capacity,
    uint8_t *out_len) {
  uint8_t candidate[H2_JIELI_ADV_DATA_MAX];
  uint8_t candidate_len = 0u;
  if (capacity > sizeof(candidate)) capacity = sizeof(candidate);
  int rc = h2_encode_adv_candidate(data, candidate, capacity, &candidate_len);
  if (rc != H2_PAL_OK) return rc;
  memcpy(out, candidate, candidate_len);
  *out_len = candidate_len;
  return H2_PAL_OK;
}

static uint8_t h2_adv_phy(h2_pal_ble_phy_t phy) {
  if (phy == H2_PAL_BLE_PHY_2M) return ADV_SET_2M_PHY;
  if (phy == H2_PAL_BLE_PHY_CODED) return ADV_SET_CODED_PHY;
  return ADV_SET_1M_PHY;
}

static void h2_u24(uint8_t out[3], uint32_t value) {
  out[0] = (uint8_t)value;
  out[1] = (uint8_t)(value >> 8u);
  out[2] = (uint8_t)(value >> 16u);
}

static int h2_adv_submit(struct h2_pal_ble_adv_set *set) {
  if (set->params.type == H2_PAL_BLE_ADV_TYPE_LEGACY) {
    if (set->data_len > H2_PAL_BLE_LEGACY_ADV_DATA_MAX_LEN)
      return H2_PAL_ERR_NO_SPACE;
    h2_ble_log("H2_JIELI_BLE_ADV_ENTER step=params\r\n");
    uint16_t interval_units =
        (uint16_t)((set->params.interval_min_ms * 8u) / 5u);
    h2_ble_log("H2_JIELI_BLE_ADV_INTERVAL units=%u\r\n",
           (unsigned)interval_units);
    int rc = h2_ble_cmd_trace("set_adv_param", ble_op_set_adv_param(
        interval_units,
        set->params.mode == H2_PAL_BLE_ADV_MODE_CONNECTABLE ? ADV_IND
                                                            : ADV_NONCONN_IND,
        ADV_CHANNEL_ALL));
    h2_ble_log("H2_JIELI_BLE_ADV_RETURN step=params code=%d\r\n", rc);
    if (rc == H2_PAL_OK) {
      h2_ble_log("H2_JIELI_BLE_ADV_ENTER step=data\r\n");
      rc = h2_ble_cmd_trace(
          "set_adv_data", ble_op_set_adv_data(set->data_len, set->data));
      h2_ble_log("H2_JIELI_BLE_ADV_RETURN step=data code=%d\r\n", rc);
    }
    if (rc == H2_PAL_OK) {
      h2_ble_log("H2_JIELI_BLE_ADV_ENTER step=response\r\n");
      rc = h2_ble_cmd_trace(
          "set_rsp_data", ble_op_set_rsp_data(
              set->scan_response_data_len, set->scan_response_data));
      h2_ble_log("H2_JIELI_BLE_ADV_RETURN step=response code=%d\r\n", rc);
    }
    if (rc == H2_PAL_OK) {
      h2_ble_log("H2_JIELI_BLE_ADV_ENTER step=enable\r\n");
      rc = h2_ble_cmd_trace("adv_enable", ble_op_adv_enable(1));
      h2_ble_log("H2_JIELI_BLE_ADV_RETURN step=enable code=%d\r\n", rc);
    }
    return rc;
  }
  struct h2_ext_adv_param *params = &h2_adv_commands.params;
  memset(params, 0, sizeof(*params));
  params->properties =
      set->params.mode == H2_PAL_BLE_ADV_MODE_CONNECTABLE ? 1u : 0u;
  h2_u24(params->interval_min, (set->params.interval_min_ms * 8u + 4u) / 5u);
  h2_u24(params->interval_max, (set->params.interval_max_ms * 8u + 4u) / 5u);
  params->channel_map = 7u;
  params->primary_phy = h2_adv_phy(set->params.primary_phy);
  params->secondary_phy = h2_adv_phy(set->params.secondary_phy);
  params->sid = set->params.sid;
  struct h2_ext_adv_data *encoded = &h2_adv_commands.data;
  *encoded = (struct h2_ext_adv_data){
      .handle = 0u, .operation = 3u, .fragment_preference = 0u,
      .length = set->data_len,
  };
  memcpy(encoded->data, set->data, set->data_len);
  struct h2_ext_adv_enable *enable = &h2_adv_commands.enable;
  *enable = (struct h2_ext_adv_enable){
      .enable = 1u, .number_of_sets = 1u, .handle = 0u,
      .duration = (uint16_t)(set->params.duration_ms / 10u),
      .max_events = set->params.max_adv_events,
  };
  int rc = h2_ble_cmd_result(ble_op_set_ext_adv_param(params, sizeof(*params)));
  if (rc == H2_PAL_OK)
    rc = h2_ble_cmd_result(ble_op_set_ext_adv_data(
        encoded, (uint16_t)(4u + encoded->length)));
  if (rc == H2_PAL_OK)
    rc = h2_ble_cmd_result(ble_op_set_ext_adv_enable(enable, sizeof(*enable)));
  return rc;
}

/* Advertising command lifetime. */
extern int ble_cmd_handler_is_idle(void);
extern void stack_run_loop_resume(void);
static void h2_connection_command_consumed(void);
static int h2_command_rearm(void);

static int h2_adv_controller_consumed(int generation) {
  h2_gatt_lock();
  if (h2_adv_commands.phase == 2u &&
      h2_adv_commands.generation == (uint32_t)generation)
    h2_adv_commands.phase = 0u;
  const int restart = h2_adv_commands.phase == 0u &&
      h2_adv_commands.restart && !h2_ble.stopping;
  if (restart) h2_ble.command_rearm_needed = 1u;
  h2_gatt_unlock();
  /* The btstack hook is one-shot; return the deferred restart to that task. */
  const int result = h2_command_rearm();
  stack_run_loop_resume();
  return result;
}

static void h2_adv_command_consumed(void) {
  h2_gatt_lock();
  if (h2_adv_commands.submitting) h2_adv_commands.hook_skipped = 1u;
  const uint32_t generation = h2_adv_commands.generation;
  const int eligible = h2_adv_commands.phase == 1u &&
                       !h2_adv_commands.submitting;
  h2_gatt_unlock();
  if (eligible && ble_cmd_handler_is_idle()) {
    h2_gatt_lock();
    const int retire = h2_adv_commands.generation == generation &&
                       h2_adv_commands.phase == 1u &&
                       !h2_adv_commands.submitting;
    if (h2_adv_commands.submitting) h2_adv_commands.hook_skipped = 1u;
    if (retire) h2_adv_commands.phase = 2u;
    h2_gatt_unlock();
    if (retire) {
      /* Extended descriptors cross a second FIFO. Q_CALLBACK executes on
       * the controller only after its earlier HCI messages were consumed. */
      const int result = btctrler_hci_cmd_to_task(
          Q_CALLBACK, 3, h2_adv_controller_consumed, 1, (int)generation);
      if (result != 0) {
        h2_gatt_lock();
        if (h2_adv_commands.generation == generation &&
            h2_adv_commands.phase == 2u)
          h2_adv_commands.phase = 3u;
        h2_gatt_unlock();
        h2_ble_log("H2_JIELI_BLE_ADV_FENCE_ERROR code=%d\r\n", result);
      }
    }
  }
  h2_gatt_lock();
  const int restart = h2_adv_commands.phase == 0u &&
      !h2_adv_commands.submitting && h2_adv_commands.restart && !h2_ble.stopping;
  if (restart) h2_adv_commands.restart = 0u;
  h2_gatt_unlock();
  if (restart) h2_restart_legacy_advertising();
}

static int h2_adv_apply_with_params(struct h2_pal_ble_adv_set *set,
    const h2_pal_ble_adv_params_t *params, int automatic, int *submitted) {
  if (submitted != NULL) *submitted = 0;
  h2_gatt_lock();
  if (set != &h2_ble.adv || !set->used || h2_ble.stopping ||
      (!h2_ble.started && !h2_ble.starting)) {
    h2_gatt_unlock();
    return H2_PAL_ERR_INVALID_STATE;
  }
  if (h2_ble.command_rearm_needed) {
    if (automatic) h2_adv_commands.restart = 1u;
    h2_gatt_unlock();
    const int result = h2_command_rearm();
    return result == H2_PAL_OK ? H2_PAL_ERR_WOULD_BLOCK : result;
  }
  if (automatic && (!set->start_requested || set->started ||
      set->params.type != H2_PAL_BLE_ADV_TYPE_LEGACY || h2_ble.conn_handle != 0u)) {
    h2_gatt_unlock();
    return H2_PAL_OK;
  }
  if (h2_adv_commands.phase != 0u || h2_adv_commands.submitting) {
    if (automatic) h2_adv_commands.restart = 1u;
    h2_gatt_unlock();
    return H2_PAL_ERR_WOULD_BLOCK;
  }
  if (params != NULL) set->params = *params;
  set->start_requested = 1;
  if (!h2_ble.started) {
    h2_gatt_unlock();
    return H2_PAL_OK;
  }
  h2_adv_commands.snapshot = *set;
  h2_adv_commands.phase = 1u;
  h2_adv_commands.submitting = 1u;
  h2_adv_commands.hook_skipped = 0u;
  h2_adv_commands.restart = 0u;
  h2_adv_commands.generation = h2_adv_commands.generation == INT32_MAX
      ? 1u : h2_adv_commands.generation + 1u;
  h2_gatt_unlock();
  int rc = h2_ble_cmd_result(ble_op_regist_thread_call(h2_connection_command_consumed));
  const int registered = rc == H2_PAL_OK;
  if (registered) rc = h2_adv_submit(&h2_adv_commands.snapshot);
  h2_gatt_lock();
  h2_adv_commands.submitting = 0u;
  const int rearm = h2_adv_commands.hook_skipped;
  h2_adv_commands.hook_skipped = 0u;
  if (!registered) h2_adv_commands.phase = 0u;
  if (registered && rearm) h2_ble.command_rearm_needed = 1u;
  if (rc == H2_PAL_OK)
    set->started = set->params.type != H2_PAL_BLE_ADV_TYPE_LEGACY ||
                   h2_ble.conn_handle == 0u;
  h2_gatt_unlock();
  /* An application-thread idle query cannot prove consumption. Requeue the
   * one-shot hook after publication instead of retiring borrowed storage here. */
  if (registered && rearm) {
    const int result = h2_command_rearm();
    if (result != H2_PAL_OK) rc = result;
  }
  if (registered) stack_run_loop_resume();
  if (rc == H2_PAL_OK && submitted != NULL) *submitted = 1;
  return rc;
}

static int h2_adv_apply(struct h2_pal_ble_adv_set *set) {
  return h2_adv_apply_with_params(set, NULL, 0, NULL);
}
/* End advertising command lifetime. */

static int h2_legacy_set_adv_data(
    void *user, const h2_pal_ble_adv_data_t *data) {
  (void)user;
  if (data == NULL || (data->service_uuid_count != 0u && data->service_uuids == NULL))
    return H2_PAL_ERR_INVALID_ARG;
  h2_pal_ble_adv_data_t primary = *data;
  primary.local_name = NULL;
  /* CoreBluetooth does not reliably merge legacy scan-response manufacturer
   * data into the discovery result.  Keep H2Loader's compact H2LD identity in
   * the primary packet and move the service UUID list into the scan response.
   * This also makes multiple nearby Loader devices distinguishable. */
  if (data->manufacturer_data.len != 0u) {
    primary.service_uuids = NULL;
    primary.service_uuid_count = 0u;
  }
  uint8_t primary_data[H2_PAL_BLE_LEGACY_ADV_DATA_MAX_LEN];
  uint8_t response_data[H2_PAL_BLE_LEGACY_ADV_DATA_MAX_LEN];
  uint8_t primary_len = 0u;
  int rc = h2_encode_adv(
      &primary, primary_data, sizeof(primary_data),
      &primary_len);
  if (rc != H2_PAL_OK) return rc;

  size_t scan_response_len = 0u;
  if (data->manufacturer_data.len != 0u) {
    for (size_t i = 0u; i < data->service_uuid_count; ++i) {
      const h2_pal_ble_uuid_t *uuid = &data->service_uuids[i];
      const uint8_t type = uuid->len == 16u ? 0x07u
                           : uuid->len == 2u ? 0x03u : 0u;
      if (type == 0u || uuid->data == NULL) return H2_PAL_ERR_INVALID_ARG;
      rc = h2_adv_append(
          response_data,
          sizeof(response_data), &scan_response_len, type,
          uuid->data, uuid->len);
      if (rc != H2_PAL_OK) return rc;
    }
  }
  if (data->local_name != NULL) {
    rc = h2_adv_append(
        response_data,
        sizeof(response_data), &scan_response_len, 0x09u,
        (const uint8_t *)data->local_name, strlen(data->local_name));
    if (rc != H2_PAL_OK) return rc;
  }
  h2_gatt_lock();
  memcpy(h2_ble.adv.data, primary_data, primary_len);
  memcpy(h2_ble.adv.scan_response_data, response_data, scan_response_len);
  h2_ble.adv.data_len = primary_len;
  h2_ble.adv.scan_response_data_len = (uint8_t)scan_response_len;
  h2_ble.adv.used = 1;
  h2_gatt_unlock();
  h2_ble_log("H2_JIELI_BLE_ADV_LAYOUT primary=%u response=%u identity=%s\r\n",
         (unsigned)primary_len, (unsigned)scan_response_len,
         data->manufacturer_data.len != 0u ? "primary" : "none");
  return H2_PAL_OK;
}

static int h2_legacy_start_advertising(
    void *user, const h2_pal_ble_adv_params_t *params) {
  (void)user;
  if (params == NULL || params->type != H2_PAL_BLE_ADV_TYPE_LEGACY)
    return H2_PAL_ERR_INVALID_ARG;
  return h2_adv_apply_with_params(&h2_ble.adv, params, 0, NULL);
}

static int h2_legacy_stop_advertising(void *user) {
  return h2_adv_set_stop(user, &h2_ble.adv);
}

static int h2_ble_start(void *user) {
  (void)user;
  extern const uint64_t config_btctler_le_features;
  h2_ble_log("H2_JIELI_BLE_FEATURES high=%08x low=%08x\r\n",
         (unsigned)(config_btctler_le_features >> 32u),
         (unsigned)config_btctler_le_features);
  h2_gatt_lock();
  if (h2_ble.start_failed) {
    h2_gatt_unlock();
    return H2_PAL_ERR_IO;
  }
  if (h2_ble.started || h2_ble.starting) {
    h2_gatt_unlock();
    return H2_PAL_OK;
  }
  h2_ble.starting = 1;
  h2_gatt_unlock();
  h2_ble_log("H2_JIELI_BLE_ENTER step=controller_prepare\r\n");
  /* JieLi's BLE-only reference applications disable Classic-BT sniff before
   * configuring the controller address and starting btstack.  Keep that SDK
   * ordering even though this PAL exposes BLE only. */
  extern void lmp_set_sniff_disable(void);
  lmp_set_sniff_disable();
  h2_ble_log("H2_JIELI_BLE_OK step=controller_prepare\r\n");
  uint8_t ble_addr[6];
  const uint8_t *base_addr;
  extern void lib_make_ble_address(uint8_t *ble_address, uint8_t *edr_address);
  extern int le_controller_set_mac(void *addr);
  h2_ble_log("H2_JIELI_BLE_ENTER step=base_mac\r\n");
  base_addr = h2_ble_base_mac();
  lib_make_ble_address(ble_addr, (uint8_t *)base_addr);
  h2_ble_log(
      "H2_JIELI_BLE_OK step=base_mac "
      "base=%02x:%02x:%02x:%02x:%02x:%02x "
      "ble=%02x:%02x:%02x:%02x:%02x:%02x\r\n",
      base_addr[5], base_addr[4], base_addr[3], base_addr[2], base_addr[1],
      base_addr[0], ble_addr[5], ble_addr[4], ble_addr[3], ble_addr[2],
      ble_addr[1], ble_addr[0]);
  h2_ble_log("H2_JIELI_BLE_ENTER step=controller_mac\r\n");
  const int mac_result = le_controller_set_mac(ble_addr);
  if (mac_result != 0) {
    h2_gatt_lock();
    h2_ble.starting = 0;
    h2_gatt_unlock();
    h2_ble_log("H2_JIELI_BLE_ERROR step=controller_mac vendor=%d\r\n", mac_result);
    return H2_PAL_ERR_IO;
  }
  h2_ble_log("H2_JIELI_BLE_OK step=controller_mac vendor=%d\r\n", mac_result);
  h2_ble_log("H2_JIELI_BLE_ENTER step=btstack_init\r\n");
  const int btstack_result = btstack_init();
  if (btstack_result == 0) {
    h2_gatt_lock();
    h2_ble.native_created = 1;
    h2_gatt_unlock();
    h2_ble_log("H2_JIELI_BLE_OK step=btstack_init vendor=%d\r\n",
           btstack_result);
    return H2_PAL_OK;
  }
  h2_ble_log("H2_JIELI_BLE_ERROR step=btstack_init vendor=%d\r\n",
         btstack_result);
  h2_gatt_lock();
  h2_ble.starting = 0;
  /* The pinned SDK marks its native task as created before task_create().
   * On failure the controller may remain alive, but btstack_exit() would
   * wait on a task that does not exist. No supported rollback clears that
   * SDK ownership flag: retain the failed host until reset. */
  h2_ble.start_failed = 1;
  h2_ble.stopping = 1;
  h2_gatt_unlock();
  return H2_PAL_ERR_IO;
}

static int h2_ble_stop(void *user) {
  const void *task = h2_jieli_sdk_task_current();
  const char *name = os_current_task();
  h2_gatt_lock();
  /* Full SDK shutdown waits for the native btstack task. INIT is delivered
   * by app_core; neither task may wait for work it must itself dispatch. */
  if (h2_ble.stop_worker ||
      (name != NULL && (strcmp(name, "btstack") == 0 ||
       (h2_ble.starting && strcmp(name, "app_core") == 0)))) {
    h2_gatt_unlock();
    return H2_PAL_ERR_BUSY;
  }
  for (h2_ble_call_t *call = h2_ble_calls; call != NULL; call = call->next) {
    if (call->task == task) {
      h2_gatt_unlock();
      return H2_PAL_ERR_BUSY;
    }
  }
  int had_host = h2_ble.started || h2_ble.starting || h2_ble.stopping;
  h2_ble.stopping = 1;
  h2_ble.stop_worker = 1;
  while (h2_ble_calls != NULL || h2_ble.starting) {
    h2_gatt_unlock();
    os_time_dly(1);
    h2_gatt_lock();
  }
  if (h2_ble.start_failed) {
    h2_ble.stop_worker = 0;
    h2_gatt_unlock();
    return H2_PAL_ERR_IO;
  }
  /* A start admitted before stop may have reached the SDK only while we
   * waited. Its task still needs shutdown even if INIT was suppressed. */
  had_host = had_host || h2_ble.native_created;
  const int adv_used = h2_ble.adv.used;
  const uint16_t connection = h2_ble.conn_handle;
  h2_gatt_unlock();

  int rc = H2_PAL_OK;
  if (adv_used) rc = h2_adv_set_stop(user, &h2_ble.adv);
  if (rc == H2_PAL_OK && connection != 0u) {
    rc = h2_ble_cmd_result(ble_op_disconnect(connection));
    if (rc == H2_PAL_OK) {
      /* Admission is closed and callbacks are retired. Remember an accepted
       * disconnect so a later shutdown attempt cannot submit it twice. */
      h2_gatt_lock();
      h2_ble.conn_handle = 0u;
      h2_ble.retiring_connection = connection;
      h2_gatt_unlock();
    }
  }
  if (rc == H2_PAL_OK && had_host) {
    /* Unlike BLE_CMD_STACK_EXIT, this also barriers and joins the native
     * task and releases controller/host memory. SDK return 1 means no task. */
    const int result = btstack_exit();
    if (result != 0 && result != 1) rc = H2_PAL_ERR_IO;
  }
  if (rc == H2_PAL_OK) {
    h2_gatt_lock();
    const uint16_t retired = h2_ble.retiring_connection;
    h2_ble.retiring_connection = 0u;
    h2_gatt_unlock();
    if (retired != 0u) {
      /* SDK callbacks are closed during shutdown. Preserve the disconnect
       * notification that wakes stream owners, after native teardown proves
       * the link is gone, and before publishing HOST_STOPPED. */
      const h2_pal_ble_disconnected_info_t info = {
          .conn_handle = retired,
          .reason = ERROR_CODE_CONNECTION_TERMINATED_BY_LOCAL_HOST,
      };
      h2_ble_post(H2_PAL_SYSTEM_EVENT_TYPE_BLE_DISCONNECTED, &info, sizeof(info));
    }
    rc = h2_unregister_gatt(user);
  }
  h2_gatt_lock();
  if (rc != H2_PAL_OK) {
    h2_ble.stop_worker = 0;
    h2_gatt_unlock();
    return rc;
  }
  memset(&h2_ble, 0, sizeof(h2_ble));
  memset(&h2_adv_commands, 0, sizeof(h2_adv_commands));
  h2_gatt_unlock();
  if (had_host)
    h2_ble_post(H2_PAL_SYSTEM_EVENT_TYPE_BLE_HOST_STOPPED, NULL, 0u);
  return H2_PAL_OK;
}

static int h2_adv_set_create(
    void *user, const h2_pal_ble_adv_params_t *params,
    h2_pal_ble_adv_set_t **out_set) {
  (void)user;
  if (params == NULL || out_set == NULL) return H2_PAL_ERR_INVALID_ARG;
  h2_gatt_lock();
  if (!h2_ble.started && !h2_ble.starting) {
    h2_gatt_unlock();
    return H2_PAL_ERR_INVALID_STATE;
  }
  if (h2_ble.adv.used) {
    h2_gatt_unlock();
    return H2_PAL_ERR_FULL;
  }
  memset(&h2_ble.adv, 0, sizeof(h2_ble.adv));
  h2_ble.adv.params = *params;
  h2_ble.adv.used = 1;
  *out_set = &h2_ble.adv;
  h2_gatt_unlock();
  return H2_PAL_OK;
}

static int h2_adv_set_data(
    void *user, h2_pal_ble_adv_set_t *set,
    const h2_pal_ble_adv_data_t *data) {
  (void)user;
  uint8_t candidate[H2_JIELI_ADV_DATA_MAX];
  uint8_t length = 0u;
  int rc = h2_encode_adv(data, candidate, sizeof(candidate), &length);
  if (rc != H2_PAL_OK) return rc;
  h2_gatt_lock();
  if (set != &h2_ble.adv || !set->used) {
    h2_gatt_unlock();
    return H2_PAL_ERR_INVALID_ARG;
  }
  memcpy(set->data, candidate, length);
  set->data_len = length;
  h2_gatt_unlock();
  return H2_PAL_OK;
}

static int h2_adv_set_start(void *user, h2_pal_ble_adv_set_t *set) {
  (void)user;
  int submitted = 0;
  const int rc = h2_adv_apply_with_params(set, NULL, 0, &submitted);
  if (rc == H2_PAL_OK && submitted) {
    const h2_pal_ble_adv_set_event_t event = {.set = set, .status = rc};
    h2_ble_post(H2_PAL_SYSTEM_EVENT_TYPE_BLE_ADVERTISING_STARTED,
                &event, sizeof(event));
  }
  return rc;
}

static int h2_adv_stop_request(h2_pal_ble_adv_set_t *set, int destroy) {
  h2_gatt_lock();
  if (set != &h2_ble.adv || !set->used) {
    h2_gatt_unlock();
    return H2_PAL_ERR_INVALID_ARG;
  }
  /* Full host exit proves consumption even when hook registration is broken. */
  if (!h2_ble.stopping &&
      (h2_ble.command_rearm_needed)) {
    h2_gatt_unlock();
    const int result = h2_command_rearm();
    return result == H2_PAL_OK ? H2_PAL_ERR_WOULD_BLOCK : result;
  }
  if (h2_adv_commands.submitting) {
    h2_gatt_unlock();
    return H2_PAL_ERR_WOULD_BLOCK;
  }
  h2_adv_commands.submitting = 1u;
  h2_adv_commands.hook_skipped = 0u;
  const int started = set->started;
  const int extended = set->params.type == H2_PAL_BLE_ADV_TYPE_EXTENDED;
  h2_gatt_unlock();
  int rc = H2_PAL_OK;
  if (started) {
    if (extended) {
      static const struct h2_ext_adv_enable disable = {
          .enable = 0u, .number_of_sets = 1u, .handle = 0u};
      rc = h2_ble_cmd_result(
          ble_op_set_ext_adv_enable(&disable, sizeof(disable)));
    } else {
      rc = h2_ble_cmd_result(ble_op_adv_enable(0));
    }
  }
  h2_gatt_lock();
  h2_adv_commands.submitting = 0u;
  const int rearm = h2_adv_commands.hook_skipped;
  h2_adv_commands.hook_skipped = 0u;
  if (rearm) h2_ble.command_rearm_needed = 1u;
  if (rc == H2_PAL_OK) {
    set->start_requested = 0;
    set->started = 0;
    h2_adv_commands.restart = 0u;
    if (destroy) memset(set, 0, sizeof(*set));
  }
  h2_gatt_unlock();
  if (rearm) {
    const int result = h2_command_rearm();
    if (result != H2_PAL_OK) rc = result;
  }
  /* A pending start's pointer fence may have observed stop's submission. */
  h2_gatt_lock();
  const int wake = h2_adv_commands.phase == 1u;
  h2_gatt_unlock();
  if (wake) stack_run_loop_resume();
  if (rc == H2_PAL_OK) {
    const h2_pal_ble_adv_set_event_t event = {.set = set, .status = rc};
    h2_ble_post(H2_PAL_SYSTEM_EVENT_TYPE_BLE_ADVERTISING_STOPPED,
                &event, sizeof(event));
  }
  return rc;
}

static int h2_adv_set_stop(void *user, h2_pal_ble_adv_set_t *set) {
  (void)user;
  return h2_adv_stop_request(set, 0);
}

static int h2_adv_set_destroy(void *user, h2_pal_ble_adv_set_t *set) {
  (void)user;
  return h2_adv_stop_request(set, 1);
}

static int h2_register_gatt(
    void *user, const h2_pal_ble_gatt_service_t *services, size_t count) {
  (void)user;
  if (count != 1u || services == NULL || !services[0].primary ||
      services[0].characteristic_count != 2u ||
      services[0].characteristics == NULL ||
      !h2_uuid_equal(&services[0].uuid, h2_service_uuid) ||
      !h2_uuid_equal(&services[0].characteristics[0].uuid, h2_tx_uuid) ||
      !h2_uuid_equal(&services[0].characteristics[1].uuid, h2_rx_uuid))
    return H2_PAL_ERR_UNSUPPORTED;
  h2_gatt_lock();
  if (h2_gatt_calls != NULL || h2_gatt_unregistering != 0u) {
    h2_gatt_unlock();
    return H2_PAL_ERR_BUSY;
  }
  h2_ble.characteristics[0] = services[0].characteristics[0];
  h2_ble.characteristics[1] = services[0].characteristics[1];
  if (services[0].out_service_handle != NULL)
    *services[0].out_service_handle = H2_JIELI_GATT_SERVICE_HANDLE;
  if (services[0].characteristics[0].out_value_handle != NULL)
    *services[0].characteristics[0].out_value_handle = H2_JIELI_GATT_TX_VALUE_HANDLE;
  if (services[0].characteristics[0].out_cccd_handle != NULL)
    *services[0].characteristics[0].out_cccd_handle = H2_JIELI_GATT_TX_CCCD_HANDLE;
  if (services[0].characteristics[1].out_value_handle != NULL)
    *services[0].characteristics[1].out_value_handle = H2_JIELI_GATT_RX_VALUE_HANDLE;
  h2_ble.gatt_registered = 1;
  h2_gatt_unlock();
  return H2_PAL_OK;
}

static int h2_unregister_gatt(void *user) {
  (void)user;
  const void *task = h2_jieli_sdk_task_current();
  h2_gatt_lock();
  for (h2_gatt_call_t *call = h2_gatt_calls; call != NULL; call = call->next) {
    if (call->task == task) {
      /* Successful unregister releases the caller's borrowed context. This
       * callback still owns it: report busy instead of waiting on ourselves
       * or claiming that its lifetime has already ended. */
      h2_gatt_unlock();
      return H2_PAL_ERR_BUSY;
    }
  }
  ++h2_gatt_unregistering;
  h2_ble.gatt_registered = 0;
  memset(h2_ble.characteristics, 0, sizeof(h2_ble.characteristics));
  while (h2_gatt_calls != NULL) {
    h2_gatt_unlock();
    os_time_dly(1);
    h2_gatt_lock();
  }
  --h2_gatt_unregistering;
  h2_gatt_unlock();
  return H2_PAL_OK;
}

static int h2_notify(
    void *user, uint16_t conn_handle, uint16_t attr_handle,
    const uint8_t *data, size_t len) {
  (void)user;
  h2_gatt_lock();
  if (conn_handle == 0u || conn_handle != h2_ble.conn_handle ||
      attr_handle != H2_JIELI_GATT_TX_VALUE_HANDLE ||
      (len != 0u && data == NULL) ||
      h2_ble.mtu < H2_PAL_BLE_ATT_HEADER_LEN ||
      len > (size_t)(h2_ble.mtu - H2_PAL_BLE_ATT_HEADER_LEN)) {
    h2_gatt_unlock();
    return H2_PAL_ERR_INVALID_ARG;
  }
  h2_gatt_unlock();
  return h2_ble_cmd_result(ble_op_att_send_data(
      attr_handle, data, (uint16_t)len, ATT_OP_AUTO_READ_CCC));
}

static int h2_disconnect(void *user, uint16_t conn_handle) {
  (void)user;
  h2_gatt_lock();
  if (conn_handle == 0u || conn_handle != h2_ble.conn_handle) {
    h2_gatt_unlock();
    return H2_PAL_ERR_INVALID_ARG;
  }
  h2_gatt_unlock();
  return h2_ble_cmd_result(ble_op_disconnect(conn_handle));
}

/* Connection request lifetime. */
/* Command hook retry. */
static void h2_connection_command_consumed(void);
static int h2_command_rearm(void) {
  h2_gatt_lock();
  if (h2_ble.stopping || !h2_ble.command_rearm_needed) {
    h2_gatt_unlock();
    return H2_PAL_OK;
  }
  if (h2_ble.command_rearm_active) {
    h2_gatt_unlock();
    return H2_PAL_ERR_WOULD_BLOCK;
  }
  /* Claim this attempt before calling the SDK: the hook can run before the
   * call returns and request another rearm. Never erase that newer request. */
  h2_ble.command_rearm_needed = 0u;
  h2_ble.command_rearm_active = 1u;
  h2_gatt_unlock();
  const int result = h2_ble_cmd_result(
      ble_op_regist_thread_call(h2_connection_command_consumed));
  h2_gatt_lock();
  h2_ble.command_rearm_active = 0u;
  if (result != H2_PAL_OK) h2_ble.command_rearm_needed = 1u;
  h2_gatt_unlock();
  if (result != H2_PAL_OK)
    h2_ble_log("H2_JIELI_BLE_REARM_ERROR code=%d\r\n", result);
  return result;
}
/* End command hook retry. */


/* These pinned SDK symbols run/query the native BLE command loop. A queue
 * empty query from an application thread is not a pointer-retirement fence. */
extern int ble_cmd_handler_is_idle(void);
extern void stack_run_loop_resume(void);

static void h2_connection_command_consumed(void) {
  h2_ble_call_t call;
  if (h2_ble_call_begin(&call) != H2_PAL_OK) return;
  h2_adv_command_consumed();
  h2_gatt_lock();
  if (h2_ble.conn_submitting) h2_ble.conn_hook_skipped = 1u;
  const uint32_t generation = h2_ble.conn_generation;
  const int eligible = h2_ble.conn_pending && !h2_ble.conn_submitting;
  h2_gatt_unlock();
  /* This hook runs after command dispatch on the consuming SDK thread.
   * Capture eligibility before the query: this thread cannot consume a new
   * producer's commands while it is executing this hook. */
  if (!eligible || !ble_cmd_handler_is_idle()) {
    (void)h2_command_rearm();
    h2_ble_call_end(&call);
    return;
  }
  h2_gatt_lock();
  if (h2_ble.conn_generation == generation && !h2_ble.conn_submitting)
    h2_ble.conn_pending = 0u;
  h2_gatt_unlock();
  (void)h2_command_rearm();
  h2_ble_call_end(&call);
}

static int h2_update_connection(
    void *user, uint16_t conn_handle,
    const h2_pal_ble_connection_params_t *params) {
  (void)user;
  if (params == NULL || params->interval_min_ms == 0u ||
      params->interval_max_ms < params->interval_min_ms)
    return H2_PAL_ERR_INVALID_ARG;
  h2_gatt_lock();
  if (conn_handle != h2_ble.conn_handle) {
    h2_gatt_unlock();
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (h2_ble.command_rearm_needed) {
    h2_gatt_unlock();
    const int rc = h2_command_rearm();
    return rc == H2_PAL_OK ? H2_PAL_ERR_WOULD_BLOCK : rc;
  }
  if (h2_ble.conn_pending) {
    h2_gatt_unlock();
    return H2_PAL_ERR_WOULD_BLOCK;
  }
  h2_ble.conn_pending = 1u;
  h2_ble.conn_submitting = 1u;
  h2_ble.conn_hook_skipped = 0u;
  ++h2_ble.conn_generation;
  h2_ble.conn_params = (struct conn_update_param_t){
      .interval_min = (uint16_t)(params->interval_min_ms * 4u / 5u),
      .interval_max = (uint16_t)(params->interval_max_ms * 4u / 5u),
      .latency = params->latency,
      .timeout = (uint16_t)(params->supervision_timeout_ms / 10u),
  };
  h2_gatt_unlock();
  /* Registration is itself queued before the borrowed request. SDK enqueue
   * failure leaves no borrowed command; successful enqueue keeps storage
   * immutable until the consumer hook, or full host shutdown, retires it. */
  int result = ble_op_regist_thread_call(h2_connection_command_consumed);
  if (result == 0)
    result = ble_op_conn_param_request(conn_handle, &h2_ble.conn_params);
  h2_gatt_lock();
  h2_ble.conn_submitting = 0u;
  const int rearm = h2_ble.conn_hook_skipped;
  h2_ble.conn_hook_skipped = 0u;
  if (result != 0) h2_ble.conn_pending = 0u;
  if (result == 0 && rearm) h2_ble.command_rearm_needed = 1u;
  h2_gatt_unlock();
  /* Only the consuming SDK thread can retire the request buffer. */
  const int rearm_result = result == 0 && rearm
      ? h2_command_rearm() : H2_PAL_OK;
  if (result == 0) stack_run_loop_resume();
  h2_ble_log(
      "H2_JIELI_BLE_CONN_PARAMS request=%u-%u latency=%u timeout=%u "
      "vendor=%d\r\n",
      (unsigned)params->interval_min_ms, (unsigned)params->interval_max_ms,
      (unsigned)params->latency, (unsigned)params->supervision_timeout_ms,
      result);
  return rearm_result != H2_PAL_OK ? rearm_result : h2_ble_cmd_result(result);
}

static int h2_exchange_mtu(
    void *user, uint16_t conn_handle, uint16_t *out_mtu,
    uint32_t timeout_ms) {
  (void)user;
  (void)timeout_ms;
  h2_gatt_lock();
  if (conn_handle != h2_ble.conn_handle || out_mtu == NULL) {
    h2_gatt_unlock();
    return H2_PAL_ERR_INVALID_ARG;
  }
  *out_mtu = h2_ble.mtu;
  h2_gatt_unlock();
  return H2_PAL_OK;
}

/* The central owns PHY selection. CoreBluetooth moves the link to 2M on its
 * own; a peripheral-initiated LL_PHY_REQ after that is answered with
 * LL_REJECT_IND, which this controller does not handle: ll_slave.c:662
 * asserts ("0 S LL_REJECT_IND") and resets the chip. */
static int h2_set_phy(
    void *user, uint16_t conn_handle, h2_pal_ble_phy_t tx_phy,
    h2_pal_ble_phy_t rx_phy, uint32_t timeout_ms) {
  (void)user;
  (void)tx_phy;
  (void)rx_phy;
  (void)timeout_ms;
  h2_gatt_lock();
  const int current = conn_handle == h2_ble.conn_handle;
  h2_gatt_unlock();
  return current ? H2_PAL_ERR_UNSUPPORTED : H2_PAL_ERR_INVALID_ARG;
}

static uint16_t h2_att_read(
    hci_con_handle_t connection_handle, uint16_t handle, uint16_t offset,
    uint8_t *buffer, uint16_t buffer_size) {
  (void)connection_handle;
  h2_att_trace_record(0u, handle, offset, buffer, buffer_size);
  if (handle == H2_JIELI_GATT_DEVICE_NAME_HANDLE) {
    static const uint8_t name[] = "H2Loader";
    if (offset >= sizeof(name) - 1u) return 0u;
    const uint16_t remaining = (uint16_t)(sizeof(name) - 1u - offset);
    if (buffer == NULL) return (uint16_t)(sizeof(name) - 1u);
    const uint16_t copied = remaining < buffer_size ? remaining : buffer_size;
    memcpy(buffer, name + offset, copied);
    return copied;
  }
  if (handle != H2_JIELI_GATT_TX_CCCD_HANDLE) return 0u;
  if (offset >= 2u) return 0u;
  if (buffer == NULL) return 2u;
  const uint8_t cccd[2] = {(uint8_t)att_get_ccc_config(handle), 0u};
  const uint16_t remaining = 2u - offset;
  const uint16_t copied = remaining < buffer_size ? remaining : buffer_size;
  memcpy(buffer, cccd + offset, copied);
  return copied;
}

static int h2_att_write(
    hci_con_handle_t connection_handle, uint16_t handle,
    uint16_t transaction_mode, uint16_t offset, uint8_t *buffer,
    uint16_t buffer_size) {
  (void)transaction_mode;
  h2_att_trace_record(1u, handle, offset, buffer, buffer_size);
  if (handle == H2_JIELI_GATT_TX_CCCD_HANDLE && buffer_size >= 2u) {
    const uint16_t value = (uint16_t)buffer[0] | ((uint16_t)buffer[1] << 8u);
    att_set_ccc_config(handle, (uint8_t)value);
    const h2_pal_ble_subscription_state_t state = {
        .conn_handle = connection_handle,
        .value_handle = H2_JIELI_GATT_TX_VALUE_HANDLE,
        .mode = H2_PAL_BLE_SUBSCRIBE_MODE_NOTIFY,
        .enabled = (value & 1u) != 0u,
    };
    h2_ble_post(H2_PAL_SYSTEM_EVENT_TYPE_BLE_SUBSCRIPTION_CHANGED,
                &state, sizeof(state));
    return 0;
  }
  if (handle == H2_JIELI_GATT_RX_VALUE_HANDLE) {
    h2_gatt_call_t call = {.task = h2_jieli_sdk_task_current()};
    h2_gatt_lock();
    if (!h2_ble.gatt_registered || h2_ble.characteristics[1].write == NULL) {
      h2_gatt_unlock();
      return 0;
    }
    const h2_pal_ble_gatt_characteristic_t rx = h2_ble.characteristics[1];
    call.next = h2_gatt_calls;
    h2_gatt_calls = &call;
    h2_gatt_unlock();
    const h2_pal_ble_gatt_access_t access = {
        .conn_handle = connection_handle,
        .attr_handle = handle,
        .offset = offset,
    };
    const int rc = rx.write(rx.user, &access, buffer, buffer_size);
    h2_gatt_release(&call);
    /* ATT Error Response: Unlikely Error. */
    return rc == H2_PAL_OK ? 0 : 0x0e;
  }
  return 0;
}

static void h2_packet_handler(
    uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
  (void)channel;
  if (packet_type != HCI_EVENT_PACKET) return;
  const uint8_t event_type = hci_event_packet_get_type(packet);
  switch (event_type) {
    case HCI_EVENT_COMMAND_COMPLETE:
      h2_ble_log(
          "H2_JIELI_BLE_HCI event=command-complete opcode=0x%02x%02x "
          "status=%u size=%u\r\n",
          size > 4u ? (unsigned)packet[4] : 0u,
          size > 3u ? (unsigned)packet[3] : 0u,
          size > 5u ? (unsigned)packet[5] : 0xffu, (unsigned)size);
      break;
    case HCI_EVENT_COMMAND_STATUS:
      h2_ble_log(
          "H2_JIELI_BLE_HCI event=command-status opcode=0x%02x%02x "
          "status=%u size=%u\r\n",
          size > 5u ? (unsigned)packet[5] : 0u,
          size > 4u ? (unsigned)packet[4] : 0u,
          size > 2u ? (unsigned)packet[2] : 0xffu, (unsigned)size);
      break;
    case HCI_EVENT_HARDWARE_ERROR:
      h2_ble_log("H2_JIELI_BLE_HCI event=hardware-error code=%u size=%u\r\n",
             size > 2u ? (unsigned)packet[2] : 0xffu, (unsigned)size);
      break;
    case SM_EVENT_JUST_WORKS_REQUEST: {
      const uint16_t handle =
          sm_event_just_works_request_get_handle(packet);
      /* TCFG_BLE_SECURITY_EN asks the controller to secure each new link.
       * JieLi's peripheral examples explicitly accept the resulting
       * Just-Works request; leaving it unanswered prevents CoreBluetooth
       * from completing the connection and reaching GATT discovery. */
      sm_just_works_confirm(handle);
      h2_ble_log("H2_JIELI_BLE_SECURITY just-works-confirm handle=%u\r\n",
             (unsigned)handle);
      break;
    }
    case HCI_EVENT_LE_META: {
      if (size >= 8u && packet[2] == HCI_SUBEVENT_LE_PHY_UPDATE_COMPLETE) {
        h2_ble_log("H2_JIELI_BLE_PHY_UPDATE status=%u handle=%u tx=%u rx=%u\r\n",
               packet[3], (unsigned)(packet[4] | (packet[5] << 8u)),
               packet[6], packet[7]);
      }
      const uint8_t subevent = hci_event_le_meta_get_subevent_code(packet);
      if (subevent == HCI_SUBEVENT_LE_CONNECTION_COMPLETE ||
          subevent == HCI_SUBEVENT_LE_ENHANCED_CONNECTION_COMPLETE) {
        const uint8_t status = subevent == HCI_SUBEVENT_LE_CONNECTION_COMPLETE
            ? hci_subevent_le_connection_complete_get_status(packet)
            : hci_subevent_le_enhanced_connection_complete_get_status(packet);
        const uint16_t interval =
            subevent == HCI_SUBEVENT_LE_CONNECTION_COMPLETE
                ? hci_subevent_le_connection_complete_get_conn_interval(packet)
                : hci_subevent_le_enhanced_connection_complete_get_conn_interval(packet);
        const uint16_t latency =
            subevent == HCI_SUBEVENT_LE_CONNECTION_COMPLETE
                ? hci_subevent_le_connection_complete_get_conn_latency(packet)
                : hci_subevent_le_enhanced_connection_complete_get_conn_latency(packet);
        const uint16_t supervision_timeout =
            subevent == HCI_SUBEVENT_LE_CONNECTION_COMPLETE
                ? hci_subevent_le_connection_complete_get_supervision_timeout(packet)
                : hci_subevent_le_enhanced_connection_complete_get_supervision_timeout(packet);
        /* Keep this in one log record so the UART Loader framing cannot split the
         * line while diagnosing the controller connection result. */
        h2_ble_log(
            "H2_JIELI_BLE_CONNECT subevent=%u status=%u interval=%u "
            "latency=%u timeout=%u\r\n",
            (unsigned)subevent, (unsigned)status, (unsigned)interval,
            (unsigned)latency, (unsigned)supervision_timeout);
        if (status != 0u) {
          h2_gatt_lock();
          h2_ble.adv.started = 0;
          h2_gatt_unlock();
          h2_restart_legacy_advertising();
          break;
        }
        uint16_t handle = subevent == HCI_SUBEVENT_LE_CONNECTION_COMPLETE
            ? hci_subevent_le_connection_complete_get_connection_handle(packet)
            : hci_subevent_le_enhanced_connection_complete_get_connection_handle(packet);
        /* Legacy connectable advertising stops automatically on connection. */
        h2_gatt_lock();
        h2_ble.adv.started = 0;
        h2_ble.conn_handle = handle;
        h2_ble.mtu = 23u;
        h2_gatt_unlock();
        h2_ble_log(
            "H2_JIELI_BLE_LINK_PARAMS interval=%u latency=%u timeout=%u\r\n",
            (unsigned)interval, (unsigned)latency,
            (unsigned)supervision_timeout);
        h2_ble_log("H2_JIELI_BLE_CONNECT_ENTER step=att_send_init handle=%u\r\n",
               (unsigned)handle);
        const int att_init_result = ble_op_att_send_init(
            handle, h2_att_buffer, sizeof(h2_att_buffer), H2_JIELI_ATT_MTU);
        if (h2_ble_cmd_result(att_init_result) != H2_PAL_OK) {
          /* Keep the physical handle until the disconnect callback, but do
           * not expose an unusable ATT transport as a connected PAL link. */
          h2_gatt_lock();
          if (h2_ble.conn_handle == handle) h2_ble.mtu = 0u;
          h2_gatt_unlock();
          const int disconnect_result = ble_op_disconnect(handle);
          h2_ble_log(
              "H2_JIELI_BLE_CONNECT_ERROR step=att_send_init handle=%u "
              "vendor=%d pal=%d disconnect=%d\r\n",
              (unsigned)handle, att_init_result,
              h2_ble_cmd_result(att_init_result), disconnect_result);
          break;
        }
        h2_ble_log(
            "H2_JIELI_BLE_CONNECT_OK step=att_send_init handle=%u "
            "vendor=%d pal=%d\r\n",
            (unsigned)handle, att_init_result,
            h2_ble_cmd_result(att_init_result));
        const h2_pal_ble_connection_t connection = {
            .conn_handle = handle,
            .role = H2_PAL_BLE_ROLE_PERIPHERAL,
            .mtu = 23u,
        };
        h2_ble_log("H2_JIELI_BLE_CONNECT_ENTER step=post handle=%u\r\n",
               (unsigned)handle);
        h2_ble_post(H2_PAL_SYSTEM_EVENT_TYPE_BLE_CONNECTED,
                    &connection, sizeof(connection));
        h2_ble_log("H2_JIELI_BLE_CONNECT_OK step=post handle=%u\r\n",
               (unsigned)handle);
      } else if (subevent == HCI_SUBEVENT_LE_CONNECTION_UPDATE_COMPLETE) {
        const h2_pal_ble_connection_params_t params = {
            .interval_min_ms = (uint16_t)(
                hci_subevent_le_connection_update_complete_get_conn_interval(packet) * 5u / 4u),
            .interval_max_ms = (uint16_t)(
                hci_subevent_le_connection_update_complete_get_conn_interval(packet) * 5u / 4u),
            .latency = hci_subevent_le_connection_update_complete_get_conn_latency(packet),
            .supervision_timeout_ms = (uint16_t)(
                hci_subevent_le_connection_update_complete_get_supervision_timeout(packet) * 10u),
        };
        h2_ble_post(H2_PAL_SYSTEM_EVENT_TYPE_BLE_CONNECTION_UPDATED,
                    &params, sizeof(params));
      }
      break;
    }
    case HCI_EVENT_DISCONNECTION_COMPLETE: {
      /* Pinned hci_event_handler reports payload length + 1, although the
       * controller queues payload length + 2 bytes (including both headers).
       * The four-byte disconnect payload therefore arrives with size == 5. */
      if (size < 5u || packet[1] < 4u || packet[2] != 0u) break;
      const uint16_t handle =
          hci_event_disconnection_complete_get_connection_handle(packet);
      h2_gatt_lock();
      if (handle == 0u || handle != h2_ble.conn_handle) {
        h2_gatt_unlock();
        break;
      }
      const h2_pal_ble_disconnected_info_t info = {
          .conn_handle = handle,
          .reason = packet[5],
      };
      h2_ble.conn_handle = 0u;
      h2_ble.mtu = 0u;
      h2_gatt_unlock();
      h2_ble_log(
          "H2_JIELI_BLE_DISCONNECT handle=%u reason=%u\r\n",
          (unsigned)info.conn_handle, (unsigned)info.reason);
      h2_att_trace_dump();
      h2_gatt_lock();
      h2_att_trace_count = 0u;
      h2_gatt_unlock();
      (void)ble_op_att_send_init(0u, NULL, 0u, 0u);
      h2_ble_post(H2_PAL_SYSTEM_EVENT_TYPE_BLE_DISCONNECTED,
                  &info, sizeof(info));
      h2_restart_legacy_advertising();
      break;
    }
    case ATT_EVENT_MTU_EXCHANGE_COMPLETE: {
      if (size < 6u) break;
      const uint16_t handle = att_event_mtu_exchange_complete_get_handle(packet);
      const uint16_t mtu = att_event_mtu_exchange_complete_get_MTU(packet);
      if (mtu < 23u || mtu > H2_JIELI_ATT_MTU) break;
      h2_gatt_lock();
      if (handle == 0u || handle != h2_ble.conn_handle) {
        h2_gatt_unlock();
        break;
      }
      h2_ble.mtu = mtu;
      h2_gatt_unlock();
      (void)ble_op_att_set_send_mtu(mtu - H2_PAL_BLE_ATT_HEADER_LEN);
      const h2_pal_ble_mtu_info_t info = {
          .conn_handle = handle, .mtu = mtu};
      h2_ble_post(H2_PAL_SYSTEM_EVENT_TYPE_BLE_MTU_CHANGED,
                  &info, sizeof(info));
      break;
    }
    case HCI_EVENT_NUMBER_OF_COMPLETED_PACKETS:
    case ATT_EVENT_CAN_SEND_NOW:
      /* Per-connection-event data-path signals: logging them from the
       * stack task serializes every packet behind the UART console. */
      break;
    default:
      h2_ble_log("H2_JIELI_BLE_HCI event=0x%02x size=%u\r\n",
             (unsigned)event_type, (unsigned)size);
      break;
  }
}

static uint16_t h2_att_read_retained(
    hci_con_handle_t connection, uint16_t handle, uint16_t offset,
    uint8_t *buffer, uint16_t size) {
  h2_ble_call_t call;
  if (h2_ble_call_begin(&call) != H2_PAL_OK) return 0u;
  const uint16_t result = h2_att_read(connection, handle, offset, buffer, size);
  h2_ble_call_end(&call);
  return result;
}

static int h2_att_write_retained(
    hci_con_handle_t connection, uint16_t handle, uint16_t transaction,
    uint16_t offset, uint8_t *buffer, uint16_t size) {
  h2_ble_call_t call;
  if (h2_ble_call_begin(&call) != H2_PAL_OK) return 0x0e;
  const int result = h2_att_write(connection, handle, transaction, offset, buffer, size);
  h2_ble_call_end(&call);
  return result;
}

static void h2_packet_handler_retained(
    uint8_t type, uint16_t channel, uint8_t *packet, uint16_t size) {
  h2_ble_call_t call;
  if (h2_ble_call_begin(&call) != H2_PAL_OK) return;
  h2_packet_handler(type, channel, packet, size);
  h2_ble_call_end(&call);
}

void ble_profile_init(void) {
  h2_gatt_lock();
  h2_adv_commands.phase = 0u;
  h2_adv_commands.submitting = 0u;
  h2_adv_commands.restart = 0u;
  h2_gatt_unlock();
  h2_ble_log("H2_JIELI_BLE_PROFILE_ENTER step=device_db\r\n");
  le_device_db_init();
  h2_ble_log("H2_JIELI_BLE_PROFILE_OK step=device_db\r\n");
  /* Match JieLi's GATT-server initialization contract: initialize SM even
   * when the application does not proactively request link security. */
  h2_ble_log("H2_JIELI_BLE_PROFILE_ENTER step=security_manager\r\n");
  sm_init();
  sm_set_io_capabilities(IO_CAPABILITY_NO_INPUT_NO_OUTPUT);
  sm_set_authentication_requirements(
      TCFG_BLE_SECURITY_EN ? SM_AUTHREQ_BONDING : 0);
  sm_set_encryption_key_size_range(7u, 16u);
  sm_set_request_security(TCFG_BLE_SECURITY_EN);
  h2_ble_log("H2_JIELI_BLE_SECURITY request=%u bonding=%u\r\n",
         (unsigned)TCFG_BLE_SECURITY_EN, (unsigned)!!TCFG_BLE_SECURITY_EN);
  sm_event_callback_set(h2_packet_handler_retained);
  h2_ble_log("H2_JIELI_BLE_PROFILE_OK step=security_manager\r\n");
  h2_ble_log("H2_JIELI_BLE_PROFILE_ENTER step=att_server\r\n");
  att_server_init(h2_profile_data, h2_att_read_retained, h2_att_write_retained);
  h2_ble_log("H2_JIELI_BLE_PROFILE_OK step=att_server\r\n");
  h2_ble_log("H2_JIELI_BLE_PROFILE_ENTER step=handlers\r\n");
  att_server_register_packet_handler(h2_packet_handler_retained);
  hci_event_callback_set(h2_packet_handler_retained);
  le_l2cap_register_packet_handler(h2_packet_handler_retained);
  h2_ble_log("H2_JIELI_BLE_PROFILE_OK step=handlers\r\n");
  ble_vendor_set_default_att_mtu(H2_JIELI_ATT_MTU);
  h2_ble_log("H2_JIELI_BLE_PROFILE_OK step=mtu\r\n");
}

void bt_ble_init(void) {
  h2_ble_call_t call = {.task = h2_jieli_sdk_task_current()};
  h2_gatt_lock();
  if (!h2_ble.starting) {
    h2_gatt_unlock();
    return;
  }
  if (h2_ble.stopping) {
    h2_ble.starting = 0;
    h2_gatt_unlock();
    return;
  }
  call.next = h2_ble_calls;
  h2_ble_calls = &call;
  h2_gatt_unlock();
  extern u8 get_ble_gatt_role(void);
  const u8 previous_role = get_ble_gatt_role();
  if (previous_role == 1u) {
    ble_stack_gatt_role(0u);
  }
  h2_ble_log("H2_JIELI_BLE_EVENT event=BT_STATUS_INIT_OK\r\n");
  h2_ble_log("H2_JIELI_BLE_ROLE previous=%u active=%u\r\n",
         (unsigned)previous_role, (unsigned)get_ble_gatt_role());
  h2_gatt_lock();
  h2_ble.starting = 0;
  if (h2_ble.stopping) {
    h2_gatt_unlock();
    h2_ble_call_end(&call);
    return;
  }
  h2_ble.started = 1;
  h2_gatt_unlock();
  h2_ble_post(H2_PAL_SYSTEM_EVENT_TYPE_BLE_HOST_STARTED, NULL, 0u);
  h2_gatt_lock();
  const int pending_adv = h2_ble.adv.used && h2_ble.adv.start_requested &&
                          !h2_ble.adv.started;
  h2_gatt_unlock();
  if (pending_adv) {
    const int rc = h2_adv_apply(&h2_ble.adv);
    const h2_pal_ble_adv_set_event_t event = {
        .set = &h2_ble.adv, .status = rc};
    h2_ble_post(H2_PAL_SYSTEM_EVENT_TYPE_BLE_ADVERTISING_STARTED,
                &event, sizeof(event));
  }
  h2_ble_call_end(&call);
}

int h2_jieli_ac791n_devkit_ble_bt_event_handler(struct sys_event *event) {
  if (event == NULL || event->from != BT_EVENT_FROM_CON) return 0;
  const struct bt_event *bt = (const struct bt_event *)event->payload;
  h2_ble_log("H2_JIELI_BLE_BT_EVENT event=%u value=%u\r\n",
         (unsigned)bt->event, (unsigned)bt->value);
  if (bt->event == BT_STATUS_INIT_OK) bt_ble_init();
  return 0;
}

/* Public operations retain the host through SDK calls and subscriber posts. */
static int h2_ble_start_retained(void *user) {
  h2_ble_call_t call;
  int rc = h2_ble_call_begin(&call);
  if (rc != H2_PAL_OK) return rc;
  rc = h2_ble_start(user);
  h2_ble_call_end(&call);
  return rc;
}

static int h2_legacy_set_adv_data_retained(void *user, const h2_pal_ble_adv_data_t *data) {
  h2_ble_call_t call;
  int rc = h2_ble_call_begin(&call);
  if (rc != H2_PAL_OK) return rc;
  rc = h2_legacy_set_adv_data(user, data);
  h2_ble_call_end(&call);
  return rc;
}

static int h2_legacy_start_advertising_retained(void *user, const h2_pal_ble_adv_params_t *params) {
  h2_ble_call_t call;
  int rc = h2_ble_call_begin(&call);
  if (rc != H2_PAL_OK) return rc;
  rc = h2_legacy_start_advertising(user, params);
  h2_ble_call_end(&call);
  return rc;
}

static int h2_legacy_stop_advertising_retained(void *user) {
  h2_ble_call_t call;
  int rc = h2_ble_call_begin(&call);
  if (rc != H2_PAL_OK) return rc;
  rc = h2_legacy_stop_advertising(user);
  h2_ble_call_end(&call);
  return rc;
}

static int h2_adv_set_create_retained(void *user, const h2_pal_ble_adv_params_t *params, h2_pal_ble_adv_set_t **out_set) {
  h2_ble_call_t call;
  int rc = h2_ble_call_begin(&call);
  if (rc != H2_PAL_OK) return rc;
  rc = h2_adv_set_create(user, params, out_set);
  h2_ble_call_end(&call);
  return rc;
}

static int h2_adv_set_data_retained(void *user, h2_pal_ble_adv_set_t *set, const h2_pal_ble_adv_data_t *data) {
  h2_ble_call_t call;
  int rc = h2_ble_call_begin(&call);
  if (rc != H2_PAL_OK) return rc;
  rc = h2_adv_set_data(user, set, data);
  h2_ble_call_end(&call);
  return rc;
}

static int h2_adv_set_start_retained(void *user, h2_pal_ble_adv_set_t *set) {
  h2_ble_call_t call;
  int rc = h2_ble_call_begin(&call);
  if (rc != H2_PAL_OK) return rc;
  rc = h2_adv_set_start(user, set);
  h2_ble_call_end(&call);
  return rc;
}

static int h2_adv_set_stop_retained(void *user, h2_pal_ble_adv_set_t *set) {
  h2_ble_call_t call;
  int rc = h2_ble_call_begin(&call);
  if (rc != H2_PAL_OK) return rc;
  rc = h2_adv_set_stop(user, set);
  h2_ble_call_end(&call);
  return rc;
}

static int h2_adv_set_destroy_retained(void *user, h2_pal_ble_adv_set_t *set) {
  h2_ble_call_t call;
  int rc = h2_ble_call_begin(&call);
  if (rc != H2_PAL_OK) return rc;
  rc = h2_adv_set_destroy(user, set);
  h2_ble_call_end(&call);
  return rc;
}

static int h2_register_gatt_retained(void *user, const h2_pal_ble_gatt_service_t *services, size_t count) {
  h2_ble_call_t call;
  int rc = h2_ble_call_begin(&call);
  if (rc != H2_PAL_OK) return rc;
  rc = h2_register_gatt(user, services, count);
  h2_ble_call_end(&call);
  return rc;
}

static int h2_unregister_gatt_retained(void *user) {
  h2_ble_call_t call;
  int rc = h2_ble_call_begin(&call);
  if (rc != H2_PAL_OK) return rc;
  rc = h2_unregister_gatt(user);
  h2_ble_call_end(&call);
  return rc;
}

static int h2_notify_retained(void *user, uint16_t connection, uint16_t attribute, const uint8_t *data, size_t length) {
  h2_ble_call_t call;
  int rc = h2_ble_call_begin(&call);
  if (rc != H2_PAL_OK) return rc;
  rc = h2_notify(user, connection, attribute, data, length);
  h2_ble_call_end(&call);
  return rc;
}

static int h2_disconnect_retained(void *user, uint16_t connection) {
  h2_ble_call_t call;
  int rc = h2_ble_call_begin(&call);
  if (rc != H2_PAL_OK) return rc;
  rc = h2_disconnect(user, connection);
  h2_ble_call_end(&call);
  return rc;
}

static int h2_update_connection_retained(void *user, uint16_t connection, const h2_pal_ble_connection_params_t *params) {
  h2_ble_call_t call;
  int rc = h2_ble_call_begin(&call);
  if (rc != H2_PAL_OK) return rc;
  rc = h2_update_connection(user, connection, params);
  h2_ble_call_end(&call);
  return rc;
}

static int h2_exchange_mtu_retained(void *user, uint16_t connection, uint16_t *mtu, uint32_t timeout) {
  h2_ble_call_t call;
  int rc = h2_ble_call_begin(&call);
  if (rc != H2_PAL_OK) return rc;
  rc = h2_exchange_mtu(user, connection, mtu, timeout);
  h2_ble_call_end(&call);
  return rc;
}

static int h2_set_phy_retained(void *user, uint16_t connection, h2_pal_ble_phy_t tx, h2_pal_ble_phy_t rx, uint32_t timeout) {
  h2_ble_call_t call;
  int rc = h2_ble_call_begin(&call);
  if (rc != H2_PAL_OK) return rc;
  rc = h2_set_phy(user, connection, tx, rx, timeout);
  h2_ble_call_end(&call);
  return rc;
}

const h2_pal_ble_host_api_t *h2_jieli_ac791n_devkit_ble_host_api(const h2_pal_log_api_t *log) {
  if (!h2_ble_log_bind(log)) return NULL;
  static const h2_pal_ble_vtable_t vtable = {
      .start = h2_ble_start_retained,
      .stop = h2_ble_stop,
      .set_adv_data = h2_legacy_set_adv_data_retained,
      .start_advertising = h2_legacy_start_advertising_retained,
      .stop_advertising = h2_legacy_stop_advertising_retained,
      .adv_set_create = h2_adv_set_create_retained,
      .adv_set_set_data = h2_adv_set_data_retained,
      .adv_set_start = h2_adv_set_start_retained,
      .adv_set_stop = h2_adv_set_stop_retained,
      .adv_set_destroy = h2_adv_set_destroy_retained,
      .register_gatt_services = h2_register_gatt_retained,
      .unregister_gatt_services = h2_unregister_gatt_retained,
      .notify = h2_notify_retained,
      .disconnect = h2_disconnect_retained,
      .update_connection = h2_update_connection_retained,
      .exchange_mtu = h2_exchange_mtu_retained,
      .set_preferred_phy = h2_set_phy_retained,
  };
  static const h2_pal_ble_host_api_t api = {
      .user = NULL,
      .vtable = &vtable,
      .allocator = NULL,
  };
  return &api;
}

#else

#include "h2_jieli_ac791n_devkit.h"
#include "h2/pal/h2_pal_unsupported.h"

const h2_pal_ble_host_api_t *h2_jieli_ac791n_devkit_ble_host_api(const h2_pal_log_api_t *log) {
  (void)log;
  return h2_pal_unsupported_ble_host_api();
}

#endif
