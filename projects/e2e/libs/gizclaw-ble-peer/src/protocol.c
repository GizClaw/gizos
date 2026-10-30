#include "h2_gizclaw_ble_peer_protocol.h"
#include "h2/pal/core/h2_pal_errors.h"
#include <string.h>
static uint16_t u16(const uint8_t* p) {
  return (uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8u);
}
static uint64_t u64(const uint8_t* p) {
  uint64_t n = 0u;
  for (unsigned i = 0u; i < 8u; ++i) n |= (uint64_t)p[i] << (8u * i);
  return n;
}
int h2_ble_peer_parse_info(const uint8_t* p, size_t length,
                           h2_ble_peer_info_t* info) {
  if (!p || !info || length != 14u) return H2_PAL_ERR_FORMAT;
  *info = (h2_ble_peer_info_t){
      .revision = u64(p + 2), .length = u16(p + 10), .limit = u16(p + 12)};
  return p[0] == 1u && p[1] == 1u && info->revision && info->length &&
                 info->length <= H2_BLE_PEER_URL_MAX && info->limit &&
                 info->limit <= 231u
             ? H2_PAL_OK
             : H2_PAL_ERR_FORMAT;
}
int h2_ble_peer_append(const uint8_t* p, size_t length,
                       const h2_ble_peer_info_t* info, uint16_t limit,
                       uint8_t* url, size_t* received) {
  if (!p || !info || !url || !received || length <= 13u ||
      length - 13u > limit || p[0] != 1u || u64(p + 1) != info->revision ||
      u16(p + 9) != *received || u16(p + 11) != info->length ||
      *received > info->length || length - 13u > info->length - *received)
    return H2_PAL_ERR_FORMAT;
  memcpy(url + *received, p + 13, length - 13u);
  *received += length - 13u;
  return H2_PAL_OK;
}
static int token(uint8_t c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || c == '_' || c == '-';
}
int h2_ble_peer_validate_url(const uint8_t* p, size_t length,
                             const char* origin) {
  static const char path[] = "/api-keys/", prefix[] = "gizclaw_sk_v1_",
                    query[] = "?icon=gizos&name=GizOS%20E2E";
  if (!p || !origin || strncmp(origin, "https://", 8u) ||
      length > H2_BLE_PEER_URL_MAX)
    return H2_PAL_ERR_FORMAT;
  size_t root = strlen(origin), base = root + sizeof(path) - 1u;
  if (length < base + 15u + sizeof(query) - 1u || memcmp(p, origin, root) ||
      memcmp(p + root, path, sizeof(path) - 1u))
    return H2_PAL_ERR_FORMAT;
  size_t end = length - (sizeof(query) - 1u), key_length = end - base;
  if (key_length < 15u || key_length > 95u ||
      memcmp(p + base, prefix, sizeof(prefix) - 1u) ||
      memcmp(p + end, query, sizeof(query) - 1u))
    return H2_PAL_ERR_FORMAT;
  for (size_t i = base; i < end; ++i)
    if (!token(p[i])) return H2_PAL_ERR_FORMAT;
  return H2_PAL_OK;
}
static int digit(char c) {
  return c >= '0' && c <= '9'   ? c - '0'
         : c >= 'a' && c <= 'f' ? c - 'a' + 10
                                : -1;
}
int h2_ble_peer_parse_uid(const char* uid, uint8_t out[6]) {
  if (!uid || !out || strlen(uid) != 12u) return H2_PAL_ERR_INVALID_ARG;
  for (unsigned i = 0u; i < 6u; ++i) {
    int a = digit(uid[2u * i]), b = digit(uid[2u * i + 1u]);
    if (a < 0 || b < 0) return H2_PAL_ERR_INVALID_ARG;
    out[5u - i] = (uint8_t)(16 * a + b);
  }
  return H2_PAL_OK;
}
