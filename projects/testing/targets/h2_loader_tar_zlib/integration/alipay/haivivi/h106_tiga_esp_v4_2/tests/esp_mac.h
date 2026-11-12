#ifndef H2_ALIPAY_TEST_ESP_MAC_H
#define H2_ALIPAY_TEST_ESP_MAC_H

#include <stdint.h>

#define ESP_OK 0
typedef enum esp_mac_type {
  ESP_MAC_BT = 2,
} esp_mac_type_t;

int esp_read_mac(uint8_t* mac, esp_mac_type_t type);

#endif
