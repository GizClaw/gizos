#include "ble_peer_transfer.h"
#include "h2_gizclaw_ble_binding.h"
#include "h2_sha256.h"

#include <assert.h>
#include <string.h>

/* Compiled protocol fault tests. This PAL peer is never live BLE evidence. */
static const char url[] = "https://e2e.example/api-keys/gizclaw_sk_v1_fixture?name=GizOS%20E2E";
typedef struct peer {
  unsigned fault, writes;
  size_t offset;
  bool pending;
} peer_t;

static void put64(uint8_t *bytes, uint64_t value) {
  for (unsigned i = 0u; i < 8u; ++i) bytes[i] = (uint8_t)(value >> (8u * i));
}

static int discover(void *user, uint16_t connection,
    const h2_pal_ble_gatt_discovery_request_t *request,
    h2_pal_ble_gatt_discovery_entry_t *entries, size_t capacity,
    size_t *count, uint32_t timeout) {
  peer_t *peer = user;
  assert(connection == 7u && capacity == 1u && timeout);
  *count = peer->fault == 1u ? 0u : 1u;
  *entries = (h2_pal_ble_gatt_discovery_entry_t){.kind = request->kind,
      .uuid = request->uuid_filter, .start_handle = 1u, .end_handle = 9u,
      .properties = H2_PAL_BLE_GATT_PROPERTY_READ};
  const h2_pal_ble_uuid_t uuids[] = {h2_gizclaw_ble_binding_info_uuid,
      h2_gizclaw_ble_binding_request_uuid, h2_gizclaw_ble_binding_credential_uuid};
  for (size_t i = 0u; i < 3u; ++i)
    if (request->uuid_filter.len == uuids[i].len &&
        !memcmp(request->uuid_filter.data, uuids[i].data, uuids[i].len)) {
      entries->value_handle = (uint16_t)(i + 2u);
      if (i == 1u) entries->properties = H2_PAL_BLE_GATT_PROPERTY_WRITE;
    }
  if (peer->fault == 2u) entries->kind = H2_PAL_BLE_GATT_DISCOVERY_DESCRIPTOR;
  if (peer->fault == 3u) entries->start_handle = 0u;
  if (peer->fault == 4u) entries->properties = 0u;
  return H2_PAL_OK;
}

static int write_request(void *user, uint16_t connection, uint16_t handle,
    const uint8_t *bytes, size_t length, bool response, uint32_t timeout) {
  peer_t *peer = user;
  assert(connection == 7u && handle == 3u && length == 13u && response && timeout);
  assert(bytes[0] == 1u && bytes[1] == 42u && bytes[11] == 8u);
  peer->offset = bytes[9] | (size_t)bytes[10] << 8u;
  assert(peer->offset < sizeof(url)); peer->pending = true; ++peer->writes;
  return peer->fault == 11u ? H2_PAL_ERR_IO : H2_PAL_OK;
}

static int read_value(void *user, uint16_t connection, uint16_t handle,
    uint16_t offset, uint8_t *out, size_t capacity, size_t *length, uint32_t timeout) {
  peer_t *peer = user;
  assert(connection == 7u && !offset && capacity >= 244u && timeout);
  memset(out, 0, capacity); out[0] = 1u;
  if (handle == 2u) {
    out[1] = H2_GIZCLAW_BLE_BINDING_READY; put64(out + 2u, 42u);
    out[10] = (uint8_t)(sizeof(url) - 1u); out[12] = 8u; *length = 14u;
    if (peer->fault == 5u) out[0] = 2u;
    if (peer->fault == 6u) out[1] = H2_GIZCLAW_BLE_BINDING_BUSY;
    if (peer->fault == 7u) out[2] = 41u;
    if (peer->fault == 8u) ++out[10];
    if (peer->fault == 9u) out[12] = 0u;
    if (peer->fault == 10u) out[12] = 232u;
    return H2_PAL_OK;
  }
  assert(handle == 4u && peer->pending); peer->pending = false;
  put64(out + 1u, 42u); out[9] = (uint8_t)peer->offset;
  out[11] = (uint8_t)(sizeof(url) - 1u);
  size_t payload = sizeof(url) - 1u - peer->offset;
  if (payload > 8u) payload = 8u;
  memcpy(out + 13u, url + peer->offset, payload); *length = 13u + payload;
  if (peer->fault == 12u) return H2_PAL_ERR_IO;
  if (peer->fault == 13u) *length = 12u;
  if (peer->fault == 14u) out[0] = 2u;
  if (peer->fault == 15u) out[1] = 41u;
  if (peer->fault == 16u) ++out[9];
  if (peer->fault == 17u) ++out[11];
  if (peer->fault == 18u) *length = 13u;
  if (peer->fault == 19u) *length = 22u;
  if (peer->fault == 20u && peer->offset == sizeof(url) - 1u) *length = 14u;
  if (peer->fault == 21u && payload) out[13] ^= 1u;
  return H2_PAL_OK;
}

int main(void) {
  const h2_pal_ble_vtable_t vtable = {.gatt_discover = discover,
      .gatt_read = read_value, .gatt_write = write_request};
  uint8_t digest[32]; char hex[65]; h2_sha256_t sha;
  h2_sha256_init(&sha);
  h2_sha256_update(&sha, (const uint8_t *)url, sizeof(url) - 1u);
  h2_sha256_finish(&sha, digest); h2_sha256_hex(digest, hex);
  h2_gizclaw_e2e_ble_peer_expected_t expected = {
      .revision = 42u, .url_bytes = sizeof(url) - 1u, .url_sha256 = hex};
  for (unsigned fault = 0u; fault <= 21u; ++fault) {
    peer_t peer = {.fault = fault};
    const h2_pal_ble_host_api_t ble = {.vtable = &vtable, .user = &peer};
    size_t chunks = 999u;
    int rc = h2_gizclaw_e2e_ble_peer_transfer(&ble, 7u, &expected, &chunks);
    if (!fault) {
      assert(rc == H2_PAL_OK && chunks == (sizeof(url) - 2u) / 8u + 1u);
      assert(peer.writes == chunks + 1u && !peer.pending);
    } else assert(rc != H2_PAL_OK);
  }
  size_t chunks = 999u;
  assert(h2_gizclaw_e2e_ble_peer_transfer(NULL, 7u, &expected, &chunks) == H2_PAL_ERR_INVALID_ARG);
  assert(!chunks);
  return 0;
}
