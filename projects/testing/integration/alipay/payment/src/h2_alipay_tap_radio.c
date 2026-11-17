#include "h2_alipay_tap_radio.h"
#include "h2_atomic.h"

#include <string.h>

struct h2_alipay_tap_radio {
  h2_alipay_tap_radio_config_t config;
  h2_atomic_flag_t lock;
  h2_pal_ble_gatt_characteristic_t characteristic;
  h2_pal_ble_gatt_service_t service;
  h2_pal_ble_adv_set_t* advertising;
  uint16_t value_handle, connection;
  bool registered, started, peer, armed, closed;
  size_t head, count;
  struct {
    uint8_t data[512];
    size_t length;
  } queue[8];
};

static const uint8_t service_uuid[] = {0x02u, 0x38u};
static const uint8_t characteristic_uuid[] = {0x02u, 0x4au};

static void wipe(void* bytes, size_t length) {
  volatile uint8_t* cursor = bytes;
  while (length-- != 0u) *cursor++ = 0u;
}

static int acquire(h2_alipay_tap_radio_t* radio) {
  return h2_atomic_flag_test_and_set(&radio->lock, H2_ATOMIC_ACQUIRE)
             ? H2_PAL_ERR_WOULD_BLOCK
             : H2_PAL_OK;
}

static void release(h2_alipay_tap_radio_t* radio) {
  h2_atomic_flag_clear(&radio->lock, H2_ATOMIC_RELEASE);
}

static int write_value(void* user, const h2_pal_ble_gatt_access_t* access,
                       const uint8_t* data, size_t length) {
  h2_alipay_tap_radio_t* radio = user;
  if (access == NULL || access->offset != 0u || data == NULL || length == 0u ||
      length > sizeof(radio->queue[0].data))
    return H2_PAL_ERR_INVALID_ARG;
  int rc = acquire(radio);
  if (rc != H2_PAL_OK) return rc;
  if (radio->closed || !radio->armed)
    rc = H2_PAL_ERR_CLOSED;
  else if (radio->peer && radio->connection != access->conn_handle)
    rc = H2_PAL_ERR_BUSY;
  else if (radio->count == 8u)
    rc = H2_PAL_ERR_NO_SPACE;
  else {
    radio->peer = true;
    radio->connection = access->conn_handle;
    size_t tail = (radio->head + radio->count) % 8u;
    memcpy(radio->queue[tail].data, data, length);
    radio->queue[tail].length = length;
    ++radio->count;
  }
  release(radio);
  if (rc == H2_PAL_OK && radio->config.wake != NULL)
    radio->config.wake(radio->config.user);
  return rc;
}

static int read_value(void* user, const h2_pal_ble_gatt_access_t* access,
                      uint8_t* out, size_t capacity, size_t* length) {
  (void)user;
  (void)out;
  (void)capacity;
  if (access == NULL || length == NULL || access->offset != 0u)
    return H2_PAL_ERR_INVALID_ARG;
  *length = 0u;
  return H2_PAL_OK;
}

