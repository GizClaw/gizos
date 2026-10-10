#include "h2_web_platform_internal.h"

#include "h2/pal/h2_pal_unsupported.h"
#include <emscripten.h>
#include <stdlib.h>
#include <string.h>

#define BLE_SERVICES 6u
#define BLE_CHARACTERISTICS 24u
#define BLE_NAME_BYTES 249u
#define BLE_CONNECTION 1u

typedef struct web_ble_service {
  uint8_t uuid[16];
  size_t uuid_len;
  uint16_t handle;
  bool active;
} web_ble_service_t;

typedef struct web_ble_characteristic {
  uint8_t uuid[16];
  size_t uuid_len;
  unsigned service;
  uint16_t value_handle;
  uint16_t cccd_handle;
  h2_pal_ble_gatt_characteristic_t schema;
  uint8_t value[H2_PAL_BLE_ATT_MAX_VALUE_LEN];
  size_t value_len;
  bool active;
  bool subscribed;
} web_ble_characteristic_t;

typedef struct web_ble {
  h2_web_platform_t *platform;
  h2_pal_ble_api_t api;
  h2_pal_ble_vtable_t vtable;
  pthread_mutex_t mutex;
  pthread_cond_t idle;
  web_ble_service_t services[BLE_SERVICES];
  web_ble_characteristic_t characteristics[BLE_CHARACTERISTICS];
  char name[BLE_NAME_BYTES];
  uint16_t next_handle;
  bool started;
  bool advertising;
  bool connectable;
  bool connected;
  bool staged;
  bool dispatching;
  bool retiring;
  pthread_t dispatcher;
} web_ble_t;

/* All JS objects stay on the browser thread. Requests contain copied bytes;
 * the pump invokes GATT callbacks on a Worker and acknowledges the real ATT
 * result. No callback or synchronous wait runs on the browser thread. */
