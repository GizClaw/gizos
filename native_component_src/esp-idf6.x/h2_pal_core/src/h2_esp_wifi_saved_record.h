#ifndef H2_ESP_WIFI_SAVED_RECORD_H
#define H2_ESP_WIFI_SAVED_RECORD_H

#include "h2_wifi_sta.h"
#include <stdbool.h>
#include <stdint.h>

/* Version 1: version, SSID/password lengths, BSSID flag, channel, BSSID,
 * 32 SSID bytes and 64 password bytes. A zero SSID length is a tombstone. */
#define H2_ESP_WIFI_SAVED_RECORD_SIZE 107u
int h2_esp_platform_wifi_saved_record(
    uint8_t record[H2_WIFI_SAVED_LIST_BLOB_SIZE], bool write);

/* All blob I/O must run inside this transaction, including read-modify-write.
 */
int h2_esp_platform_wifi_saved_transaction(int (*operation)(void *),
                                           void *context);
int h2_esp_wifi_saved_import(
    const uint8_t record[H2_ESP_WIFI_SAVED_RECORD_SIZE],
    uint8_t blob[H2_WIFI_SAVED_LIST_BLOB_SIZE]);

#endif
