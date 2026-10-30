#ifndef H2_ESP_H2LOADER_DEVICE_UID_H
#define H2_ESP_H2LOADER_DEVICE_UID_H

#include "esp_mac.h"
#include "sdkconfig.h"

static inline esp_err_t h2_esp_h2loader_read_device_mac(uint8_t mac[6]) {
#if defined(CONFIG_IDF_TARGET_ESP32P4) && CONFIG_IDF_TARGET_ESP32P4
  /* P4 has no local Bluetooth MAC. Keep identity independent of the C6. */
  return esp_efuse_mac_get_default(mac);
#else
  return esp_read_mac(mac, ESP_MAC_BT);
#endif
}

#endif