/* clang-format off */
EM_JS(void, ble_bridge_js,
      (void *context, h2_web_main_result_t *result, h2_web_main_completion_t *completion), {
  h2WebMain(context, result, completion, ['i32','i32','i32','i32','i32','i32','i32'], 'i32',
    (action, platform, handle, flags, len, data, text) => {
    const registry = Module.h2WebBluetoothInstances ||= new Map();
    let s = registry.get(platform);
    const fail = (code, message) => Object.assign(new Error(message), {code});
    if (action === 0) {
      const option = Module.h2WebEnvironment?.ble;
      if (option === undefined || option?.enabled === false) return 0;
      if (option?.enabled !== true || Module.h2WebEnvironment?.version !== 1) return -1;
      const codes = {closed: handle, invalid: flags, full: len, timeout: HEAP32[data >> 2]};
      s = {codes, queue: [], pending: new Map(), next: 1, closed: false,
        snapshot: {simulated: true, started: false, advertising: false, connected: false,
          mtu: 517, localName: "", services: []}, notifications: [], serial: 0};
      const events = new EventTarget();
      const request = (op, attr = 0, bytes = new Uint8Array()) => {
        if (s.closed) return Promise.reject(fail(codes.closed, 'Simulated BLE is closed'));
        if (!Number.isInteger(attr) || attr < 0 || attr > 65535 ||
            !(bytes instanceof Uint8Array) || bytes.length > 514)
          return Promise.reject(fail(codes.invalid, 'Invalid simulated ATT request'));
        if (s.pending.size >= 32 || s.next >= 2147483647) return Promise.reject(fail(codes.full, 'Simulated BLE queue is full'));
        return new Promise((resolve, reject) => {
          const id = s.next++;
          const timer = setTimeout(() => {
            s.pending.delete(id); s.queue = s.queue.filter(item => item.id !== id);
            reject(fail(codes.timeout, 'Simulated BLE request timed out'));
          }, 10000);
          s.pending.set(id, {resolve, reject, timer});
          s.queue.push({id, op, attr, bytes: bytes.slice()});
          Module._h2_web_fake_ble_wake(platform);
        });
      };
      s.public = Object.freeze({
        simulated: true,
        get snapshot() { return structuredClone(s.snapshot); },
        get notifications() { return structuredClone(s.notifications); },
        scan: () => !s.closed && s.snapshot.advertising ? [structuredClone(s.snapshot)] : [],
        connect: () => request(1),
        disconnect: () => request(2),
        subscribe: (attr, enabled = true) => typeof enabled === 'boolean'
          ? request(3, attr, Uint8Array.of(Number(enabled)))
          : Promise.reject(fail(codes.invalid, 'Invalid subscription')),
        write: (attr, bytes) => request(4, attr, bytes),
        read: attr => request(5, attr),
        addEventListener: (...args) => events.addEventListener(...args),
        removeEventListener: (...args) => events.removeEventListener(...args)
      });
      s.events = events; registry.set(platform, s);
      Module.h2WebBluetooth = s.public;
      return 1;
    }
    if (!s) return -10;
    if (action === 1) {
      Object.assign(s.snapshot, {started: !!(flags & 1), advertising: !!(flags & 2),
        connected: !!(flags & 4), localName: UTF8ToString(text)});
    } else if (action === 2) {
      s.snapshot.services = [];
    } else if (action === 3) {
      const hex = (ptr, size) => Array.from(HEAPU8.subarray(ptr, ptr + size), b => b.toString(16).padStart(2,'0')).join("");
      const uuid = hex(data + 2, HEAPU8[data]);
      let service = s.snapshot.services.find(item => item.uuid === uuid);
      if (!service) { service = {uuid, characteristics: []}; s.snapshot.services.push(service); }
      service.characteristics.push({uuid: hex(data + 18, HEAPU8[data + 1]),
        valueHandle: handle, cccdHandle: len, properties: flags});
    } else if (action === 4) {
      const item = s.queue.shift();
      if (!item) return 0;
      HEAP32.set([item.id, item.op, item.attr, item.bytes.length], data >> 2);
      HEAPU8.set(item.bytes, data + 16);
      return 1;
    } else if (action === 5) {
      const pending = s.pending.get(handle);
      if (pending) {
        clearTimeout(pending.timer); s.pending.delete(handle);
        if (flags) pending.reject(fail(flags, 'Simulated ATT request failed: ' + flags));
        else pending.resolve({result: 0, value: HEAPU8.slice(data, data + len)});
      }
    } else if (action === 6) {
      const item = {sequence: ++s.serial, connection: flags, valueHandle: handle,
        value: Array.from(HEAPU8.subarray(data, data + len))};
      s.notifications.push(item);
      if (s.notifications.length > 128) s.notifications.shift();
      const event = new Event('notification'); event.data = structuredClone(item);
      s.events.dispatchEvent(event);
    } else if (action === 8) {
      const bytes = Array.from(HEAPU8.subarray(data, data + len));
      if (flags === 0) s.snapshot.advertisement = {serviceUuids: [], manufacturerData: [], serviceDataUuid: [], serviceData: []};
      else if (flags === 1) s.snapshot.advertisement.serviceUuids.push(bytes);
      else s.snapshot.advertisement[["","","manufacturerData",'serviceDataUuid','serviceData'][flags]] = bytes;
    } else if (action === 7) {
      s.closed = true;
      Object.assign(s.snapshot, {closed: true, started: false, advertising: false, connected: false, services: []});
      for (const pending of s.pending.values()) {
        clearTimeout(pending.timer); pending.reject(fail(s.codes.closed, 'Simulated BLE is closed'));
      }
      s.pending.clear(); s.queue = [];
      if (Module.h2WebBluetooth === s.public) delete Module.h2WebBluetooth;
      registry.delete(platform);
    }
    return 0;
  });
});
/* clang-format on */

