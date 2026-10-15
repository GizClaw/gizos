#include "h2_gizclaw_ble_binding.h"

#include <string.h>

#define BINDING_CONNECTION_MAX 8u
#define BINDING_SUBSCRIPTION_MAX 6u
#define BINDING_ORIGIN_MAX 192u
#define BINDING_ICON_MAX 32u
#define BINDING_NAME_MAX 160u
#define BINDING_LOCAL_NAME_MAX 29u

/* Reverse RFC 4122 display order, as required by BLE PAL UUID values. */
#define BINDING_UUID(slot)                                                     \
  {0x36, 0x3b, 0xf8, 0xe4, 0x2b, 0xb1, 0x2f, 0x8c,                             \
   0x0d, 0x4b, 0xa6, 0x91, slot, 0xd4, 0xed, 0x14}
static const uint8_t service_uuid_bytes[16] = BINDING_UUID(0xb0);
static const uint8_t info_uuid_bytes[16] = BINDING_UUID(0xb1);
static const uint8_t request_uuid_bytes[16] = BINDING_UUID(0xb2);
static const uint8_t credential_uuid_bytes[16] = BINDING_UUID(0xb3);
const h2_pal_ble_uuid_t h2_gizclaw_ble_binding_service_uuid = {
    service_uuid_bytes, sizeof(service_uuid_bytes)};
const h2_pal_ble_uuid_t h2_gizclaw_ble_binding_info_uuid = {
    info_uuid_bytes, sizeof(info_uuid_bytes)};
const h2_pal_ble_uuid_t h2_gizclaw_ble_binding_request_uuid = {
    request_uuid_bytes, sizeof(request_uuid_bytes)};
const h2_pal_ble_uuid_t h2_gizclaw_ble_binding_credential_uuid = {
    credential_uuid_bytes, sizeof(credential_uuid_bytes)};

typedef struct binding_connection {
  uint16_t handle;
  uint16_t mtu;
} binding_connection_t;

struct h2_gizclaw_ble_binding {
  h2_gizclaw_ble_binding_config_t config;
  char origin[BINDING_ORIGIN_MAX + 1u];
  char icon[BINDING_ICON_MAX + 1u];
  char name[BINDING_NAME_MAX + 1u];
  char local_name[BINDING_LOCAL_NAME_MAX + 1u];
  h2_pal_mutex_t *mutex;
  h2_pal_ble_gatt_service_t service;
  h2_pal_ble_gatt_characteristic_t characteristics[3];
  uint16_t handles[3];
  h2_pal_system_event_subscription_t *subscriptions[BINDING_SUBSCRIPTION_MAX];
  h2_pal_ble_adv_set_t *adv_set;
  bool registered;
  bool open;
  bool advertising;
  bool host_stopped;
  uint64_t adv_event_revision;
  uint16_t conn_handle;
  binding_connection_t connections[BINDING_CONNECTION_MAX];
  bool pending;
  uint64_t requested_revision;
  uint16_t requested_offset;
  uint16_t requested_limit;
  h2_gizclaw_ble_binding_exposure_t
      exposures[H2_GIZCLAW_BLE_BINDING_EXPOSURE_MAX];
  size_t exposure_head;
  size_t exposure_count;
  h2_gizclaw_ble_binding_exposure_t last_exposed;
  h2_pal_result_t last_error;
};

static void erase(void *data, size_t len) {
  volatile unsigned char *p = data;
  while (len-- != 0u)
    *p++ = 0u;
}

static bool span_valid(h2_gizclaw_str_t text, size_t max_len) {
  return text.len <= max_len &&
         (text.len == 0u ||
          (text.data != NULL && memchr(text.data, 0, text.len) == NULL));
}

static bool ascii_alnum(unsigned char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9');
}

static bool local_name_valid(h2_gizclaw_str_t text) {
  if (!span_valid(text, BINDING_LOCAL_NAME_MAX)) return false;
  for (size_t i = 0u; i < text.len; ++i) {
    const unsigned char c = (unsigned char)text.data[i];
    if (c < 0x20u || c > 0x7eu) return false;
  }
  return true;
}

static bool unreserved(unsigned char c) {
  return ascii_alnum(c) || c == '-' || c == '_' || c == '.' || c == '~';
}

static bool origin_valid(h2_gizclaw_str_t text) {
  if (!span_valid(text, BINDING_ORIGIN_MAX) || text.len <= 8u ||
      memcmp(text.data, "https://", 8u) != 0)
    return false;
  size_t i = 8u, label_len = 0u;
  for (; i < text.len && text.data[i] != ':'; ++i) {
    unsigned char c = (unsigned char)text.data[i];
    if (c == '.') {
      if (label_len == 0u || text.data[i - 1u] == '-')
        return false;
      label_len = 0u;
    } else {
      if ((!ascii_alnum(c) && c != '-') || (label_len == 0u && c == '-') ||
          ++label_len > 63u)
        return false;
    }
  }
  if (label_len == 0u || text.data[i - 1u] == '-')
    return false;
  if (i == text.len)
    return true;
  if (++i == text.len)
    return false;
  uint32_t port = 0u;
  for (; i < text.len; ++i) {
    unsigned char c = (unsigned char)text.data[i];
    if (c < '0' || c > '9' || port > 6553u)
      return false;
    port = port * 10u + (uint32_t)(c - '0');
    if (port > 65535u)
      return false;
  }
  return port != 0u;
}

