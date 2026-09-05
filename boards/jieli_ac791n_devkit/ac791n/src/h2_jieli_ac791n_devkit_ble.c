#include "app_config.h"

#ifdef H2_JIELI_BLE_ENABLE

#include "system/includes.h"
#include "bt_common.h"
#include "btcontroller_config.h"
#include "btstack/avctp_user.h"
#include "btstack/btstack_task.h"
#include "btstack/le/att.h"
#include "btstack/le/ble_api.h"
#include "btstack/le/le_common_define.h"
#include "btstack/le/le_user.h"
#include "btstack/le/sm.h"
#include "event/bt_event.h"
#include "syscfg/syscfg_id.h"

#include "h2_jieli_ac791n_devkit.h"
#include "h2_jieli_wl82_platform_core.h"

#include <string.h>

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
  /* Match JieLi's supported GATT-server configuration.  The SDK reference
   * peripheral uses a 200-byte local MTU and a 512-byte ATT send cbuf. */
  H2_JIELI_ATT_MTU = 200,
  H2_JIELI_ATT_BUFFER_SIZE = ATT_CTRL_BLOCK_SIZE + H2_JIELI_ATT_MTU + 512,
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
  uint16_t conn_handle;
  uint16_t mtu;
  h2_pal_ble_gatt_characteristic_t characteristics[2];
  int gatt_registered;
  struct h2_pal_ble_adv_set adv;
  struct conn_update_param_t conn_params;
} h2_jieli_ble_state_t;

static h2_jieli_ble_state_t h2_ble;
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
}

static void h2_att_trace_dump(void) {
  const uint8_t count = h2_att_trace_count < 16u ? h2_att_trace_count : 16u;
  const uint8_t start = h2_att_trace_count <= 16u
                            ? 0u
                            : (uint8_t)(h2_att_trace_count % 16u);
  printf("H2_JIELI_ATT_TRACE count=%u\r\n", (unsigned)h2_att_trace_count);
  for (uint8_t i = 0u; i < count; ++i) {
    const h2_jieli_att_trace_t *entry =
        &h2_att_trace[(uint8_t)((start + i) % 16u)];
    printf("H2_JIELI_ATT_ACCESS kind=%c handle=%u offset=%u size=%u buffer=%u\r\n",
           entry->kind == 0u ? 'R' : 'W', (unsigned)entry->handle,
           (unsigned)entry->offset, (unsigned)entry->size,
           (unsigned)entry->has_buffer);
  }
}

static int h2_adv_set_stop(void *user, h2_pal_ble_adv_set_t *set);
static int h2_adv_apply(struct h2_pal_ble_adv_set *set);