static int bridge(web_ble_t *s, int action, int handle, int flags, size_t len,
                  const void *data, const char *text) {
  int size = (int)len;
  return h2_web_main_call(ble_bridge_js,
                          (const void *[]){&action, &s->platform, &handle,
                                           &flags, &size, &data, &text})
      .i32;
}

static bool uuid_valid(const h2_pal_ble_uuid_t *uuid) {
  return uuid && uuid->data &&
         (uuid->len == 2u || uuid->len == 4u || uuid->len == 16u);
}

/* Called with the mutex held. JS publication cannot invoke native callbacks. */
static void publish(web_ble_t *s) {
  bridge(s, 1, 0, s->started | (s->advertising << 1) | (s->connected << 2), 0,
         NULL, s->name);
  bridge(s, 2, 0, 0, 0, NULL, NULL);
  for (size_t i = 0; i < BLE_CHARACTERISTICS; ++i) {
    web_ble_characteristic_t *c = &s->characteristics[i];
    if (!c->active)
      continue;
    const web_ble_service_t *service = &s->services[c->service];
    uint8_t uuids[34] = {(uint8_t)service->uuid_len, (uint8_t)c->uuid_len};
    memcpy(uuids + 2, service->uuid, service->uuid_len);
    memcpy(uuids + 18, c->uuid, c->uuid_len);
    bridge(s, 3, c->value_handle, c->schema.properties, c->cccd_handle, uuids,
           NULL);
  }
}

static int post(web_ble_t *s, h2_pal_system_event_type_t type,
                const void *payload, size_t size) {
  const h2_pal_system_event_t event = {
      .type = type, .payload = payload, .payload_size = size};
  return h2_pal_system_event_post(&s->platform->system_event_api, &event, 0);
}

static web_ble_characteristic_t *find_char(web_ble_t *s, uint16_t handle) {
  for (size_t i = 0; i < BLE_CHARACTERISTICS; ++i)
    if (s->characteristics[i].active &&
        s->characteristics[i].value_handle == handle)
      return &s->characteristics[i];
  return NULL;
}

/* Stop/unregister fence the pump so borrowed callback contexts cannot escape.
 * A callback cannot synchronously unregister itself. */
static int fence(web_ble_t *s) {
  if (s->dispatching && pthread_equal(s->dispatcher, pthread_self()))
    return H2_PAL_ERR_BUSY;
  if (s->retiring)
    return H2_PAL_ERR_BUSY;
  s->retiring = true;
  while (s->dispatching)
    pthread_cond_wait(&s->idle, &s->mutex);
  return H2_PAL_OK;
}

static int start(void *user) {
  web_ble_t *s = user;
  pthread_mutex_lock(&s->mutex);
  s->started = true;
  publish(s);
  pthread_mutex_unlock(&s->mutex);
  return H2_PAL_OK;
}

static int disconnect(void *user, uint16_t handle) {
  web_ble_t *s = user;
  pthread_mutex_lock(&s->mutex);
  if (!s->connected || handle != BLE_CONNECTION) {
    pthread_mutex_unlock(&s->mutex);
    return H2_PAL_ERR_NOT_FOUND;
  }
  s->connected = false;
  for (size_t i = 0; i < BLE_CHARACTERISTICS; ++i)
    s->characteristics[i].subscribed = false;
  publish(s);
  pthread_mutex_unlock(&s->mutex);
  const h2_pal_ble_disconnected_info_t event = {.conn_handle = handle};
  return post(s, H2_PAL_SYSTEM_EVENT_TYPE_BLE_DISCONNECTED, &event,
              sizeof(event));
}

static int stop(void *user) {
  web_ble_t *s = user;
  pthread_mutex_lock(&s->mutex);
  int rc = fence(s);
  if (rc != H2_PAL_OK) {
    pthread_mutex_unlock(&s->mutex);
    return rc;
  }
  bool connected = s->connected;
  s->started = false;
  s->advertising = false;
  s->retiring = false;
  publish(s);
  pthread_mutex_unlock(&s->mutex);
  return connected ? disconnect(s, BLE_CONNECTION) : H2_PAL_OK;
}

