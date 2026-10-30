#ifndef H2_GIZCLAW_BLE_PEER_PROTOCOL_H
#define H2_GIZCLAW_BLE_PEER_PROTOCOL_H
#include <stddef.h>
#include <stdint.h>
#define H2_BLE_PEER_URL_MAX 1024u
typedef struct h2_ble_peer_info {
  uint64_t revision;
  uint16_t length, limit;
} h2_ble_peer_info_t;
int h2_ble_peer_parse_info(const uint8_t* bytes, size_t length,
                           h2_ble_peer_info_t* info);
int h2_ble_peer_append(const uint8_t* frame, size_t length,
                       const h2_ble_peer_info_t* info, uint16_t limit,
                       uint8_t* url, size_t* received);
int h2_ble_peer_validate_url(const uint8_t* url, size_t length,
                             const char* origin);
int h2_ble_peer_parse_uid(const char* uid, uint8_t little_endian[6]);
#endif
