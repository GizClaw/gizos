#pragma once
#include <stdint.h>
#define BK_OK 0
#define MAC_TYPE_BLUETOOTH 2
static inline int bk_get_mac(uint8_t *mac, int type) {
  (void)mac;
  (void)type;
  return BK_OK;
}