static int set_adv_data(void *user, const h2_pal_ble_adv_data_t *data) {
  web_ble_t *s = user;
  if (!data || data->service_uuid_count > BLE_SERVICES ||
      data->manufacturer_data.len > H2_PAL_BLE_EXT_ADV_DATA_MAX_LEN ||
      data->service_data.len > H2_PAL_BLE_EXT_ADV_DATA_MAX_LEN ||
      (data->service_data_uuid.len && !uuid_valid(&data->service_data_uuid)) ||
      (data->service_uuid_count && !data->service_uuids) ||
      (data->manufacturer_data.len && !data->manufacturer_data.data) ||
      (data->service_data.len && !data->service_data.data))
    return H2_PAL_ERR_INVALID_ARG;
  size_t len = data->local_name ? strnlen(data->local_name, BLE_NAME_BYTES) : 0;
  if (len >= BLE_NAME_BYTES)
    return H2_PAL_ERR_INVALID_ARG;
  for (size_t i = 0; i < data->service_uuid_count; ++i)
    if (!uuid_valid(&data->service_uuids[i]))
      return H2_PAL_ERR_INVALID_ARG;
  pthread_mutex_lock(&s->mutex);
  if (!s->started) {
    pthread_mutex_unlock(&s->mutex);
    return H2_PAL_ERR_INVALID_STATE;
  }
  memset(s->name, 0, sizeof(s->name));
  if (len)
    memcpy(s->name, data->local_name, len);
  s->staged = true;
  bridge(s, 8, 0, 0, 0, NULL, NULL);
  for (size_t i = 0; i < data->service_uuid_count; ++i)
    bridge(s, 8, 0, 1, data->service_uuids[i].len, data->service_uuids[i].data,
           NULL);
  bridge(s, 8, 0, 2, data->manufacturer_data.len, data->manufacturer_data.data,
         NULL);
  bridge(s, 8, 0, 3, data->service_data_uuid.len, data->service_data_uuid.data,
         NULL);
  bridge(s, 8, 0, 4, data->service_data.len, data->service_data.data, NULL);
  publish(s);
  pthread_mutex_unlock(&s->mutex);
  return H2_PAL_OK;
}

static int start_advertising(void *user,
                             const h2_pal_ble_adv_params_t *params) {
  web_ble_t *s = user;
  if (!params || params->mode > H2_PAL_BLE_ADV_MODE_NON_CONNECTABLE ||
      params->type > H2_PAL_BLE_ADV_TYPE_EXTENDED ||
      params->interval_min_ms > params->interval_max_ms)
    return H2_PAL_ERR_INVALID_ARG;
  if (params->duration_ms || params->max_adv_events)
    return H2_PAL_ERR_UNSUPPORTED;
  pthread_mutex_lock(&s->mutex);
  int rc = s->started && s->staged ? H2_PAL_OK : H2_PAL_ERR_INVALID_STATE;
  if (!rc) {
    s->advertising = true;
    s->connectable = params->mode == H2_PAL_BLE_ADV_MODE_CONNECTABLE;
    publish(s);
  }
  pthread_mutex_unlock(&s->mutex);
  return rc;
}

static int stop_advertising(void *user) {
  web_ble_t *s = user;
  pthread_mutex_lock(&s->mutex);
  s->advertising = false;
  publish(s);
  pthread_mutex_unlock(&s->mutex);
  return H2_PAL_OK;
}

