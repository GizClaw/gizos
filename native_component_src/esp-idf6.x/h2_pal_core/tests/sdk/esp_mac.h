#ifndef H2_TEST_ESP_MAC_H
#define H2_TEST_ESP_MAC_H

#include "esp_err.h"
#include <stdint.h>

typedef enum {
  ESP_MAC_WIFI_STA,
  ESP_MAC_WIFI_SOFTAP,
  ESP_MAC_BT
} esp_mac_type_t;
esp_err_t esp_read_mac(uint8_t* mac, esp_mac_type_t type);
esp_err_t esp_efuse_mac_get_default(uint8_t* mac);

#endif
