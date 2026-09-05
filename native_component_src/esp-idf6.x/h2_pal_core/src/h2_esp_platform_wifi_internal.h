#ifndef H2_ESP_PLATFORM_WIFI_INTERNAL_H
#define H2_ESP_PLATFORM_WIFI_INTERNAL_H

#include "esp_wifi.h"
#include "h2_esp_wifi_saved_record.h"

int h2_esp_platform_wifi_set_config_safe(
    wifi_interface_t interface,
    const wifi_config_t *config);

#endif