static int register_services(void *user,
                             const h2_pal_ble_gatt_service_t *services,
                             size_t count) {
  web_ble_t *s = user;
  if (!services || !count || count > BLE_SERVICES)
    return H2_PAL_ERR_INVALID_ARG;
  size_t chars = 0;
  for (size_t i = 0; i < count; ++i) {
    const h2_pal_ble_gatt_service_t *service = &services[i];
    if (!uuid_valid(&service->uuid) || !service->characteristics ||
        !service->characteristic_count ||
        service->characteristic_count > BLE_CHARACTERISTICS - chars)
      return H2_PAL_ERR_INVALID_ARG;
    chars += service->characteristic_count;
    for (size_t j = 0; j < service->characteristic_count; ++j) {
      const h2_pal_ble_gatt_characteristic_t *c = &service->characteristics[j];
      if (!uuid_valid(&c->uuid) || !c->max_value_len ||
          c->max_value_len > H2_PAL_BLE_ATT_MAX_VALUE_LEN ||
          c->initial_value_len > c->max_value_len ||
          (c->initial_value_len && !c->initial_value))
        return H2_PAL_ERR_INVALID_ARG;
    }
    for (size_t j = 0; j < i; ++j)
      if (services[j].uuid.len == service->uuid.len &&
          !memcmp(services[j].uuid.data, service->uuid.data, service->uuid.len))
        return H2_PAL_ERR_INVALID_ARG;
  }
  pthread_mutex_lock(&s->mutex);
  size_t free_services = 0, free_chars = 0;
  for (size_t i = 0; i < BLE_SERVICES; ++i) {
    if (!s->services[i].active) {
      ++free_services;
      continue;
    }
    for (size_t j = 0; j < count; ++j)
      if (s->services[i].uuid_len == services[j].uuid.len &&
          !memcmp(s->services[i].uuid, services[j].uuid.data,
                  services[j].uuid.len)) {
        pthread_mutex_unlock(&s->mutex);
        return H2_PAL_ERR_BUSY;
      }
  }
  for (size_t i = 0; i < BLE_CHARACTERISTICS; ++i)
    free_chars += !s->characteristics[i].active;
  if (s->retiring || count > free_services || chars > free_chars ||
      s->next_handle > UINT16_MAX - count - chars * 3u) {
    pthread_mutex_unlock(&s->mutex);
    return H2_PAL_ERR_NO_SPACE;
  }
  for (size_t i = 0; i < count; ++i) {
    size_t slot = 0;
    while (s->services[slot].active)
      ++slot;
    web_ble_service_t *service = &s->services[slot];
    *service = (web_ble_service_t){.uuid_len = services[i].uuid.len,
                                   .handle = s->next_handle++,
                                   .active = true};
    memcpy(service->uuid, services[i].uuid.data, service->uuid_len);
    if (services[i].out_service_handle)
      *services[i].out_service_handle = service->handle;
    for (size_t j = 0; j < services[i].characteristic_count; ++j) {
      size_t index = 0;
      while (s->characteristics[index].active)
        ++index;
      web_ble_characteristic_t *c = &s->characteristics[index];
      *c = (web_ble_characteristic_t){
          .uuid_len = services[i].characteristics[j].uuid.len,
          .service = (unsigned)slot,
          .schema = services[i].characteristics[j],
          .active = true};
      memcpy(c->uuid, c->schema.uuid.data, c->uuid_len);
      ++s->next_handle;
      c->value_handle = s->next_handle++;
      if (c->schema.properties &
          (H2_PAL_BLE_GATT_PROPERTY_NOTIFY | H2_PAL_BLE_GATT_PROPERTY_INDICATE))
        c->cccd_handle = s->next_handle++;
      c->value_len = c->schema.initial_value_len;
      if (c->value_len)
        memcpy(c->value, c->schema.initial_value, c->value_len);
      if (c->schema.out_value_handle)
        *c->schema.out_value_handle = c->value_handle;
      if (c->schema.out_cccd_handle)
        *c->schema.out_cccd_handle = c->cccd_handle;
    }
  }
  publish(s);
  pthread_mutex_unlock(&s->mutex);
  return H2_PAL_OK;
}

