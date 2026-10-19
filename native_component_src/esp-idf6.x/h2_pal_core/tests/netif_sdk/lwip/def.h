#ifndef TEST_NETIF_DEF_H
#define TEST_NETIF_DEF_H
#include <stdint.h>
static inline uint32_t lwip_ntohl(uint32_t value) {
  const uint8_t *bytes = (const uint8_t *)&value;
  return (uint32_t)bytes[0] << 24u | (uint32_t)bytes[1] << 16u |
         (uint32_t)bytes[2] << 8u | bytes[3];
}
#endif
