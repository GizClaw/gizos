#include "h2_gizclaw_ble_peer.h"
#include "h2_gizclaw_ble_peer_protocol.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#define PEER_WINDOW_MS 1200000u
/* Independent implementation of the published v1 wire UUIDs. */
static const uint8_t service_uuid[16] = {0x36, 0x3b, 0xf8, 0xe4, 0x2b, 0xb1,
                                         0x2f, 0x8c, 0x0d, 0x4b, 0xa6, 0x91,
                                         0xb0, 0xd4, 0xed, 0x14};
typedef struct peer_context {
  h2_runtime_t* rt;
  h2_pal_mutex_t* mutex;
  uint8_t address[6];
  h2_pal_ble_addr_t observed;
  char name[30], previous[30];
  bool service, connectable, found, scanning, host_started;
  unsigned packets;
  int rssi;
  uint16_t connection, last_disconnected;
  h2_pal_system_event_subscription_t* disconnected;
} peer_context_t;
static peer_context_t context;
static int log_line(peer_context_t* c, const char* fmt, ...) {
  char line[384];
  va_list args;
  va_start(args, fmt);
  int n = vsnprintf(line, sizeof(line), fmt, args);
  va_end(args);
  if (n < 0 || (size_t)n >= sizeof(line)) return H2_PAL_ERR_TRUNCATED;
  return h2_pal_log_write(c->rt->log, H2_PAL_LOG_INFO, "ble-peer", line);
}
/* The fixture observes raw System Events, so consume the independent Runtime
 * bridge as well; repeated scan lifecycles must not fill its bounded queue. */