static int unregister_service(void *user, const h2_pal_ble_uuid_t *uuid) {
  web_ble_t *s = user;
  if (uuid && !uuid_valid(uuid))
    return H2_PAL_ERR_INVALID_ARG;
  pthread_mutex_lock(&s->mutex);
  int rc = fence(s);
  if (rc) {
    pthread_mutex_unlock(&s->mutex);
    return rc;
  }
  bool found = uuid == NULL, remaining = false;
  for (size_t i = 0; i < BLE_SERVICES; ++i) {
    web_ble_service_t *service = &s->services[i];
    if (service->active &&
        (!uuid || (uuid->len == service->uuid_len &&
                   !memcmp(uuid->data, service->uuid, uuid->len)))) {
      found = true;
      service->active = false;
      for (size_t j = 0; j < BLE_CHARACTERISTICS; ++j)
        if (s->characteristics[j].active && s->characteristics[j].service == i)
          s->characteristics[j].active = false;
    }
    remaining |= service->active;
  }
  s->retiring = false;
  bool connected = s->connected && !remaining;
  publish(s);
  pthread_mutex_unlock(&s->mutex);
  if (connected)
    (void)disconnect(s, BLE_CONNECTION);
  return found ? H2_PAL_OK : H2_PAL_ERR_NOT_FOUND;
}

static int unregister_all(void *user) { return unregister_service(user, NULL); }

static int notify(void *user, uint16_t conn, uint16_t attr, const uint8_t *data,
                  size_t len) {
  web_ble_t *s = user;
  if ((len && !data) || len > H2_PAL_BLE_ATT_MAX_VALUE_LEN)
    return H2_PAL_ERR_INVALID_ARG;
  pthread_mutex_lock(&s->mutex);
  web_ble_characteristic_t *c = find_char(s, attr);
  int rc = !s->connected || conn != BLE_CONNECTION ? H2_PAL_ERR_INVALID_STATE
           : !c                                    ? H2_PAL_ERR_NOT_FOUND
           : !c->subscribed                        ? H2_PAL_ERR_INVALID_STATE
           : !(c->schema.properties & H2_PAL_BLE_GATT_PROPERTY_NOTIFY) ||
                   len > c->schema.max_value_len
               ? H2_PAL_ERR_INVALID_ARG
               : H2_PAL_OK;
  if (!rc)
    bridge(s, 6, attr, conn, len, data, NULL);
  pthread_mutex_unlock(&s->mutex);
  return rc;
}

static int exchange_mtu(void *user, uint16_t conn, uint16_t *out,
                        uint32_t timeout) {
  (void)timeout;
  if (!out)
    return H2_PAL_ERR_INVALID_ARG;
  *out = 0;
  web_ble_t *s = user;
  pthread_mutex_lock(&s->mutex);
  int rc =
      s->connected && conn == BLE_CONNECTION ? H2_PAL_OK : H2_PAL_ERR_NOT_FOUND;
  if (!rc)
    *out = H2_PAL_BLE_ATT_MAX_MTU;
  pthread_mutex_unlock(&s->mutex);
  return rc;
}