static void h2_restart_legacy_advertising(void) {
  if (!h2_ble.started || !h2_ble.adv.used ||
      !h2_ble.adv.start_requested ||
      h2_ble.adv.params.type != H2_PAL_BLE_ADV_TYPE_LEGACY) {
    return;
  }
  const int rc = h2_adv_apply(&h2_ble.adv);
  h2_ble.adv.started = rc == H2_PAL_OK;
  printf("H2_JIELI_BLE_ADV_RESTART code=%d\r\n", rc);
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
    printf("H2_JIELI_BLE_EVENT_POST type=%u code=%d\r\n",
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
  printf("H2_JIELI_BLE_VENDOR op=%s vendor=%d pal=%d\r\n",
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

static int h2_encode_adv(
    const h2_pal_ble_adv_data_t *data, uint8_t *out, size_t capacity,
    uint8_t *out_len) {
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
    if ((uuid_len != 2u && uuid_len != 16u) ||
        data->service_data_uuid.data == NULL ||
        uuid_len + data->service_data.len > sizeof(service) ||
        (data->service_data.len != 0u && data->service_data.data == NULL))
      return H2_PAL_ERR_INVALID_ARG;
    memcpy(service, data->service_data_uuid.data, uuid_len);
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

static int h2_adv_apply(struct h2_pal_ble_adv_set *set) {
  if (set->params.type == H2_PAL_BLE_ADV_TYPE_LEGACY) {
    if (set->data_len > H2_PAL_BLE_LEGACY_ADV_DATA_MAX_LEN)
      return H2_PAL_ERR_NO_SPACE;
    printf("H2_JIELI_BLE_ADV_ENTER step=params\r\n");
    uint16_t interval_units =
        (uint16_t)((set->params.interval_min_ms * 8u) / 5u);
    printf("H2_JIELI_BLE_ADV_INTERVAL units=%u\r\n",
           (unsigned)interval_units);
    int rc = h2_ble_cmd_trace("set_adv_param", ble_op_set_adv_param(
        interval_units,
        set->params.mode == H2_PAL_BLE_ADV_MODE_CONNECTABLE ? ADV_IND
                                                            : ADV_NONCONN_IND,
        ADV_CHANNEL_ALL));
    printf("H2_JIELI_BLE_ADV_RETURN step=params code=%d\r\n", rc);
    if (rc == H2_PAL_OK) {
      printf("H2_JIELI_BLE_ADV_ENTER step=data\r\n");
      rc = h2_ble_cmd_trace(
          "set_adv_data", ble_op_set_adv_data(set->data_len, set->data));
      printf("H2_JIELI_BLE_ADV_RETURN step=data code=%d\r\n", rc);
    }
    if (rc == H2_PAL_OK) {
      printf("H2_JIELI_BLE_ADV_ENTER step=response\r\n");
      rc = h2_ble_cmd_trace(
          "set_rsp_data", ble_op_set_rsp_data(
              set->scan_response_data_len, set->scan_response_data));
      printf("H2_JIELI_BLE_ADV_RETURN step=response code=%d\r\n", rc);
    }
    if (rc == H2_PAL_OK) {
      printf("H2_JIELI_BLE_ADV_ENTER step=enable\r\n");
      rc = h2_ble_cmd_trace("adv_enable", ble_op_adv_enable(1));
      printf("H2_JIELI_BLE_ADV_RETURN step=enable code=%d\r\n", rc);
    }
    return rc;
  }
  struct h2_ext_adv_param params;
  memset(&params, 0, sizeof(params));
  params.properties =
      set->params.mode == H2_PAL_BLE_ADV_MODE_CONNECTABLE ? 1u : 0u;
  h2_u24(params.interval_min,
         (set->params.interval_min_ms * 8u + 4u) / 5u);
  h2_u24(params.interval_max,
         (set->params.interval_max_ms * 8u + 4u) / 5u);
  params.channel_map = 7u;
  params.primary_phy = h2_adv_phy(set->params.primary_phy);
  params.secondary_phy = h2_adv_phy(set->params.secondary_phy);
  params.sid = set->params.sid;
  struct h2_ext_adv_data encoded = {
      .handle = 0u, .operation = 3u, .fragment_preference = 0u,
      .length = set->data_len,
  };
  memcpy(encoded.data, set->data, set->data_len);
  struct h2_ext_adv_enable enable = {
      .enable = 1u, .number_of_sets = 1u, .handle = 0u,
      .duration = (uint16_t)(set->params.duration_ms / 10u),
      .max_events = set->params.max_adv_events,
  };
  int rc = h2_ble_cmd_result(ble_op_set_ext_adv_param(&params, sizeof(params)));
  if (rc == H2_PAL_OK)
    rc = h2_ble_cmd_result(ble_op_set_ext_adv_data(
        &encoded, (uint16_t)(4u + encoded.length)));
  if (rc == H2_PAL_OK)
    rc = h2_ble_cmd_result(ble_op_set_ext_adv_enable(&enable, sizeof(enable)));
  return rc;
}

static int h2_legacy_set_adv_data(
    void *user, const h2_pal_ble_adv_data_t *data) {
  (void)user;
  if (data == NULL) return H2_PAL_ERR_INVALID_ARG;
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
  uint8_t primary_len = 0u;
  int rc = h2_encode_adv(
      &primary, h2_ble.adv.data, H2_PAL_BLE_LEGACY_ADV_DATA_MAX_LEN,
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
          h2_ble.adv.scan_response_data,
          sizeof(h2_ble.adv.scan_response_data), &scan_response_len, type,
          uuid->data, uuid->len);
      if (rc != H2_PAL_OK) return rc;
    }
  }
  if (data->local_name != NULL) {
    rc = h2_adv_append(
        h2_ble.adv.scan_response_data,
        sizeof(h2_ble.adv.scan_response_data), &scan_response_len, 0x09u,
        (const uint8_t *)data->local_name, strlen(data->local_name));
    if (rc != H2_PAL_OK) return rc;
  }
  h2_ble.adv.data_len = primary_len;
  h2_ble.adv.scan_response_data_len = (uint8_t)scan_response_len;
  printf("H2_JIELI_BLE_ADV_LAYOUT primary=%u response=%u identity=%s\r\n",
         (unsigned)primary_len, (unsigned)scan_response_len,
         data->manufacturer_data.len != 0u ? "primary" : "none");
  h2_ble.adv.used = 1;
  return H2_PAL_OK;
}

static int h2_legacy_start_advertising(
    void *user, const h2_pal_ble_adv_params_t *params) {
  (void)user;
  if (params == NULL || params->type != H2_PAL_BLE_ADV_TYPE_LEGACY ||
      !h2_ble.adv.used) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  h2_ble.adv.params = *params;
  h2_ble.adv.start_requested = 1;
  if (!h2_ble.started) return h2_ble.starting ? H2_PAL_OK
                                              : H2_PAL_ERR_INVALID_STATE;
  const int rc = h2_adv_apply(&h2_ble.adv);
  if (rc == H2_PAL_OK) h2_ble.adv.started = 1;
  return rc;
}

static int h2_legacy_stop_advertising(void *user) {
  if (!h2_ble.adv.used) return H2_PAL_ERR_INVALID_STATE;
  return h2_adv_set_stop(user, &h2_ble.adv);
}

static int h2_ble_start(void *user) {
  (void)user;
  extern const uint64_t config_btctler_le_features;
  printf("H2_JIELI_BLE_FEATURES high=%08x low=%08x\r\n",
         (unsigned)(config_btctler_le_features >> 32u),
         (unsigned)config_btctler_le_features);
  if (h2_ble.started || h2_ble.starting) return H2_PAL_OK;
  h2_ble.starting = 1;
  printf("H2_JIELI_BLE_ENTER step=controller_prepare\r\n");
  /* JieLi's BLE-only reference applications disable Classic-BT sniff before
   * configuring the controller address and starting btstack.  Keep that SDK
   * ordering even though this PAL exposes BLE only. */
  extern void lmp_set_sniff_disable(void);
  lmp_set_sniff_disable();
  printf("H2_JIELI_BLE_OK step=controller_prepare\r\n");
  uint8_t ble_addr[6];
  const uint8_t *base_addr;
  extern void lib_make_ble_address(uint8_t *ble_address, uint8_t *edr_address);
  extern int le_controller_set_mac(void *addr);
  printf("H2_JIELI_BLE_ENTER step=base_mac\r\n");
  base_addr = h2_ble_base_mac();
  lib_make_ble_address(ble_addr, (uint8_t *)base_addr);
  printf(
      "H2_JIELI_BLE_OK step=base_mac "
      "base=%02x:%02x:%02x:%02x:%02x:%02x "
      "ble=%02x:%02x:%02x:%02x:%02x:%02x\r\n",
      base_addr[5], base_addr[4], base_addr[3], base_addr[2], base_addr[1],
      base_addr[0], ble_addr[5], ble_addr[4], ble_addr[3], ble_addr[2],
      ble_addr[1], ble_addr[0]);
  printf("H2_JIELI_BLE_ENTER step=controller_mac\r\n");
  const int mac_result = le_controller_set_mac(ble_addr);
  printf("H2_JIELI_BLE_OK step=controller_mac vendor=%d\r\n", mac_result);
  printf("H2_JIELI_BLE_ENTER step=btstack_init\r\n");
  const int btstack_result = btstack_init();
  if (btstack_result == 0) {
    printf("H2_JIELI_BLE_OK step=btstack_init vendor=%d\r\n",
           btstack_result);
    return H2_PAL_OK;
  }
  printf("H2_JIELI_BLE_ERROR step=btstack_init vendor=%d\r\n",
         btstack_result);
  h2_ble.starting = 0;
  return H2_PAL_ERR_IO;
}

static int h2_ble_stop(void *user) {
  (void)user;
  if (!h2_ble.started && !h2_ble.starting) return H2_PAL_OK;
  if (h2_ble.adv.used) (void)h2_adv_set_stop(user, &h2_ble.adv);
  if (h2_ble.conn_handle != 0u) (void)ble_op_disconnect(h2_ble.conn_handle);
  (void)ble_user_cmd_prepare(BLE_CMD_STACK_EXIT, 0);
  memset(&h2_ble, 0, sizeof(h2_ble));
  h2_ble_post(H2_PAL_SYSTEM_EVENT_TYPE_BLE_HOST_STOPPED, NULL, 0u);
  return H2_PAL_OK;
}

static int h2_adv_set_create(
    void *user, const h2_pal_ble_adv_params_t *params,
    h2_pal_ble_adv_set_t **out_set) {
  (void)user;
  if (!h2_ble.started && !h2_ble.starting)
    return H2_PAL_ERR_INVALID_STATE;
  if (h2_ble.adv.used) return H2_PAL_ERR_FULL;
  memset(&h2_ble.adv, 0, sizeof(h2_ble.adv));
  h2_ble.adv.params = *params;
  h2_ble.adv.used = 1;
  *out_set = &h2_ble.adv;
  return H2_PAL_OK;
}

static int h2_adv_set_data(
    void *user, h2_pal_ble_adv_set_t *set,
    const h2_pal_ble_adv_data_t *data) {
  (void)user;
  if (set != &h2_ble.adv || !set->used) return H2_PAL_ERR_INVALID_ARG;
  return h2_encode_adv(data, set->data, sizeof(set->data), &set->data_len);
}

static int h2_adv_set_start(void *user, h2_pal_ble_adv_set_t *set) {
  (void)user;
  if (set != &h2_ble.adv || !set->used ||
      (!h2_ble.started && !h2_ble.starting))
    return H2_PAL_ERR_INVALID_STATE;
  set->start_requested = 1;
  if (!h2_ble.started) return H2_PAL_OK;
  int rc = h2_adv_apply(set);
  if (rc == H2_PAL_OK) set->started = 1;
  const h2_pal_ble_adv_set_event_t event = {.set = set, .status = rc};
  h2_ble_post(H2_PAL_SYSTEM_EVENT_TYPE_BLE_ADVERTISING_STARTED,
              &event, sizeof(event));
  return rc;
}

static int h2_adv_set_stop(void *user, h2_pal_ble_adv_set_t *set) {
  (void)user;
  if (set != &h2_ble.adv || !set->used) return H2_PAL_ERR_INVALID_ARG;
  int rc = H2_PAL_OK;
  set->start_requested = 0;
  if (set->started) {
    if (set->params.type == H2_PAL_BLE_ADV_TYPE_EXTENDED) {
      const struct h2_ext_adv_enable disable = {
          .enable = 0u, .number_of_sets = 1u, .handle = 0u};
      rc = h2_ble_cmd_result(
          ble_op_set_ext_adv_enable(&disable, sizeof(disable)));
    } else {
      rc = h2_ble_cmd_result(ble_op_adv_enable(0));
    }
    if (rc == H2_PAL_OK) set->started = 0;
  }
  const h2_pal_ble_adv_set_event_t event = {.set = set, .status = rc};
  h2_ble_post(H2_PAL_SYSTEM_EVENT_TYPE_BLE_ADVERTISING_STOPPED,
              &event, sizeof(event));
  return rc;
}

static int h2_adv_set_destroy(void *user, h2_pal_ble_adv_set_t *set) {
  int rc = h2_adv_set_stop(user, set);
  if (rc == H2_PAL_OK) memset(set, 0, sizeof(*set));
  return rc;
}

static int h2_register_gatt(
    void *user, const h2_pal_ble_gatt_service_t *services, size_t count) {
  (void)user;
  if (count != 1u || services == NULL || !services[0].primary ||
      services[0].characteristic_count != 2u ||
      !h2_uuid_equal(&services[0].uuid, h2_service_uuid) ||
      !h2_uuid_equal(&services[0].characteristics[0].uuid, h2_tx_uuid) ||
      !h2_uuid_equal(&services[0].characteristics[1].uuid, h2_rx_uuid))
    return H2_PAL_ERR_UNSUPPORTED;
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
  return H2_PAL_OK;
}

static int h2_unregister_gatt(void *user) {
  (void)user;
  h2_ble.gatt_registered = 0;
  memset(h2_ble.characteristics, 0, sizeof(h2_ble.characteristics));
  return H2_PAL_OK;
}

static int h2_notify(
    void *user, uint16_t conn_handle, uint16_t attr_handle,
    const uint8_t *data, size_t len) {
  (void)user;
  if (conn_handle != h2_ble.conn_handle ||
      attr_handle != H2_JIELI_GATT_TX_VALUE_HANDLE ||
      (len != 0u && data == NULL) || len + 3u > h2_ble.mtu)
    return H2_PAL_ERR_INVALID_ARG;
  return h2_ble_cmd_result(ble_op_att_send_data(
      attr_handle, data, (uint16_t)len, ATT_OP_AUTO_READ_CCC));
}

static int h2_disconnect(void *user, uint16_t conn_handle) {
  (void)user;
  if (conn_handle == 0u || conn_handle != h2_ble.conn_handle)
    return H2_PAL_ERR_INVALID_ARG;
  return h2_ble_cmd_result(ble_op_disconnect(conn_handle));
}

static int h2_update_connection(
    void *user, uint16_t conn_handle,
    const h2_pal_ble_connection_params_t *params) {
  (void)user;
  if (conn_handle != h2_ble.conn_handle || params == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  /* Keep the central-selected parameters.  JieLi's peripheral examples use
   * a 20-30 ms request range; forcing the Loader's exact 15 ms request here
   * makes CoreBluetooth lose link synchronisation before ATT discovery. */
  printf(
      "H2_JIELI_BLE_CONN_PARAMS keep-central requested=%u-%u latency=%u "
      "timeout=%u\r\n",
      (unsigned)params->interval_min_ms,
      (unsigned)params->interval_max_ms,
      (unsigned)params->latency,
      (unsigned)params->supervision_timeout_ms);
  return H2_PAL_ERR_UNSUPPORTED;
}

static int h2_exchange_mtu(
    void *user, uint16_t conn_handle, uint16_t *out_mtu,
    uint32_t timeout_ms) {
  (void)user;
  (void)timeout_ms;
  if (conn_handle != h2_ble.conn_handle || out_mtu == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  *out_mtu = h2_ble.mtu;
  return H2_PAL_OK;
}

static uint8_t h2_conn_phy(h2_pal_ble_phy_t phy) {
  if (phy == H2_PAL_BLE_PHY_2M) return CONN_SET_2M_PHY;
  if (phy == H2_PAL_BLE_PHY_CODED) return CONN_SET_CODED_PHY;
  return CONN_SET_1M_PHY;
}

static int h2_set_phy(
    void *user, uint16_t conn_handle, h2_pal_ble_phy_t tx_phy,
    h2_pal_ble_phy_t rx_phy, uint32_t timeout_ms) {
  (void)user;
  (void)timeout_ms;
  if (conn_handle != h2_ble.conn_handle) return H2_PAL_ERR_INVALID_ARG;
  return h2_ble_cmd_result(ble_op_set_ext_phy(
      conn_handle, 0u, h2_conn_phy(tx_phy), h2_conn_phy(rx_phy),
      CONN_SET_PHY_OPTIONS_NONE));
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
  if (buffer != NULL && offset == 0u && buffer_size >= 2u) {
    buffer[0] = att_get_ccc_config(handle);
    buffer[1] = 0u;
  }
  return 2u;
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
  if (handle == H2_JIELI_GATT_RX_VALUE_HANDLE && h2_ble.gatt_registered) {
    const h2_pal_ble_gatt_characteristic_t *rx = &h2_ble.characteristics[1];
    if (rx->write == NULL) return 0;
    const h2_pal_ble_gatt_access_t access = {
        .conn_handle = connection_handle,
        .attr_handle = handle,
        .offset = offset,
    };
    /* ATT Error Response: Unlikely Error. */
    return rx->write(rx->user, &access, buffer, buffer_size) == H2_PAL_OK
               ? 0 : 0x0e;
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
      printf(
          "H2_JIELI_BLE_HCI event=command-complete opcode=0x%02x%02x "
          "status=%u size=%u\r\n",
          size > 4u ? (unsigned)packet[4] : 0u,
          size > 3u ? (unsigned)packet[3] : 0u,
          size > 5u ? (unsigned)packet[5] : 0xffu, (unsigned)size);
      break;
    case HCI_EVENT_COMMAND_STATUS:
      printf(
          "H2_JIELI_BLE_HCI event=command-status opcode=0x%02x%02x "
          "status=%u size=%u\r\n",
          size > 5u ? (unsigned)packet[5] : 0u,
          size > 4u ? (unsigned)packet[4] : 0u,
          size > 2u ? (unsigned)packet[2] : 0xffu, (unsigned)size);
      break;
    case HCI_EVENT_HARDWARE_ERROR:
      printf("H2_JIELI_BLE_HCI event=hardware-error code=%u size=%u\r\n",
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
      printf("H2_JIELI_BLE_SECURITY just-works-confirm handle=%u\r\n",
             (unsigned)handle);
      break;
    }
    case HCI_EVENT_LE_META: {
      if (size >= 8u && packet[2] == HCI_SUBEVENT_LE_PHY_UPDATE_COMPLETE) {
        printf("H2_JIELI_BLE_PHY_UPDATE status=%u handle=%u tx=%u rx=%u\r\n",
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
        /* Keep this in one printf so the UART Loader framing cannot split the
         * line while diagnosing the controller connection result. */
        printf(
            "H2_JIELI_BLE_CONNECT subevent=%u status=%u interval=%u "
            "latency=%u timeout=%u\r\n",
            (unsigned)subevent, (unsigned)status, (unsigned)interval,
            (unsigned)latency, (unsigned)supervision_timeout);
        if (status != 0u) {
          h2_ble.adv.started = 0;
          h2_restart_legacy_advertising();
          break;
        }
        uint16_t handle = subevent == HCI_SUBEVENT_LE_CONNECTION_COMPLETE
            ? hci_subevent_le_connection_complete_get_connection_handle(packet)
            : hci_subevent_le_enhanced_connection_complete_get_connection_handle(packet);
        /* Legacy connectable advertising stops automatically on connection. */
        h2_ble.adv.started = 0;
        h2_ble.conn_handle = handle;
        h2_ble.mtu = 23u;
        printf(
            "H2_JIELI_BLE_LINK_PARAMS interval=%u latency=%u timeout=%u\r\n",
            (unsigned)interval, (unsigned)latency,
            (unsigned)supervision_timeout);
        printf("H2_JIELI_BLE_CONNECT_ENTER step=att_send_init handle=%u\r\n",
               (unsigned)handle);
        const int att_init_result = ble_op_att_send_init(
            handle, h2_att_buffer, sizeof(h2_att_buffer), H2_JIELI_ATT_MTU);
        if (h2_ble_cmd_result(att_init_result) != H2_PAL_OK) {
          /* Keep the physical handle until the disconnect callback, but do
           * not expose an unusable ATT transport as a connected PAL link. */
          h2_ble.mtu = 0u;
          const int disconnect_result = ble_op_disconnect(handle);
          printf(
              "H2_JIELI_BLE_CONNECT_ERROR step=att_send_init handle=%u "
              "vendor=%d pal=%d disconnect=%d\r\n",
              (unsigned)handle, att_init_result,
              h2_ble_cmd_result(att_init_result), disconnect_result);
          break;
        }
        printf(
            "H2_JIELI_BLE_CONNECT_OK step=att_send_init handle=%u "
            "vendor=%d pal=%d\r\n",
            (unsigned)handle, att_init_result,
            h2_ble_cmd_result(att_init_result));
        const h2_pal_ble_connection_t connection = {
            .conn_handle = handle,
            .role = H2_PAL_BLE_ROLE_PERIPHERAL,
            .mtu = 23u,
        };
        printf("H2_JIELI_BLE_CONNECT_ENTER step=post handle=%u\r\n",
               (unsigned)handle);
        h2_ble_post(H2_PAL_SYSTEM_EVENT_TYPE_BLE_CONNECTED,
                    &connection, sizeof(connection));
        printf("H2_JIELI_BLE_CONNECT_OK step=post handle=%u\r\n",
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
      printf(
          "H2_JIELI_BLE_DISCONNECT handle=%u reason=%u\r\n",
          (unsigned)h2_ble.conn_handle, (unsigned)packet[5]);
      h2_att_trace_dump();
      h2_att_trace_count = 0u;
      const h2_pal_ble_disconnected_info_t info = {
          .conn_handle = h2_ble.conn_handle,
          .reason = packet[5],
      };
      h2_ble.conn_handle = 0u;
      h2_ble.mtu = 0u;
      (void)ble_op_att_send_init(0u, NULL, 0u, 0u);
      h2_ble_post(H2_PAL_SYSTEM_EVENT_TYPE_BLE_DISCONNECTED,
                  &info, sizeof(info));
      h2_restart_legacy_advertising();
      break;
    }
    case ATT_EVENT_MTU_EXCHANGE_COMPLETE: {
      h2_ble.mtu = att_event_mtu_exchange_complete_get_MTU(packet);
      (void)ble_op_att_set_send_mtu(h2_ble.mtu - H2_PAL_BLE_ATT_HEADER_LEN);
      const h2_pal_ble_mtu_info_t info = {
          .conn_handle = h2_ble.conn_handle, .mtu = h2_ble.mtu};
      h2_ble_post(H2_PAL_SYSTEM_EVENT_TYPE_BLE_MTU_CHANGED,
                  &info, sizeof(info));
      break;
    }
    default:
      printf("H2_JIELI_BLE_HCI event=0x%02x size=%u\r\n",
             (unsigned)event_type, (unsigned)size);
      break;
  }
}

void ble_profile_init(void) {
  printf("H2_JIELI_BLE_PROFILE_ENTER step=device_db\r\n");
  le_device_db_init();
  printf("H2_JIELI_BLE_PROFILE_OK step=device_db\r\n");
  /* Match JieLi's GATT-server initialization contract: initialize SM even
   * when the application does not proactively request link security. */
  printf("H2_JIELI_BLE_PROFILE_ENTER step=security_manager\r\n");
  sm_init();
  sm_set_io_capabilities(IO_CAPABILITY_NO_INPUT_NO_OUTPUT);
  sm_set_authentication_requirements(
      TCFG_BLE_SECURITY_EN ? SM_AUTHREQ_BONDING : 0);
  sm_set_encryption_key_size_range(7u, 16u);
  sm_set_request_security(TCFG_BLE_SECURITY_EN);
  printf("H2_JIELI_BLE_SECURITY request=%u bonding=%u\r\n",
         (unsigned)TCFG_BLE_SECURITY_EN, (unsigned)!!TCFG_BLE_SECURITY_EN);
  sm_event_callback_set(h2_packet_handler);
  printf("H2_JIELI_BLE_PROFILE_OK step=security_manager\r\n");
  printf("H2_JIELI_BLE_PROFILE_ENTER step=att_server\r\n");
  att_server_init(h2_profile_data, h2_att_read, h2_att_write);
  printf("H2_JIELI_BLE_PROFILE_OK step=att_server\r\n");
  printf("H2_JIELI_BLE_PROFILE_ENTER step=handlers\r\n");
  att_server_register_packet_handler(h2_packet_handler);
  hci_event_callback_set(h2_packet_handler);
  le_l2cap_register_packet_handler(h2_packet_handler);
  printf("H2_JIELI_BLE_PROFILE_OK step=handlers\r\n");
  ble_vendor_set_default_att_mtu(H2_JIELI_ATT_MTU);
  printf("H2_JIELI_BLE_PROFILE_OK step=mtu\r\n");
}

void bt_ble_init(void) {
  extern u8 get_ble_gatt_role(void);
  const u8 previous_role = get_ble_gatt_role();
  if (previous_role == 1u) {
    ble_stack_gatt_role(0u);
  }
  printf("H2_JIELI_BLE_EVENT event=BT_STATUS_INIT_OK\r\n");
  printf("H2_JIELI_BLE_ROLE previous=%u active=%u\r\n",
         (unsigned)previous_role, (unsigned)get_ble_gatt_role());
  h2_ble.starting = 0;
  h2_ble.started = 1;
  h2_ble_post(H2_PAL_SYSTEM_EVENT_TYPE_BLE_HOST_STARTED, NULL, 0u);
  if (h2_ble.adv.used && h2_ble.adv.start_requested &&
      !h2_ble.adv.started) {
    const int rc = h2_adv_apply(&h2_ble.adv);
    if (rc == H2_PAL_OK) h2_ble.adv.started = 1;
    const h2_pal_ble_adv_set_event_t event = {
        .set = &h2_ble.adv, .status = rc};
    h2_ble_post(H2_PAL_SYSTEM_EVENT_TYPE_BLE_ADVERTISING_STARTED,
                &event, sizeof(event));
  }
}

int h2_jieli_ac791n_devkit_ble_bt_event_handler(struct sys_event *event) {
  if (event == NULL || event->from != BT_EVENT_FROM_CON) return 0;
  const struct bt_event *bt = (const struct bt_event *)event->payload;
  printf("H2_JIELI_BLE_BT_EVENT event=%u value=%u\r\n",
         (unsigned)bt->event, (unsigned)bt->value);
  if (bt->event == BT_STATUS_INIT_OK) bt_ble_init();
  return 0;
}

const h2_pal_ble_host_api_t *h2_jieli_ac791n_devkit_ble_host_api(void) {
  static const h2_pal_ble_vtable_t vtable = {
      .start = h2_ble_start,
      .stop = h2_ble_stop,
      .set_adv_data = h2_legacy_set_adv_data,
      .start_advertising = h2_legacy_start_advertising,
      .stop_advertising = h2_legacy_stop_advertising,
      .adv_set_create = h2_adv_set_create,
      .adv_set_set_data = h2_adv_set_data,
      .adv_set_start = h2_adv_set_start,
      .adv_set_stop = h2_adv_set_stop,
      .adv_set_destroy = h2_adv_set_destroy,
      .register_gatt_services = h2_register_gatt,
      .unregister_gatt_services = h2_unregister_gatt,
      .notify = h2_notify,
      .disconnect = h2_disconnect,
      .update_connection = h2_update_connection,
      .exchange_mtu = h2_exchange_mtu,
      .set_preferred_phy = h2_set_phy,
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

const h2_pal_ble_host_api_t *h2_jieli_ac791n_devkit_ble_host_api(void) {
  return h2_pal_unsupported_ble_host_api();
}

#endif
