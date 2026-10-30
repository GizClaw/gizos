#include "h2_gizclaw_ble_peer_protocol.h"
#include "h2/pal/core/h2_pal_errors.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <string.h>
static void put64(uint8_t* p, uint64_t n) {
  for (unsigned i = 0; i < 8u; ++i) p[i] = (uint8_t)(n >> (8u * i));
}
int main(void) {
  uint8_t address[6] = {0};
  const uint8_t expected[] = {0xfa, 0xd4, 0xca, 0x3f, 0x1f, 0xb8};
  assert(h2_ble_peer_parse_uid("b81f3fcad4fa", address) == 0 &&
         !memcmp(address, expected, 6));
  assert(h2_ble_peer_parse_uid("wrong", address) == H2_PAL_ERR_INVALID_ARG);
  assert(h2_ble_peer_parse_uid("b81f3fcad4fg", address) ==
         H2_PAL_ERR_INVALID_ARG);
  uint8_t info_wire[14] = {1, 1};
  put64(info_wire + 2, 7u);
  info_wire[10] = 4u;
  info_wire[12] = 8u;
  h2_ble_peer_info_t info;
  assert(h2_ble_peer_parse_info(info_wire, 14, &info) == 0 &&
         info.revision == 7u && info.length == 4u && info.limit == 8u);
  assert(h2_ble_peer_parse_info(info_wire, 13, &info) == H2_PAL_ERR_FORMAT);
  info_wire[1] = 2;
  assert(h2_ble_peer_parse_info(info_wire, 14, &info) == H2_PAL_ERR_FORMAT);
  info_wire[1] = 1;
  assert(h2_ble_peer_parse_info(info_wire, 14, &info) == 0);
  uint8_t frame[17] = {1}, url[1025] = {0};
  put64(frame + 1, 7u);
  frame[11] = 4u;
  memcpy(frame + 13, "test", 4);
  size_t received = 0;
  assert(h2_ble_peer_append(frame, 17, &info, 8, url, &received) == 0 &&
         received == 4u && !memcmp(url, "test", 4));
  assert(h2_ble_peer_append(frame, 17, &info, 8, url, &received) ==
         H2_PAL_ERR_FORMAT);
  received = 0;
  frame[1] = 8;
  assert(h2_ble_peer_append(frame, 17, &info, 8, url, &received) ==
         H2_PAL_ERR_FORMAT);
  frame[1] = 7;
  assert(h2_ble_peer_append(frame, 13, &info, 8, url, &received) ==
         H2_PAL_ERR_FORMAT);
  assert(h2_ble_peer_append(frame, 17, &info, 2, url, &received) ==
         H2_PAL_ERR_FORMAT);
  static const char root
      [] = "https://e2e.example.test",
  credential[] =
      "https://e2e.example.test/api-keys/gizclaw_sk_v1_test_only_001?icon=gizos&name=GizOS%20E2E";
  assert(h2_ble_peer_validate_url((const uint8_t*)credential,
                                  strlen(credential), root) == 0);
  assert(h2_ble_peer_validate_url((const uint8_t*)credential,
                                  strlen(credential) - 1,
                                  root) == H2_PAL_ERR_FORMAT);
  assert(h2_ble_peer_validate_url((const uint8_t*)credential,
                                  strlen(credential),
                                  "https://other.test") == H2_PAL_ERR_FORMAT);
  return 0;
}
