#include "ble_peer_transfer.h"

#include "h2_gizclaw_ble_binding.h"
#include "h2_sha256.h"

#include <string.h>

enum { PEER_IO_MS = 5000u };

static uint16_t u16(const uint8_t *p) {
  return (uint16_t)((uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8u));
}

static uint64_t u64(const uint8_t *p) {
  uint64_t value = 0u;
  for (unsigned i = 0u; i < 8u; ++i) value |= (uint64_t)p[i] << (8u * i);
  return value;
}

static int discover(const h2_pal_ble_host_api_t *ble, uint16_t connection,
                     h2_pal_ble_gatt_discovery_kind_t kind,
                     h2_pal_ble_uuid_t uuid, uint16_t start, uint16_t end,
                     h2_pal_ble_gatt_discovery_entry_t *entry) {
  const h2_pal_ble_gatt_discovery_request_t request = {
      .kind = kind, .uuid_filter = uuid, .start_handle = start, .end_handle = end};
  size_t count = 0u;
  int rc = h2_pal_ble_gatt_discover(ble, connection, &request, entry, 1u,
                                     &count, PEER_IO_MS);
  if (rc != H2_PAL_OK) return rc;
  return count == 1u && entry->kind == kind && entry->uuid.len == uuid.len &&
                 entry->uuid.data && !memcmp(entry->uuid.data, uuid.data, uuid.len)
             ? H2_PAL_OK : H2_PAL_ERR_FORMAT;
}

int h2_gizclaw_e2e_ble_peer_transfer(
    const h2_pal_ble_host_api_t *ble, uint16_t connection,
    const h2_gizclaw_e2e_ble_peer_expected_t *expected, size_t *out_chunks) {
  if (out_chunks) *out_chunks = 0u;
  if (!ble || !expected || !out_chunks || !expected->revision ||
      !expected->url_bytes || expected->url_bytes > H2_GIZCLAW_BLE_BINDING_URL_MAX ||
      !expected->url_sha256 || strlen(expected->url_sha256) != 64u)
    return H2_PAL_ERR_INVALID_ARG;
  h2_pal_ble_gatt_discovery_entry_t service = {0}, chars[3] = {0};
  int rc = discover(ble, connection, H2_PAL_BLE_GATT_DISCOVERY_SERVICE,
                    h2_gizclaw_ble_binding_service_uuid, 1u, UINT16_MAX, &service);
  if (rc == H2_PAL_OK && (!service.start_handle || service.end_handle < service.start_handle))
    rc = H2_PAL_ERR_FORMAT;
  const h2_pal_ble_uuid_t uuids[] = {h2_gizclaw_ble_binding_info_uuid,
      h2_gizclaw_ble_binding_request_uuid, h2_gizclaw_ble_binding_credential_uuid};
  for (size_t i = 0u; rc == H2_PAL_OK && i < 3u; ++i) {
    rc = discover(ble, connection, H2_PAL_BLE_GATT_DISCOVERY_CHARACTERISTIC,
                   uuids[i], service.start_handle, service.end_handle, &chars[i]);
    if (rc == H2_PAL_OK && (chars[i].value_handle == 0u ||
        !(chars[i].properties & (i == 1u ? H2_PAL_BLE_GATT_PROPERTY_WRITE
                                        : H2_PAL_BLE_GATT_PROPERTY_READ))))
      rc = H2_PAL_ERR_FORMAT;
  }
  uint8_t bytes[H2_GIZCLAW_BLE_BINDING_FRAME_MAX] = {0};
  char url[H2_GIZCLAW_BLE_BINDING_URL_MAX + 1u] = {0};
  size_t length = 0u, offset = 0u;
  uint16_t limit = 0u;
  if (rc == H2_PAL_OK)
    rc = h2_pal_ble_gatt_read(ble, connection, chars[0].value_handle, 0u,
                               bytes, sizeof(bytes), &length, PEER_IO_MS);
  if (rc == H2_PAL_OK) {
    if (length != H2_GIZCLAW_BLE_BINDING_INFO_LEN || bytes[0] != 1u ||
        bytes[1] != H2_GIZCLAW_BLE_BINDING_READY ||
        u64(bytes + 2u) != expected->revision ||
        u16(bytes + 10u) != expected->url_bytes ||
        !(limit = u16(bytes + 12u)) || limit > 231u)
      rc = H2_PAL_ERR_FORMAT;
  }
  /* Include the explicit empty terminal chunk; no automatic long-read/EOF
   * probe and no offset other than zero is sent to the ATT read operation. */
  while (rc == H2_PAL_OK) {
    uint8_t request[H2_GIZCLAW_BLE_BINDING_REQUEST_LEN] = {1u};
    for (unsigned i = 0u; i < 8u; ++i)
      request[1u + i] = (uint8_t)(expected->revision >> (8u * i));
    request[9] = (uint8_t)offset; request[10] = (uint8_t)(offset >> 8u);
    request[11] = (uint8_t)limit; request[12] = (uint8_t)(limit >> 8u);
    rc = h2_pal_ble_gatt_write(ble, connection, chars[1].value_handle,
                                request, sizeof(request), true, PEER_IO_MS);
    if (rc != H2_PAL_OK) break;
    rc = h2_pal_ble_gatt_read(ble, connection, chars[2].value_handle, 0u,
                               bytes, sizeof(bytes), &length, PEER_IO_MS);
    if (rc != H2_PAL_OK) break;
    if (length < H2_GIZCLAW_BLE_BINDING_HEADER_LEN ||
        length > sizeof(bytes) || bytes[0] != 1u ||
        u64(bytes + 1u) != expected->revision || u16(bytes + 9u) != offset ||
        u16(bytes + 11u) != expected->url_bytes) {
      rc = H2_PAL_ERR_FORMAT; break;
    }
    const size_t payload = length - H2_GIZCLAW_BLE_BINDING_HEADER_LEN;
    if (payload > limit || payload > expected->url_bytes - offset ||
        (payload == 0u) != (offset == expected->url_bytes)) {
      rc = H2_PAL_ERR_FORMAT; break;
    }
    if (payload == 0u) break;
    memcpy(url + offset, bytes + H2_GIZCLAW_BLE_BINDING_HEADER_LEN, payload);
    offset += payload; ++*out_chunks;
  }
  if (rc == H2_PAL_OK) {
    uint8_t digest[32]; char hex[65]; h2_sha256_t sha;
    h2_sha256_init(&sha);
    h2_sha256_update(&sha, (const uint8_t *)url, offset);
    h2_sha256_finish(&sha, digest); h2_sha256_hex(digest, hex);
    if (offset != expected->url_bytes || strncmp(url, "https://", 8u) ||
        strcmp(hex, expected->url_sha256)) rc = H2_PAL_ERR_FORMAT;
  }
  volatile unsigned char *secret = (volatile unsigned char *)url;
  for (size_t i = 0u; i < sizeof(url); ++i) secret[i] = 0u;
  secret = bytes;
  for (size_t i = 0u; i < sizeof(bytes); ++i) secret[i] = 0u;
  return rc;
}
