#ifndef H2_TEST_ESP_MAC_H
#define H2_TEST_ESP_MAC_H
#include <stdint.h>
typedef int esp_err_t;
typedef enum { ESP_MAC_BT } esp_mac_type_t;
esp_err_t esp_efuse_mac_get_default(uint8_t* mac);
esp_err_t esp_read_mac(uint8_t* mac, esp_mac_type_t type);
#endif