static int dispatch(web_ble_t *s, const int32_t *request, uint8_t *out,
                    size_t *out_len) {
  const int op = request[1];
  const uint16_t handle = (uint16_t)request[2];
  const size_t len = (size_t)request[3];
  const uint8_t *data = (const uint8_t *)(request + 4);
  if (len > H2_PAL_BLE_ATT_MAX_VALUE_LEN)
    return H2_PAL_ERR_INVALID_ARG;
  if (op == 2)
    return disconnect(s, BLE_CONNECTION);
  pthread_mutex_lock(&s->mutex);
  if (!s->started) {
    pthread_mutex_unlock(&s->mutex);
    return H2_PAL_ERR_INVALID_STATE;
  }
  if (op == 1) {
    if (!s->advertising || !s->connectable || s->connected) {
      pthread_mutex_unlock(&s->mutex);
      return H2_PAL_ERR_INVALID_STATE;
    }
    s->connected = true;
    s->advertising = false;
    publish(s);
    pthread_mutex_unlock(&s->mutex);
    const h2_pal_ble_connection_t event = {
        .conn_handle = BLE_CONNECTION,
        .role = H2_PAL_BLE_ROLE_PERIPHERAL,
        .mtu = H2_PAL_BLE_ATT_MAX_MTU,
        .peer_addr = {.value = {2, 0, 0, 0, 0, 1},
                      .type = H2_PAL_BLE_ADDR_TYPE_RANDOM}};
    return post(s, H2_PAL_SYSTEM_EVENT_TYPE_BLE_CONNECTED, &event,
                sizeof(event));
  }
  web_ble_characteristic_t *c = find_char(s, handle);
  if (!s->connected || !c) {
    pthread_mutex_unlock(&s->mutex);
    return H2_PAL_ERR_NOT_FOUND;
  }
  const h2_pal_ble_gatt_access_t access = {.conn_handle = BLE_CONNECTION,
                                           .attr_handle = handle};
  if (op == 3) {
    if (!(c->schema.properties & H2_PAL_BLE_GATT_PROPERTY_NOTIFY) ||
        len != 1u) {
      pthread_mutex_unlock(&s->mutex);
      return H2_PAL_ERR_INVALID_ARG;
    }
    c->subscribed = data[0] != 0;
    const h2_pal_ble_subscription_state_t event = {
        .conn_handle = BLE_CONNECTION,
        .value_handle = handle,
        .mode = H2_PAL_BLE_SUBSCRIBE_MODE_NOTIFY,
        .enabled = c->subscribed};
    pthread_mutex_unlock(&s->mutex);
    return post(s, H2_PAL_SYSTEM_EVENT_TYPE_BLE_SUBSCRIPTION_CHANGED, &event,
                sizeof(event));
  }
  h2_pal_ble_gatt_characteristic_t schema = c->schema;
  const uint32_t secured =
      op == 4 ? H2_PAL_BLE_GATT_PERMISSION_WRITE_ENCRYPTED |
                    H2_PAL_BLE_GATT_PERMISSION_WRITE_AUTHENTICATED
              : H2_PAL_BLE_GATT_PERMISSION_READ_ENCRYPTED |
                    H2_PAL_BLE_GATT_PERMISSION_READ_AUTHENTICATED;
  if (schema.permissions & secured) {
    pthread_mutex_unlock(&s->mutex);
    return H2_PAL_ERR_UNSUPPORTED;
  }
  int rc = H2_PAL_ERR_INVALID_ARG;
  if (op == 4 &&
      (schema.properties & (H2_PAL_BLE_GATT_PROPERTY_WRITE |
                            H2_PAL_BLE_GATT_PROPERTY_WRITE_NO_RSP)) &&
      (schema.permissions & H2_PAL_BLE_GATT_PERMISSION_WRITE) &&
      len <= schema.max_value_len) {
    pthread_mutex_unlock(&s->mutex);
    rc = schema.write ? schema.write(schema.user, &access, data, len)
                      : H2_PAL_OK;
    pthread_mutex_lock(&s->mutex);
    if (!rc) {
      memcpy(c->value, data, len);
      c->value_len = len;
    }
  } else if (op == 5 && (schema.properties & H2_PAL_BLE_GATT_PROPERTY_READ) &&
             (schema.permissions & H2_PAL_BLE_GATT_PERMISSION_READ)) {
    if (schema.read) {
      pthread_mutex_unlock(&s->mutex);
      rc =
          schema.read(schema.user, &access, out, schema.max_value_len, out_len);
      pthread_mutex_lock(&s->mutex);
      if (*out_len > schema.max_value_len) {
        *out_len = 0;
        rc = H2_PAL_ERR_NO_SPACE;
      }
    } else {
      memcpy(out, c->value, c->value_len);
      *out_len = c->value_len;
      rc = H2_PAL_OK;
    }
  }
  pthread_mutex_unlock(&s->mutex);
  return rc;
}