h2_pal_result_t h2_alipay_tap_radio_open(
    const h2_alipay_tap_radio_config_t* config, h2_alipay_tap_radio_t** out) {
  if (out == NULL) return H2_PAL_ERR_INVALID_ARG;
  *out = NULL;
  if (config == NULL || config->memory == NULL || config->ble == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  const h2_pal_ble_vtable_t* vtable = config->ble->vtable;
  if (vtable == NULL || vtable->register_gatt_services == NULL ||
      vtable->unregister_gatt_service == NULL || vtable->notify == NULL ||
      vtable->disconnect == NULL)
    return H2_PAL_ERR_UNSUPPORTED;
  h2_alipay_tap_radio_t* radio =
      h2_pal_mem_alloc(config->memory, sizeof(*radio));
  if (radio == NULL) return H2_PAL_ERR_NO_MEMORY;
  memset(radio, 0, sizeof(*radio));
  radio->config = *config;
  int rc = h2_atomic_flag_init(&radio->lock) == H2_ATOMIC_OK
               ? H2_PAL_OK
               : H2_PAL_ERR_NO_MEMORY;
  if (rc != H2_PAL_OK) {
    h2_pal_mem_free(config->memory, radio);
    return rc;
  }
  *out = radio;
  radio->characteristic = (h2_pal_ble_gatt_characteristic_t){
      .uuid = {characteristic_uuid, sizeof(characteristic_uuid)},
      .properties = H2_PAL_BLE_GATT_PROPERTY_READ |
                    H2_PAL_BLE_GATT_PROPERTY_WRITE |
                    H2_PAL_BLE_GATT_PROPERTY_NOTIFY,
      .permissions =
          H2_PAL_BLE_GATT_PERMISSION_READ | H2_PAL_BLE_GATT_PERMISSION_WRITE,
      .max_value_len = 512u,
      .read = read_value,
      .write = write_value,
      .user = radio,
      .out_value_handle = &radio->value_handle};
  radio->service =
      (h2_pal_ble_gatt_service_t){.uuid = {service_uuid, sizeof(service_uuid)},
                                  .primary = true,
                                  .characteristics = &radio->characteristic,
                                  .characteristic_count = 1u};
  rc = h2_pal_ble_register_gatt_services(config->ble, &radio->service, 1u);
  if (rc == H2_PAL_OK) {
    radio->registered = true;
    const h2_pal_ble_adv_params_t params = {
        .mode = H2_PAL_BLE_ADV_MODE_CONNECTABLE,
        .interval_min_ms = 32u,
        .interval_max_ms = 48u,
        .type = H2_PAL_BLE_ADV_TYPE_LEGACY,
        .primary_phy = H2_PAL_BLE_PHY_1M,
        .secondary_phy = H2_PAL_BLE_PHY_1M};
    rc = h2_pal_ble_adv_set_create(config->ble, &params, &radio->advertising);
  }
  if (rc == H2_PAL_OK) {
    const h2_pal_ble_adv_data_t data = {
        .service_uuids = &radio->service.uuid,
        .service_uuid_count = 1u,
        .service_data = {radio->config.mac, sizeof(radio->config.mac)},
        .service_data_uuid = radio->service.uuid};
    rc = h2_pal_ble_adv_set_set_data(config->ble, radio->advertising, &data);
  }
  if (rc == H2_PAL_OK) {
    rc = h2_pal_ble_adv_set_start(config->ble, radio->advertising);
    radio->started = rc == H2_PAL_OK;
  }
  if (rc != H2_PAL_OK) (void)h2_alipay_tap_radio_close(out);
  return rc;
}

h2_pal_result_t h2_alipay_tap_radio_arm(h2_alipay_tap_radio_t* radio) {
  if (radio == NULL) return H2_PAL_ERR_INVALID_ARG;
  int rc = acquire(radio);
  if (rc != H2_PAL_OK) return rc;
  if (radio->closed || radio->armed || radio->peer)
    rc = H2_PAL_ERR_INVALID_STATE;
  else
    radio->armed = true;
  release(radio);
  return rc;
}

h2_pal_result_t h2_alipay_tap_radio_take(h2_alipay_tap_radio_t* radio,
                                         uint8_t* out, size_t* length) {
  if (radio == NULL || out == NULL || length == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  int rc = acquire(radio);
  if (rc != H2_PAL_OK) return rc;
  if (radio->closed)
    rc = H2_PAL_ERR_CLOSED;
  else if (radio->count == 0u)
    rc = H2_PAL_ERR_WOULD_BLOCK;
  else {
    size_t required = radio->queue[radio->head].length;
    if (*length < required)
      rc = H2_PAL_ERR_NO_SPACE;
    else {
      memcpy(out, radio->queue[radio->head].data, required);
      wipe(radio->queue + radio->head, sizeof(radio->queue[radio->head]));
      radio->head = (radio->head + 1u) % 8u;
      --radio->count;
    }
    *length = required;
  }
  release(radio);
  return rc;
}

h2_pal_result_t h2_alipay_tap_radio_notify(h2_alipay_tap_radio_t* radio,
                                           bool finish, const uint8_t* data,
                                           size_t length) {
  if (radio == NULL || data == NULL || length == 0u || length > 20u)
    return H2_PAL_ERR_INVALID_ARG;
  int rc = acquire(radio);
  if (rc != H2_PAL_OK) return rc;
  bool ready = !radio->closed && radio->armed && radio->peer;
  uint16_t connection = radio->connection;
  release(radio);
  if (!ready) return H2_PAL_ERR_INVALID_STATE;
  rc = h2_pal_ble_notify(radio->config.ble, connection, radio->value_handle,
                         data, length);
  if (rc == H2_PAL_OK && finish) {
    rc = acquire(radio);
    if (rc == H2_PAL_OK) {
      radio->armed = false;
      release(radio);
    }
  }
  return rc;
}

h2_pal_result_t h2_alipay_tap_radio_close(h2_alipay_tap_radio_t** owner) {
  if (owner == NULL) return H2_PAL_ERR_INVALID_ARG;
  h2_alipay_tap_radio_t* radio = *owner;
  if (radio == NULL) return H2_PAL_OK;
  int rc = acquire(radio);
  if (rc != H2_PAL_OK) return rc;
  radio->closed = true;
  release(radio);
  const h2_pal_ble_host_api_t* ble = radio->config.ble;
  if (radio->started) {
    rc = h2_pal_ble_adv_set_stop(ble, radio->advertising);
    if (rc != H2_PAL_OK) return rc;
    radio->started = false;
  }
  if (radio->advertising != NULL) {
    rc = h2_pal_ble_adv_set_destroy(ble, radio->advertising);
    if (rc != H2_PAL_OK) return rc;
    radio->advertising = NULL;
  }
  if (radio->registered) {
    rc = h2_pal_ble_unregister_gatt_service(ble, &radio->service.uuid);
    if (rc != H2_PAL_OK) return rc;
    radio->registered = false;
  }
  if (radio->peer) {
    rc = h2_pal_ble_disconnect(ble, radio->connection);
    if (rc != H2_PAL_OK && rc != H2_PAL_ERR_NOT_FOUND &&
        rc != H2_PAL_ERR_INVALID_STATE)
      return rc;
    radio->peer = false;
  }
  const h2_pal_mem_api_t* memory = radio->config.memory;
  h2_atomic_flag_destroy(&radio->lock);
  wipe(radio, sizeof(*radio));
  h2_pal_mem_free(memory, radio);
  *owner = NULL;
  return H2_PAL_OK;
}
