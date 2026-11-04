#ifndef H2_TEST_BK_OBSERVATION_SYSTEM_H
#define H2_TEST_BK_OBSERVATION_SYSTEM_H

#include <stdint.h>

#define BK_OK 0
typedef enum {
  MAC_TYPE_WIFI_STA,
  MAC_TYPE_WIFI_AP,
  MAC_TYPE_BLUETOOTH
} mac_type_t;
int bk_get_mac(uint8_t* mac, mac_type_t type);

#endif