static bool icon_valid(h2_gizclaw_str_t text) {
  if (!span_valid(text, BINDING_ICON_MAX))
    return false;
  for (size_t i = 0u; i < text.len; ++i) {
    unsigned char c = (unsigned char)text.data[i];
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
          (i != 0u && (c == '-' || c == '_'))))
      return false;
  }
  return true;
}

static bool name_valid(h2_gizclaw_str_t text) {
  if (!span_valid(text, BINDING_NAME_MAX))
    return false;
  size_t scalars = 0u;
  for (size_t i = 0u; i < text.len;) {
    uint32_t value = (unsigned char)text.data[i++];
    size_t continuation = 0u;
    uint32_t minimum = 0u;
    if (value < 0x80u) {
      if (value < 0x20u || value == 0x7fu)
        return false;
    } else if (value >= 0xc2u && value <= 0xdfu) {
      continuation = 1u;
      value &= 0x1fu;
      minimum = 0x80u;
    } else if (value >= 0xe0u && value <= 0xefu) {
      continuation = 2u;
      value &= 0x0fu;
      minimum = 0x800u;
    } else if (value >= 0xf0u && value <= 0xf4u) {
      continuation = 3u;
      value &= 0x07u;
      minimum = 0x10000u;
    } else {
      return false;
    }
    if (continuation > text.len - i)
      return false;
    for (size_t j = 0u; j < continuation; ++j) {
      unsigned char c = (unsigned char)text.data[i++];
      if ((c & 0xc0u) != 0x80u)
        return false;
      value = (value << 6u) | (uint32_t)(c & 0x3fu);
    }
    if (value < minimum || value > 0x10ffffu ||
        (value >= 0xd800u && value <= 0xdfffu) ||
        (value >= 0x80u && value <= 0x9fu) || ++scalars > 40u)
      return false;
  }
  return true;
}

static bool secret_valid(h2_gizclaw_str_t text) {
  if (!span_valid(text, 95u) || text.len <= 14u ||
      memcmp(text.data, "gizclaw_sk_v1_", 14u) != 0)
    return false;
  for (size_t i = 14u; i < text.len; ++i) {
    unsigned char c = (unsigned char)text.data[i];
    if (!ascii_alnum(c) && c != '-' && c != '_')
      return false;
  }
  return true;
}

static void append_text(char *out, size_t *len, h2_gizclaw_str_t text,
                        bool percent_encode) {
  static const char hex[] = "0123456789ABCDEF";
  for (size_t i = 0u; i < text.len; ++i) {
    unsigned char c = (unsigned char)text.data[i];
    if (percent_encode && !unreserved(c)) {
      out[(*len)++] = '%';
      out[(*len)++] = hex[c >> 4u];
      out[(*len)++] = hex[c & 15u];
    } else {
      out[(*len)++] = (char)c;
    }
  }
}