static void drain_events(peer_context_t* c) {
  union {
    uint64_t align;
    uint8_t bytes[H2_RUNTIME_EVENT_PAYLOAD_MAX];
  } payload;
  h2_runtime_event_t event = {.payload = payload.bytes,
                              .payload_capacity = sizeof(payload.bytes)};
  while (h2_runtime_poll_event(c->rt, &event) == H2_PAL_OK) {
  }
  memset(&payload, 0, sizeof(payload));
}
static uint64_t monotonic(peer_context_t* c) {
  uint64_t n = 0u;
  (void)h2_pal_time_get_monotonic_ms(c->rt->time, &n);
  return n;
}
static bool name_valid(const char* p, size_t n) {
  if (!p || n != 22u || memcmp(p, "h2e2e-", 6u)) return false;
  for (size_t i = 6u; i < n; ++i)
    if (!((p[i] >= '0' && p[i] <= '9') || (p[i] >= 'a' && p[i] <= 'f')))
      return false;
  return true;
}
static bool on_scan(void* user, const h2_pal_ble_scan_result_t* result) {
  peer_context_t* c = user;
  if (!result || memcmp(result->addr.value, c->address, 6u)) return false;
  if (h2_pal_mutex_lock(c->rt->sync, c->mutex)) return false;
  ++c->packets;
  c->observed = result->addr;
  c->rssi = result->rssi;
  c->connectable |= result->connectable;
  for (size_t i = 0u; i < result->service_uuid_count; ++i)
    if (result->service_uuids[i].len == 16u &&
        !memcmp(result->service_uuids[i].data, service_uuid, 16u))
      c->service = true;
  if (name_valid(result->local_name, result->local_name_len)) {
    memcpy(c->name, result->local_name, result->local_name_len);
    c->name[result->local_name_len] = 0;
  }
  c->found = c->service && c->connectable && c->name[0] &&
             strcmp(c->previous, c->name);
  (void)h2_pal_mutex_unlock(c->rt->sync, c->mutex);
  return false;
}
static int on_disconnected(void* user, const h2_pal_system_event_t* event) {
  peer_context_t* c = user;
  if (event->type == H2_PAL_SYSTEM_EVENT_TYPE_BLE_DISCONNECTED &&
      event->payload &&
      event->payload_size == sizeof(h2_pal_ble_disconnected_info_t)) {
    const h2_pal_ble_disconnected_info_t* info = event->payload;
    int rc = h2_pal_mutex_lock(c->rt->sync, c->mutex);
    if (rc) return rc;
    c->last_disconnected = info->conn_handle;
    (void)h2_pal_mutex_unlock(c->rt->sync, c->mutex);
  }
  return H2_PAL_OK;
}
static int disconnect_peer(peer_context_t* c) {
  if (c->connection == H2_PAL_BLE_INVALID_CONN_HANDLE) return H2_PAL_OK;
  uint16_t handle = c->connection;
  int rc = h2_pal_ble_disconnect(c->rt->ble_host, handle);
  uint64_t start = monotonic(c);
  while (monotonic(c) - start < 3000u) {
    int locked = h2_pal_mutex_lock(c->rt->sync, c->mutex);
    if (locked) return locked;
    bool done = c->last_disconnected == handle;
    (void)h2_pal_mutex_unlock(c->rt->sync, c->mutex);
    if (done) {
      c->connection = H2_PAL_BLE_INVALID_CONN_HANDLE;
      return H2_PAL_OK;
    }
    if (rc) return rc;
    (void)h2_pal_time_sleep_ms(c->rt->time, 10u);
  }
  return H2_PAL_ERR_TIMEOUT;
}
static int read_credential(peer_context_t* c, const char* uid,
                           const char* origin) {
  const h2_pal_ble_connect_params_t params = {.timeout_ms = 8000u,
                                              .interval_min_ms = 30u,
                                              .interval_max_ms = 50u,
                                              .latency = 0u,
                                              .supervision_timeout_ms = 4000u};
  c->last_disconnected = H2_PAL_BLE_INVALID_CONN_HANDLE;
  int rc = h2_pal_ble_connect(c->rt->ble_host, &c->observed, &params,
                              &c->connection);
  h2_pal_ble_gatt_discovery_entry_t entries[8];
  size_t count = 0u;
  h2_pal_ble_gatt_discovery_request_t request = {
      .kind = H2_PAL_BLE_GATT_DISCOVERY_SERVICE,
      .uuid_filter = {service_uuid, 16u},
      .start_handle = 1u,
      .end_handle = 0xffffu};
  if (!rc)
    rc = h2_pal_ble_gatt_discover(c->rt->ble_host, c->connection, &request,
                                  entries, 8u, &count, 5000u);
  if (!rc && (count != 1u || !entries[0].start_handle ||
              entries[0].end_handle < entries[0].start_handle))
    rc = H2_PAL_ERR_FORMAT;
  if (!rc) {
    request = (h2_pal_ble_gatt_discovery_request_t){
        .kind = H2_PAL_BLE_GATT_DISCOVERY_CHARACTERISTIC,
        .start_handle = entries[0].start_handle,
        .end_handle = entries[0].end_handle};
    rc = h2_pal_ble_gatt_discover(c->rt->ble_host, c->connection, &request,
                                  entries, 8u, &count, 5000u);
  }
  uint16_t handles[3] = {0u};
  for (size_t i = 0u; !rc && i < count; ++i)
    for (unsigned k = 0u; k < 3u; ++k) {
      uint8_t uuid[16];
      memcpy(uuid, service_uuid, 16u);
      uuid[12] = (uint8_t)(0xb1u + k);
      if (entries[i].uuid.len == 16u &&
          !memcmp(entries[i].uuid.data, uuid, 16u))
        handles[k] = entries[i].value_handle;
    }
  if (!rc && (!handles[0] || !handles[1] || !handles[2]))
    rc = H2_PAL_ERR_FORMAT;
  uint8_t frame[244] = {0}, url[H2_BLE_PEER_URL_MAX + 1u] = {0};
  size_t length = 0u, received = 0u;
  h2_ble_peer_info_t info = {0};
  if (!rc)
    rc = h2_pal_ble_gatt_read(c->rt->ble_host, c->connection, handles[0], 0u,
                              frame, sizeof(frame), &length, 5000u);
  if (!rc) rc = h2_ble_peer_parse_info(frame, length, &info);
  uint16_t limit = info.limit < 32u ? info.limit : 32u;
  unsigned chunks = 0u;
  uint64_t start = monotonic(c);
  while (!rc && received < info.length) {
    uint8_t packet[13] = {1u};
    for (unsigned i = 0u; i < 8u; ++i)
      packet[1u + i] = (uint8_t)(info.revision >> (8u * i));
    packet[9] = (uint8_t)received;
    packet[10] = (uint8_t)(received >> 8u);
    packet[11] = (uint8_t)limit;
    packet[12] = (uint8_t)(limit >> 8u);
    rc = h2_pal_ble_gatt_write(c->rt->ble_host, c->connection, handles[1],
                               packet, sizeof(packet), true, 5000u);
    if (!rc)
      rc = h2_pal_ble_gatt_read(c->rt->ble_host, c->connection, handles[2], 0u,
                                frame, sizeof(frame), &length, 5000u);
    if (!rc)
      rc = h2_ble_peer_append(frame, length, &info, limit, url, &received);
    if (!rc) ++chunks;
    drain_events(c);
    if (!rc && monotonic(c) - start > 30000u) rc = H2_PAL_ERR_TIMEOUT;
  }
  if (!rc) rc = h2_ble_peer_validate_url(url, received, origin);
  uint8_t digest[32] = {0};
  char hex[65] = {0};
  static const uint8_t salt[] = "gizclaw-ble-e2e-v1";
  if (!rc)
    rc = h2_pal_crypto_hkdf_sha256(c->rt->crypto, url, received, salt,
                                   sizeof(salt) - 1u, NULL, 0u, digest,
                                   sizeof(digest));
  for (unsigned i = 0u; i < 32u; ++i)
    (void)snprintf(hex + 2u * i, 3u, "%02x", digest[i]);
  memset(url, 0, sizeof(url));
  memset(frame, 0, sizeof(frame));
  int disconnected = disconnect_peer(c);
  if (!rc) rc = disconnected;
  if (!rc)
    rc = log_line(
        c,
        "H2_BLE_PEER_TRANSFER uid=%s name=%s revision=%llu bytes=%u chunks=%u fingerprint=%s rc=0 disconnected=1",
        uid, c->name, (unsigned long long)info.revision, (unsigned)received,
        chunks, hex);
  if (rc)
    (void)log_line(c, "H2_BLE_PEER_TRANSFER_FAIL uid=%s rc=%d disconnected=%d",
                   uid, rc, disconnected == H2_PAL_OK);
  return rc;
}
int h2_gizclaw_ble_peer_run(h2_runtime_t* rt, const char* uid,
                            const char* origin, unsigned transfers) {
  peer_context_t* c = &context;
  if (c->rt) return H2_PAL_ERR_INVALID_STATE;
  if (!rt || !origin || strncmp(origin, "https://", 8u) || !origin[8] ||
      strlen(origin) > 192u || strpbrk(origin + 8, "/?#@ \r\n\t") ||
      !transfers || transfers > 2u)
    return H2_PAL_ERR_INVALID_ARG;
  *c = (peer_context_t){.rt = rt,
                        .connection = H2_PAL_BLE_INVALID_CONN_HANDLE,
                        .last_disconnected = H2_PAL_BLE_INVALID_CONN_HANDLE};
  int rc = h2_ble_peer_parse_uid(uid, c->address);
  if (rc) return rc;
  const h2_pal_mutex_config_t mutex = {.name = "gizclaw/ble-peer",
                                       .allocator = rt->mem};
  rc = h2_pal_mutex_create(rt->sync, &mutex, &c->mutex);
  if (!rc)
    rc = h2_pal_system_event_subscribe(
        rt->system_event, H2_PAL_SYSTEM_EVENT_TYPE_BLE_DISCONNECTED,
        on_disconnected, c, &c->disconnected);
  if (!rc) {
    rc = h2_pal_ble_start(rt->ble_host);
    c->host_started = !rc;
  }
  uint64_t start = monotonic(c);
  unsigned completed = 0u;
  bool announced = false;
  while (!rc && completed < transfers &&
         monotonic(c) - start < PEER_WINDOW_MS) {
    drain_events(c);
    if (h2_pal_mutex_lock(rt->sync, c->mutex)) {
      rc = H2_PAL_ERR_IO;
      break;
    }
    c->service = false;
    c->connectable = false;
    c->found = false;
    c->name[0] = 0;
    c->packets = 0u;
    (void)h2_pal_mutex_unlock(rt->sync, c->mutex);
    const h2_pal_ble_scan_params_t scan = {.mode = H2_PAL_BLE_SCAN_MODE_ACTIVE,
                                           .type = H2_PAL_BLE_SCAN_TYPE_LEGACY,
                                           .interval_ms = 50u,
                                           .window_ms = 40u,
                                           .timeout_ms = 3000u};
    rc = h2_pal_ble_start_scan(rt->ble_host, &scan, on_scan, c);
    c->scanning = !rc;
    if (!rc && !announced) {
      rc = log_line(c, "H2_BLE_PEER_READY uid=%s transfers=%u window_ms=%u",
                    uid, transfers, PEER_WINDOW_MS);
      announced = !rc;
    }
    uint64_t scan_start = monotonic(c);
    bool found = false;
    while (!rc && monotonic(c) - scan_start < 3000u) {
      rc = h2_pal_mutex_lock(rt->sync, c->mutex);
      if (rc) break;
      found = c->found;
      (void)h2_pal_mutex_unlock(rt->sync, c->mutex);
      if (found) break;
      rc = h2_pal_time_sleep_ms(rt->time, 20u);
    }
    int stop = h2_pal_ble_stop_scan(rt->ble_host);
    if (!stop) c->scanning = false;
    if (!rc) rc = stop;
    if (!rc && found) {
      rc = read_credential(c, uid, origin);
      if (!rc) {
        ++completed;
        memcpy(c->previous, c->name, sizeof(c->previous));
      }
    } else if (!rc && c->packets)
      (void)log_line(
          c,
          "H2_BLE_PEER_SCAN uid=%s packets=%u service=%u connectable=%u name=%s rssi=%d",
          uid, c->packets, c->service, c->connectable,
          c->name[0] ? c->name : "-", c->rssi);
    if (!rc) rc = h2_pal_time_sleep_ms(rt->time, 100u);
  }
  if (!rc && completed != transfers) rc = H2_PAL_ERR_TIMEOUT;
  int cleanup = c->scanning ? h2_pal_ble_stop_scan(rt->ble_host) : H2_PAL_OK;
  int next = disconnect_peer(c);
  if (!cleanup) cleanup = next;
  if (!cleanup && c->host_started) {
    cleanup = h2_pal_ble_stop(rt->ble_host);
    if (!cleanup) c->host_started = false;
  }
  if (!cleanup && c->disconnected) {
    h2_pal_system_event_unsubscribe(rt->system_event, c->disconnected);
    c->disconnected = NULL;
  }
  if (!cleanup && c->mutex) {
    cleanup = h2_pal_mutex_destroy(rt->sync, c->mutex);
    if (!cleanup) c->mutex = NULL;
  }
  (void)log_line(
      c,
      "H2_BLE_PEER_CLEANUP transfers=%u cleanup=%d scan_stopped=%u disconnected=%u host_stopped=%u",
      completed, cleanup, !c->scanning,
      c->connection == H2_PAL_BLE_INVALID_CONN_HANDLE, !c->host_started);
  return rc ? rc : cleanup;
}
