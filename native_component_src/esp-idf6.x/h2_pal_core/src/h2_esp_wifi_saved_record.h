#ifndef H2_ESP_WIFI_SAVED_RECORD_H
#define H2_ESP_WIFI_SAVED_RECORD_H

#include <stdbool.h>
#include <stdint.h>

/* Version 1: version, SSID/password lengths, BSSID flag, channel, BSSID,
 * 32 SSID bytes and 64 password bytes. A zero SSID length is a tombstone. */
#define H2_ESP_WIFI_SAVED_RECORD_SIZE 107u
int h2_esp_platform_wifi_saved_record(
    uint8_t record[H2_ESP_WIFI_SAVED_RECORD_SIZE], bool write);

#endif