h2_pal_result_t
h2_gizclaw_ble_binding_format_url(h2_gizclaw_str_t server_origin,
                                  h2_gizclaw_str_t icon, h2_gizclaw_str_t name,
                                  h2_gizclaw_str_t secret, char *out,
                                  size_t capacity, size_t *out_len) {
  if (out_len != NULL)
    *out_len = 0u;
  if (out == NULL || capacity == 0u || out_len == NULL) {
    if (out != NULL)
      erase(out, capacity);
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (!origin_valid(server_origin) || !icon_valid(icon) || !name_valid(name) ||
      !secret_valid(secret)) {
    erase(out, capacity);
    return H2_PAL_ERR_INVALID_ARG;
  }
  size_t required = server_origin.len + 10u + secret.len;
  if (icon.len != 0u)
    required += 6u + icon.len;
  if (name.len != 0u) {
    required += 6u;
    for (size_t i = 0u; i < name.len; ++i)
      required += unreserved((unsigned char)name.data[i]) ? 1u : 3u;
  }
  if (required > H2_GIZCLAW_BLE_BINDING_URL_MAX || required >= capacity) {
    erase(out, capacity);
    return H2_PAL_ERR_TRUNCATED;
  }
  size_t len = 0u;
  append_text(out, &len, server_origin, false);
  append_text(out, &len, (h2_gizclaw_str_t){"/api-keys/", 10u}, false);
  append_text(out, &len, secret, false);
  if (icon.len != 0u) {
    append_text(out, &len, (h2_gizclaw_str_t){"?icon=", 6u}, false);
    append_text(out, &len, icon, false);
  }
  if (name.len != 0u) {
    append_text(out, &len,
                (h2_gizclaw_str_t){icon.len == 0u ? "?name=" : "&name=", 6u},
                false);
    append_text(out, &len, name, true);
  }
  out[len] = '\0';
  *out_len = len;
  return H2_PAL_OK;
}

static void put_u16(uint8_t *out, uint16_t value) {
  out[0] = (uint8_t)value;
  out[1] = (uint8_t)(value >> 8u);
}

static void put_u64(uint8_t *out, uint64_t value) {
  for (size_t i = 0u; i < 8u; ++i)
    out[i] = (uint8_t)(value >> (8u * i));
}

static uint16_t get_u16(const uint8_t *data) {
  return (uint16_t)((uint16_t)data[0] | ((uint16_t)data[1] << 8u));
}

static uint64_t get_u64(const uint8_t *data) {
  uint64_t value = 0u;
  for (size_t i = 0u; i < 8u; ++i)
    value |= (uint64_t)data[i] << (8u * i);
  return value;
}

static h2_pal_result_t lock(h2_gizclaw_ble_binding_t *binding) {
  return h2_pal_mutex_lock(binding->config.sync, binding->mutex);
}

static void unlock(h2_gizclaw_ble_binding_t *binding) {
  (void)h2_pal_mutex_unlock(binding->config.sync, binding->mutex);
}

static uint16_t payload_max(h2_gizclaw_ble_binding_t *binding, uint16_t conn) {
  uint16_t mtu = 23u;
  for (size_t i = 0u; i < BINDING_CONNECTION_MAX; ++i) {
    if (binding->connections[i].handle == conn) {
      mtu = binding->connections[i].mtu;
      break;
    }
  }
  /* A short ATT response ends mobile stacks' automatic long-read procedure. */
  size_t capacity = mtu - 2u;
  if (capacity > H2_GIZCLAW_BLE_BINDING_FRAME_MAX)
    capacity = H2_GIZCLAW_BLE_BINDING_FRAME_MAX;
  return (uint16_t)(capacity - H2_GIZCLAW_BLE_BINDING_HEADER_LEN);
}

/* Called under binding mutex. API-key snapshot never waits for RPC. */
static h2_pal_result_t credential(h2_gizclaw_ble_binding_t *binding,
                                  h2_gizclaw_api_key_snapshot_t *key, char *url,
                                  size_t *out_len) {
  *out_len = 0u;
  h2_pal_result_t rc =
      h2_gizclaw_api_key_state_snapshot(binding->config.api_key_state, key);
  if (rc != H2_PAL_OK)
    return rc;
  if (!key->valid || key->stale || key->busy || key->closed)
    return H2_PAL_ERR_INVALID_STATE;
  const char *end = memchr(key->key.secret, 0, sizeof(key->key.secret));
  if (end == NULL || key->key.name[0] == '\0' ||
      memchr(key->key.name, 0, sizeof(key->key.name)) == NULL)
    return H2_PAL_ERR_FORMAT;
  return h2_gizclaw_ble_binding_format_url(
      binding->config.server_origin, binding->config.icon, binding->config.name,
      (h2_gizclaw_str_t){key->key.secret, (size_t)(end - key->key.secret)}, url,
      H2_GIZCLAW_BLE_BINDING_URL_MAX + 1u, out_len);
}

static bool access_valid(const h2_pal_ble_gatt_access_t *access) {
  return access != NULL &&
         access->conn_handle != H2_PAL_BLE_INVALID_CONN_HANDLE &&
         access->offset == 0u;
}

static uint8_t key_status(const h2_gizclaw_api_key_snapshot_t *key,
                          h2_pal_result_t credential_result) {
  if (key->closed)
    return H2_GIZCLAW_BLE_BINDING_CLOSED;
  if (key->busy)
    return H2_GIZCLAW_BLE_BINDING_BUSY;
  if (credential_result == H2_PAL_OK)
    return H2_GIZCLAW_BLE_BINDING_READY;
  if (key->last_error == H2_GIZCLAW_API_KEY_ERR_EXHAUSTED &&
      key->has_rpc_error &&
      key->rpc_error_code == H2_GIZCLAW_RPC_ERROR_RESOURCE_EXHAUSTED)
    return H2_GIZCLAW_BLE_BINDING_EXHAUSTED;
  if (key->last_error != H2_PAL_OK ||
      credential_result != H2_PAL_ERR_INVALID_STATE)
    return H2_GIZCLAW_BLE_BINDING_FAILED;
  return 0u;
}

static h2_pal_result_t info_read(void *user,
                                 const h2_pal_ble_gatt_access_t *access,
                                 uint8_t *out, size_t capacity,
                                 size_t *out_len) {
  if (out_len != NULL)
    *out_len = 0u;
  if (!access_valid(access) || out == NULL || out_len == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  if (capacity < H2_GIZCLAW_BLE_BINDING_INFO_LEN)
    return H2_PAL_ERR_NO_SPACE;
  h2_gizclaw_ble_binding_t *binding = user;
  h2_pal_result_t rc = lock(binding);
  if (rc != H2_PAL_OK)
    return rc;
  if (!binding->open) {
    unlock(binding);
    return H2_PAL_ERR_CLOSED;
  }
  h2_gizclaw_api_key_snapshot_t key = {0};
  char url[H2_GIZCLAW_BLE_BINDING_URL_MAX + 1u] = {0};
  size_t url_len = 0u;
  rc = credential(binding, &key, url, &url_len);
  out[0] = H2_GIZCLAW_BLE_BINDING_VERSION;
  out[1] = key_status(&key, rc);
  put_u64(out + 2u, key.revision);
  put_u16(out + 10u, (uint16_t)url_len);
  put_u16(out + 12u, payload_max(binding, access->conn_handle));
  *out_len = H2_GIZCLAW_BLE_BINDING_INFO_LEN;
  rc = H2_PAL_OK;
  erase(&key, sizeof(key));
  erase(url, sizeof(url));
  unlock(binding);
  return rc;
}

static h2_pal_result_t request_write(void *user,
                                     const h2_pal_ble_gatt_access_t *access,
                                     const uint8_t *data, size_t len) {
  if (!access_valid(access) || data == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  if (len != H2_GIZCLAW_BLE_BINDING_REQUEST_LEN ||
      data[0] != H2_GIZCLAW_BLE_BINDING_VERSION)
    return H2_PAL_ERR_FORMAT;
  uint64_t revision = get_u64(data + 1u);
  uint16_t offset = get_u16(data + 9u), limit = get_u16(data + 11u);
  if (limit == 0u || limit > H2_GIZCLAW_BLE_BINDING_FRAME_MAX -
                                 H2_GIZCLAW_BLE_BINDING_HEADER_LEN)
    return H2_PAL_ERR_INVALID_ARG;
  h2_gizclaw_ble_binding_t *binding = user;
  h2_pal_result_t rc = lock(binding);
  if (rc != H2_PAL_OK)
    return rc;
  if (!binding->open) {
    unlock(binding);
    return H2_PAL_ERR_CLOSED;
  }
  if (binding->conn_handle != H2_PAL_BLE_INVALID_CONN_HANDLE &&
      binding->conn_handle != access->conn_handle) {
    unlock(binding);
    return H2_PAL_ERR_BUSY;
  }
  h2_gizclaw_api_key_snapshot_t key = {0};
  char url[H2_GIZCLAW_BLE_BINDING_URL_MAX + 1u] = {0};
  size_t url_len = 0u;
  rc = credential(binding, &key, url, &url_len);
  if (rc == H2_PAL_OK && key.revision != revision)
    rc = H2_PAL_ERR_INVALID_STATE;
  if (rc != H2_PAL_OK) {
    binding->pending = false;
  } else if (offset > url_len) {
    rc = H2_PAL_ERR_INVALID_ARG;
  } else if (binding->pending && (revision != binding->requested_revision ||
                                  offset != binding->requested_offset ||
                                  limit != binding->requested_limit)) {
    rc = H2_PAL_ERR_BUSY;
  } else {
    binding->conn_handle = access->conn_handle;
    binding->pending = true;
    binding->requested_revision = revision;
    binding->requested_offset = offset;
    binding->requested_limit = limit;
  }
  erase(&key, sizeof(key));
  erase(url, sizeof(url));
  unlock(binding);
  return rc;
}

static h2_pal_result_t
record_exposure(h2_gizclaw_ble_binding_t *binding,
                const h2_gizclaw_api_key_snapshot_t *key) {
  if (binding->last_exposed.revision == key->revision &&
      strcmp(binding->last_exposed.key_name, key->key.name) == 0)
    return H2_PAL_OK;
  if (binding->exposure_count == H2_GIZCLAW_BLE_BINDING_EXPOSURE_MAX)
    return H2_PAL_ERR_NO_SPACE;
  h2_gizclaw_ble_binding_exposure_t exposure = {.revision = key->revision};
  memcpy(exposure.key_name, key->key.name, sizeof(exposure.key_name));
  size_t tail = (binding->exposure_head + binding->exposure_count) %
                H2_GIZCLAW_BLE_BINDING_EXPOSURE_MAX;
  binding->exposures[tail] = exposure;
  ++binding->exposure_count;
  binding->last_exposed = exposure;
  return H2_PAL_OK;
}

static h2_pal_result_t credential_read(void *user,
                                       const h2_pal_ble_gatt_access_t *access,
                                       uint8_t *out, size_t capacity,
                                       size_t *out_len) {
  if (out_len != NULL)
    *out_len = 0u;
  if (!access_valid(access) || out == NULL || out_len == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  if (capacity < H2_GIZCLAW_BLE_BINDING_HEADER_LEN)
    return H2_PAL_ERR_NO_SPACE;
  h2_gizclaw_ble_binding_t *binding = user;
  h2_pal_result_t rc = lock(binding);
  if (rc != H2_PAL_OK)
    return rc;
  if (!binding->open) {
    unlock(binding);
    return H2_PAL_ERR_CLOSED;
  }
  if (binding->conn_handle != access->conn_handle || !binding->pending) {
    rc = binding->conn_handle != H2_PAL_BLE_INVALID_CONN_HANDLE &&
                 binding->conn_handle != access->conn_handle
             ? H2_PAL_ERR_BUSY
             : H2_PAL_ERR_INVALID_STATE;
    unlock(binding);
    return rc;
  }
  h2_gizclaw_api_key_snapshot_t key = {0};
  char url[H2_GIZCLAW_BLE_BINDING_URL_MAX + 1u] = {0};
  size_t url_len = 0u;
  rc = credential(binding, &key, url, &url_len);
  if (rc == H2_PAL_OK && key.revision != binding->requested_revision)
    rc = H2_PAL_ERR_INVALID_STATE;
  if (rc == H2_PAL_OK && binding->requested_offset > url_len)
    rc = H2_PAL_ERR_INVALID_STATE;
  if (rc != H2_PAL_OK) {
    binding->pending = false;
  } else {
    size_t len = url_len - binding->requested_offset;
    if (len > binding->requested_limit)
      len = binding->requested_limit;
    uint16_t max_payload = payload_max(binding, access->conn_handle);
    if (len > max_payload)
      len = max_payload;
    if (len > capacity - H2_GIZCLAW_BLE_BINDING_HEADER_LEN)
      len = capacity - H2_GIZCLAW_BLE_BINDING_HEADER_LEN;
    if (len == 0u && binding->requested_offset != url_len) {
      rc = H2_PAL_ERR_NO_SPACE;
    } else if (len != 0u &&
               (rc = record_exposure(binding, &key)) != H2_PAL_OK) {
      /* No bytes leave this function unless the obligation is recorded. */
    } else {
      out[0] = H2_GIZCLAW_BLE_BINDING_VERSION;
      put_u64(out + 1u, key.revision);
      put_u16(out + 9u, binding->requested_offset);
      put_u16(out + 11u, (uint16_t)url_len);
      memcpy(out + H2_GIZCLAW_BLE_BINDING_HEADER_LEN,
             url + binding->requested_offset, len);
      *out_len = H2_GIZCLAW_BLE_BINDING_HEADER_LEN + len;
      binding->pending = false;
    }
  }
  erase(&key, sizeof(key));
  erase(url, sizeof(url));
  unlock(binding);
  return rc;
}

static int on_system_event(void *user, const h2_pal_system_event_t *event) {
  h2_gizclaw_ble_binding_t *binding = user;
  h2_pal_result_t rc = lock(binding);
  if (rc != H2_PAL_OK)
    return rc;
  if (event->type == H2_PAL_SYSTEM_EVENT_TYPE_BLE_CONNECTED &&
      event->payload != NULL &&
      event->payload_size == sizeof(h2_pal_ble_connection_t)) {
    const h2_pal_ble_connection_t *conn = event->payload;
    if (conn->role == H2_PAL_BLE_ROLE_PERIPHERAL &&
        conn->conn_handle != H2_PAL_BLE_INVALID_CONN_HANDLE) {
      if (binding->conn_handle == conn->conn_handle) {
        binding->conn_handle = H2_PAL_BLE_INVALID_CONN_HANDLE;
        binding->pending = false;
      }
      for (size_t i = 0u; i < BINDING_CONNECTION_MAX; ++i) {
        if (binding->connections[i].handle == H2_PAL_BLE_INVALID_CONN_HANDLE ||
            binding->connections[i].handle == conn->conn_handle) {
          binding->connections[i].handle = conn->conn_handle;
          binding->connections[i].mtu =
              conn->mtu >= 23u && conn->mtu <= H2_PAL_BLE_ATT_MAX_MTU
                  ? conn->mtu
                  : 23u;
          break;
        }
      }
    }
  } else if (event->type == H2_PAL_SYSTEM_EVENT_TYPE_BLE_MTU_CHANGED &&
             event->payload != NULL &&
             event->payload_size == sizeof(h2_pal_ble_mtu_info_t)) {
    const h2_pal_ble_mtu_info_t *mtu = event->payload;
    for (size_t i = 0u; i < BINDING_CONNECTION_MAX; ++i) {
      if (binding->connections[i].handle == mtu->conn_handle &&
          mtu->mtu >= 23u && mtu->mtu <= H2_PAL_BLE_ATT_MAX_MTU)
        binding->connections[i].mtu = mtu->mtu;
    }
  } else if (event->type == H2_PAL_SYSTEM_EVENT_TYPE_BLE_DISCONNECTED &&
             event->payload != NULL &&
             event->payload_size == sizeof(h2_pal_ble_disconnected_info_t)) {
    const h2_pal_ble_disconnected_info_t *conn = event->payload;
    for (size_t i = 0u; i < BINDING_CONNECTION_MAX; ++i) {
      if (binding->connections[i].handle == conn->conn_handle)
        binding->connections[i].handle = H2_PAL_BLE_INVALID_CONN_HANDLE;
    }
    if (binding->conn_handle == conn->conn_handle) {
      binding->conn_handle = H2_PAL_BLE_INVALID_CONN_HANDLE;
      binding->pending = false;
    }
  } else if (event->type == H2_PAL_SYSTEM_EVENT_TYPE_BLE_HOST_STOPPED) {
    /* PAL Host stop retires all schemas and advertising handles. */
    binding->open = false;
    binding->advertising = false;
    binding->host_stopped = true;
    binding->pending = false;
    binding->conn_handle = H2_PAL_BLE_INVALID_CONN_HANDLE;
    binding->last_error = H2_PAL_ERR_CLOSED;
  } else if ((event->type == H2_PAL_SYSTEM_EVENT_TYPE_BLE_ADVERTISING_STARTED ||
              event->type ==
                  H2_PAL_SYSTEM_EVENT_TYPE_BLE_ADVERTISING_STOPPED) &&
             event->payload != NULL &&
             event->payload_size == sizeof(h2_pal_ble_adv_set_event_t)) {
    const h2_pal_ble_adv_set_event_t *adv = event->payload;
    if (adv->set == binding->adv_set) {
      ++binding->adv_event_revision;
      binding->advertising =
          event->type == H2_PAL_SYSTEM_EVENT_TYPE_BLE_ADVERTISING_STARTED &&
          adv->status == H2_PAL_OK;
      binding->last_error = adv->status;
    }
  }
  unlock(binding);
  return H2_PAL_OK;
}

static void copy_span(char *out, h2_gizclaw_str_t *span) {
  if (span->len != 0u)
    memcpy(out, span->data, span->len);
  out[span->len] = '\0';
  span->data = out;
}

h2_pal_result_t
h2_gizclaw_ble_binding_open(const h2_gizclaw_ble_binding_config_t *config,
                            h2_gizclaw_ble_binding_t **out_binding) {
  if (out_binding != NULL)
    *out_binding = NULL;
  if (config == NULL || out_binding == NULL || config->api_key_state == NULL ||
      config->ble == NULL || config->mem == NULL || config->sync == NULL ||
      config->system_event == NULL || !origin_valid(config->server_origin) ||
      !icon_valid(config->icon) || !name_valid(config->name) ||
      !local_name_valid(config->local_name))
    return H2_PAL_ERR_INVALID_ARG;
  if (config->system_event->vtable == NULL ||
      config->system_event->vtable->subscribe == NULL ||
      config->system_event->vtable->unsubscribe == NULL ||
      config->sync->vtable == NULL ||
      config->sync->vtable->create_mutex == NULL ||
      config->sync->vtable->destroy_mutex == NULL ||
      config->sync->vtable->lock_mutex == NULL ||
      config->sync->vtable->unlock_mutex == NULL)
    return H2_PAL_ERR_UNSUPPORTED;
  h2_gizclaw_ble_binding_t *binding =
      h2_pal_mem_alloc(config->mem, sizeof(*binding));
  if (binding == NULL)
    return H2_PAL_ERR_NO_MEMORY;
  memset(binding, 0, sizeof(*binding));
  binding->config = *config;
  copy_span(binding->origin, &binding->config.server_origin);
  copy_span(binding->icon, &binding->config.icon);
  copy_span(binding->name, &binding->config.name);
  copy_span(binding->local_name, &binding->config.local_name);
  binding->conn_handle = H2_PAL_BLE_INVALID_CONN_HANDLE;
  for (size_t i = 0u; i < BINDING_CONNECTION_MAX; ++i)
    binding->connections[i].handle = H2_PAL_BLE_INVALID_CONN_HANDLE;
  h2_pal_mutex_config_t mutex_config = {.name = "gizclaw/ble_binding",
                                        .allocator = config->mem};
  h2_pal_result_t rc =
      h2_pal_mutex_create(config->sync, &mutex_config, &binding->mutex);
  if (rc != H2_PAL_OK) {
    h2_pal_mem_free(config->mem, binding);
    return rc;
  }
  binding->characteristics[0] = (h2_pal_ble_gatt_characteristic_t){
      .uuid = h2_gizclaw_ble_binding_info_uuid,
      .properties = H2_PAL_BLE_GATT_PROPERTY_READ,
      .permissions = H2_PAL_BLE_GATT_PERMISSION_READ,
      .max_value_len = H2_GIZCLAW_BLE_BINDING_INFO_LEN,
      .read = info_read,
      .user = binding,
      .out_value_handle = &binding->handles[0]};
  binding->characteristics[1] = (h2_pal_ble_gatt_characteristic_t){
      .uuid = h2_gizclaw_ble_binding_request_uuid,
      .properties = H2_PAL_BLE_GATT_PROPERTY_WRITE,
      .permissions = H2_PAL_BLE_GATT_PERMISSION_WRITE,
      .max_value_len = H2_GIZCLAW_BLE_BINDING_REQUEST_LEN,
      .write = request_write,
      .user = binding,
      .out_value_handle = &binding->handles[1]};
  binding->characteristics[2] = (h2_pal_ble_gatt_characteristic_t){
      .uuid = h2_gizclaw_ble_binding_credential_uuid,
      .properties = H2_PAL_BLE_GATT_PROPERTY_READ,
      .permissions = H2_PAL_BLE_GATT_PERMISSION_READ,
      .max_value_len = H2_GIZCLAW_BLE_BINDING_FRAME_MAX,
      .read = credential_read,
      .user = binding,
      .out_value_handle = &binding->handles[2]};
  binding->service =
      (h2_pal_ble_gatt_service_t){.uuid = h2_gizclaw_ble_binding_service_uuid,
                                  .primary = true,
                                  .characteristics = binding->characteristics,
                                  .characteristic_count = 3u};
  *out_binding = binding;
  return H2_PAL_OK;
}

static h2_pal_result_t restart_advertising(h2_gizclaw_ble_binding_t *binding) {
  h2_pal_result_t rc = lock(binding);
  if (rc != H2_PAL_OK)
    return rc;
  uint64_t revision = binding->adv_event_revision;
  h2_pal_ble_adv_set_t *set = binding->adv_set;
  unlock(binding);
  rc = h2_pal_ble_adv_set_start(binding->config.ble, set);
  h2_pal_result_t lock_rc = lock(binding);
  if (lock_rc != H2_PAL_OK)
    return lock_rc;
  if (rc != H2_PAL_OK) {
    binding->last_error = rc;
  } else if (!binding->host_stopped &&
             revision == binding->adv_event_revision) {
    binding->advertising = true;
    binding->last_error = H2_PAL_OK;
  }
  unlock(binding);
  return rc;
}

h2_pal_result_t
h2_gizclaw_ble_binding_start(h2_gizclaw_ble_binding_t *binding) {
  if (binding == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  h2_pal_result_t rc = lock(binding);
  if (rc != H2_PAL_OK)
    return rc;
  bool open = binding->open;
  bool resources = binding->registered || binding->adv_set != NULL ||
                   binding->subscriptions[0] != NULL;
  unlock(binding);
  if (open)
    return H2_PAL_OK;
  if (resources)
    return H2_PAL_ERR_BUSY;
  if (binding->config.ble->vtable == NULL ||
      binding->config.ble->vtable->unregister_gatt_service == NULL)
    return H2_PAL_ERR_UNSUPPORTED;
  const h2_pal_system_event_type_t types[BINDING_SUBSCRIPTION_MAX] = {
      H2_PAL_SYSTEM_EVENT_TYPE_BLE_CONNECTED,
      H2_PAL_SYSTEM_EVENT_TYPE_BLE_DISCONNECTED,
      H2_PAL_SYSTEM_EVENT_TYPE_BLE_MTU_CHANGED,
      H2_PAL_SYSTEM_EVENT_TYPE_BLE_HOST_STOPPED,
      H2_PAL_SYSTEM_EVENT_TYPE_BLE_ADVERTISING_STARTED,
      H2_PAL_SYSTEM_EVENT_TYPE_BLE_ADVERTISING_STOPPED};
  rc = lock(binding);
  if (rc != H2_PAL_OK)
    return rc;
  binding->host_stopped = false;
  unlock(binding);
  for (size_t i = 0u; i < BINDING_SUBSCRIPTION_MAX; ++i) {
    rc = h2_pal_system_event_subscribe(binding->config.system_event, types[i],
                                       on_system_event, binding,
                                       &binding->subscriptions[i]);
    if (rc != H2_PAL_OK)
      goto failed;
  }
  rc = h2_pal_ble_register_gatt_services(binding->config.ble, &binding->service,
                                         1u);
  if (rc != H2_PAL_OK)
    goto failed;
  binding->registered = true;
  h2_pal_ble_adv_params_t params = {.mode = H2_PAL_BLE_ADV_MODE_CONNECTABLE,
                                    .interval_min_ms = 100u,
                                    .interval_max_ms = 200u,
                                    .type = H2_PAL_BLE_ADV_TYPE_LEGACY};
  h2_pal_ble_adv_set_t *set = NULL;
  rc = h2_pal_ble_adv_set_create(binding->config.ble, &params, &set);
  if (rc != H2_PAL_OK)
    goto failed;
  /* Assign under the same lock as handle-scoped lifecycle events. */
  if (lock(binding) != H2_PAL_OK) {
    (void)h2_pal_ble_adv_set_destroy(binding->config.ble, set);
    rc = H2_PAL_ERR_INVALID_STATE;
    goto failed;
  }
  binding->adv_set = set;
  unlock(binding);
  h2_pal_ble_adv_data_t data = {.local_name =
                                  binding->config.local_name.len != 0u
                                      ? binding->local_name : NULL,
                                .service_uuids =
                                    &h2_gizclaw_ble_binding_service_uuid,
                                .service_uuid_count = 1u};
  rc =
      h2_pal_ble_adv_set_set_data(binding->config.ble, binding->adv_set, &data);
  if (rc != H2_PAL_OK)
    goto failed;
  rc = restart_advertising(binding);
  if (rc != H2_PAL_OK)
    goto failed;
  rc = lock(binding);
  if (rc != H2_PAL_OK)
    goto failed;
  if (binding->host_stopped) {
    unlock(binding);
    rc = H2_PAL_ERR_CLOSED;
    goto failed;
  }
  binding->open = true;
  /* Exposure obligations and deduplication span this instance's windows. */
  unlock(binding);
  return H2_PAL_OK;
failed:
  (void)h2_gizclaw_ble_binding_stop(binding);
  if (lock(binding) == H2_PAL_OK) {
    binding->last_error = rc;
    unlock(binding);
  }
  return rc;
}

h2_pal_result_t h2_gizclaw_ble_binding_snapshot(
    h2_gizclaw_ble_binding_t *binding,
    h2_gizclaw_ble_binding_snapshot_t *out_snapshot) {
  if (out_snapshot != NULL)
    memset(out_snapshot, 0, sizeof(*out_snapshot));
  if (binding == NULL || out_snapshot == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  h2_pal_result_t rc = lock(binding);
  if (rc != H2_PAL_OK)
    return rc;
  out_snapshot->open = binding->open;
  out_snapshot->advertising = binding->advertising;
  out_snapshot->connected =
      binding->conn_handle != H2_PAL_BLE_INVALID_CONN_HANDLE;
  out_snapshot->pending_exposures = binding->exposure_count;
  out_snapshot->last_error = binding->last_error;
  h2_gizclaw_api_key_snapshot_t key = {0};
  char url[H2_GIZCLAW_BLE_BINDING_URL_MAX + 1u] = {0};
  size_t len = 0u;
  rc = credential(binding, &key, url, &len);
  out_snapshot->revision = key.revision;
  out_snapshot->ready = binding->open && rc == H2_PAL_OK;
  out_snapshot->url_len = out_snapshot->ready ? len : 0u;
  out_snapshot->info_flags =
      binding->open ? key_status(&key, rc) : H2_GIZCLAW_BLE_BINDING_CLOSED;
  out_snapshot->has_rpc_error = key.has_rpc_error;
  out_snapshot->rpc_error_code = key.rpc_error_code;
  if (rc != H2_PAL_OK && !key.busy)
    out_snapshot->last_error =
        rc == H2_PAL_ERR_INVALID_STATE ? key.last_error : rc;
  erase(&key, sizeof(key));
  erase(url, sizeof(url));
  unlock(binding);
  return H2_PAL_OK;
}

h2_pal_result_t h2_gizclaw_ble_binding_poll(h2_gizclaw_ble_binding_t *binding) {
  if (binding == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  h2_pal_result_t rc = lock(binding);
  if (rc != H2_PAL_OK)
    return rc;
  bool restart = binding->open && !binding->advertising &&
                 !binding->host_stopped && binding->adv_set != NULL &&
                 binding->conn_handle == H2_PAL_BLE_INVALID_CONN_HANDLE;
  unlock(binding);
  if (restart)
    return restart_advertising(binding);
  return H2_PAL_OK;
}

h2_pal_result_t h2_gizclaw_ble_binding_next_exposure(
    h2_gizclaw_ble_binding_t *binding,
    h2_gizclaw_ble_binding_exposure_t *out_exposure) {
  if (out_exposure != NULL)
    memset(out_exposure, 0, sizeof(*out_exposure));
  if (binding == NULL || out_exposure == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  h2_pal_result_t rc = lock(binding);
  if (rc != H2_PAL_OK)
    return rc;
  if (binding->exposure_count == 0u) {
    unlock(binding);
    return H2_PAL_ERR_WOULD_BLOCK;
  }
  *out_exposure = binding->exposures[binding->exposure_head];
  memset(&binding->exposures[binding->exposure_head], 0, sizeof(*out_exposure));
  binding->exposure_head =
      (binding->exposure_head + 1u) % H2_GIZCLAW_BLE_BINDING_EXPOSURE_MAX;
  --binding->exposure_count;
  unlock(binding);
  return H2_PAL_OK;
}

static h2_pal_result_t cleanup_error(h2_gizclaw_ble_binding_t *binding,
                                     h2_pal_result_t rc) {
  if (lock(binding) == H2_PAL_OK) {
    binding->last_error = rc;
    unlock(binding);
  }
  return rc;
}

h2_pal_result_t h2_gizclaw_ble_binding_stop(h2_gizclaw_ble_binding_t *binding) {
  if (binding == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  h2_pal_result_t rc = lock(binding);
  if (rc != H2_PAL_OK)
    return rc;
  binding->open = false;
  binding->pending = false;
  binding->conn_handle = H2_PAL_BLE_INVALID_CONN_HANDLE;
  bool host_stopped = binding->host_stopped;
  unlock(binding);
  /* Never hold the binding mutex across PAL: providers may dispatch events
   * synchronously or wait for a read callback to finish while unbinding. */
  if (!host_stopped && binding->adv_set != NULL) {
    rc = h2_pal_ble_adv_set_stop(binding->config.ble, binding->adv_set);
    if (rc != H2_PAL_OK)
      return cleanup_error(binding, rc);
    rc = h2_pal_ble_adv_set_destroy(binding->config.ble, binding->adv_set);
    if (rc != H2_PAL_OK)
      return cleanup_error(binding, rc);
  }
  rc = lock(binding);
  if (rc != H2_PAL_OK)
    return rc;
  binding->adv_set = NULL;
  unlock(binding);
  if (!host_stopped && binding->registered) {
    rc = h2_pal_ble_unregister_gatt_service(
        binding->config.ble, &h2_gizclaw_ble_binding_service_uuid);
    if (rc != H2_PAL_OK)
      return cleanup_error(binding, rc);
  }
  binding->registered = false;
  for (size_t i = 0u; i < BINDING_SUBSCRIPTION_MAX; ++i) {
    h2_pal_system_event_unsubscribe(binding->config.system_event,
                                    binding->subscriptions[i]);
    binding->subscriptions[i] = NULL;
  }
  rc = lock(binding);
  if (rc != H2_PAL_OK)
    return rc;
  binding->advertising = false;
  for (size_t i = 0u; i < BINDING_CONNECTION_MAX; ++i)
    binding->connections[i].handle = H2_PAL_BLE_INVALID_CONN_HANDLE;
  unlock(binding);
  return H2_PAL_OK;
}

h2_pal_result_t
h2_gizclaw_ble_binding_close(h2_gizclaw_ble_binding_t **binding) {
  if (binding == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  if (*binding == NULL)
    return H2_PAL_OK;
  h2_gizclaw_ble_binding_t *instance = *binding;
  h2_pal_result_t rc = h2_gizclaw_ble_binding_stop(instance);
  if (rc != H2_PAL_OK)
    return rc;
  if (instance->exposure_count != 0u)
    return H2_PAL_ERR_BUSY;
  rc = h2_pal_mutex_destroy(instance->config.sync, instance->mutex);
  if (rc != H2_PAL_OK)
    return rc;
  const h2_pal_mem_api_t *mem = instance->config.mem;
  erase(instance, sizeof(*instance));
  h2_pal_mem_free(mem, instance);
  *binding = NULL;
  return H2_PAL_OK;
}
