#pragma once
#include "esp_err.h"
#include <stdint.h>
#define ESP_MAC_BT 2
static inline esp_err_t esp_read_mac(uint8_t *mac, int type) {
  (void)mac;
  (void)type;
  return ESP_OK;
}