void h2_web_platform_fake_ble_poll(h2_web_platform_t *platform) {
  web_ble_t *s = platform->fake_ble;
  if (!s)
    return;
  for (unsigned budget = 0; budget < 8; ++budget) {
    int32_t request[4 + (H2_PAL_BLE_ATT_MAX_VALUE_LEN + 3u) / 4u] = {0};
    pthread_mutex_lock(&s->mutex);
    if (s->retiring) {
      pthread_mutex_unlock(&s->mutex);
      h2_web_platform_schedule(platform);
      return;
    }
    s->dispatching = true;
    s->dispatcher = pthread_self();
    pthread_mutex_unlock(&s->mutex);
    int pending = bridge(s, 4, 0, 0, 0, request, NULL);
    uint8_t out[H2_PAL_BLE_ATT_MAX_VALUE_LEN];
    size_t out_len = 0;
    if (pending == 1) {
      int rc = dispatch(s, request, out, &out_len);
      bridge(s, 5, request[0], rc, out_len, out, NULL);
    }
    pthread_mutex_lock(&s->mutex);
    s->dispatching = false;
    pthread_cond_broadcast(&s->idle);
    pthread_mutex_unlock(&s->mutex);
    if (pending != 1)
      return;
  }
  h2_web_platform_schedule(platform);
}

EMSCRIPTEN_KEEPALIVE void h2_web_fake_ble_wake(h2_web_platform_t *platform) {
  h2_web_platform_schedule(platform);
}

h2_pal_result_t h2_web_platform_fake_ble_init(h2_web_platform_t *platform) {
  web_ble_t *s = calloc(1, sizeof(*s));
  if (!s)
    return H2_PAL_ERR_NO_MEMORY;
  s->platform = platform;
  const int timeout = H2_PAL_ERR_TIMEOUT;
  int enabled = bridge(s, 0, H2_PAL_ERR_CLOSED, H2_PAL_ERR_INVALID_ARG,
                       H2_PAL_ERR_FULL, &timeout, NULL);
  if (enabled != 1) {
    free(s);
    return enabled == 0 ? H2_PAL_OK : H2_PAL_ERR_INVALID_ARG;
  }
  pthread_mutex_init(&s->mutex, NULL);
  pthread_cond_init(&s->idle, NULL);
  s->next_handle = 1;
  s->vtable = *h2_pal_unsupported_ble_host_api()->vtable;
  s->vtable.start = start;
  s->vtable.stop = stop;
  s->vtable.set_adv_data = set_adv_data;
  s->vtable.start_advertising = start_advertising;
  s->vtable.stop_advertising = stop_advertising;
  s->vtable.register_gatt_services = register_services;
  s->vtable.unregister_gatt_service = unregister_service;
  s->vtable.unregister_gatt_services = unregister_all;
  s->vtable.notify = notify;
  s->vtable.disconnect = disconnect;
  s->vtable.exchange_mtu = exchange_mtu;
  s->api = (h2_pal_ble_api_t){.user = s, .vtable = &s->vtable};
  platform->fake_ble = s;
  return H2_PAL_OK;
}

void h2_web_platform_fake_ble_deinit(h2_web_platform_t *platform) {
  web_ble_t *s = platform->fake_ble;
  if (!s)
    return;
  bridge(s, 7, 0, 0, 0, NULL, NULL);
  pthread_cond_destroy(&s->idle);
  pthread_mutex_destroy(&s->mutex);
  free(s);
  platform->fake_ble = NULL;
}

const h2_pal_ble_api_t *
h2_web_platform_fake_ble_api(h2_web_platform_t *platform) {
  web_ble_t *s = platform ? platform->fake_ble : NULL;
  return s ? &s->api : NULL;
}
