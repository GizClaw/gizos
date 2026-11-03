#ifndef H2_TEST_DISK_ESP_PARTITION_H
#define H2_TEST_DISK_ESP_PARTITION_H

#include "esp_attr.h"
#include "esp_err.h"

#include <stddef.h>
#include <stdint.h>

#ifndef ESP_ERR_INVALID_ARG
#define ESP_ERR_INVALID_ARG 0x102
#endif
#define ESP_PARTITION_TYPE_APP 0u
#define ESP_PARTITION_TYPE_DATA 1u
#define ESP_PARTITION_SUBTYPE_APP_OTA_0 0x10u
#define ESP_PARTITION_SUBTYPE_APP_OTA_1 0x11u
#define ESP_PARTITION_SUBTYPE_DATA_COREDUMP 0x03u

typedef uint8_t esp_partition_type_t;
typedef uint8_t esp_partition_subtype_t;
typedef struct esp_partition {
  esp_partition_type_t type;
  esp_partition_subtype_t subtype;
  uint32_t size;
  char label[17];
} esp_partition_t;

const esp_partition_t* esp_partition_find_first(
    esp_partition_type_t type, esp_partition_subtype_t subtype,
    const char* label);
esp_err_t esp_partition_read(const esp_partition_t* partition, size_t offset,
                             void* data, size_t len);
esp_err_t esp_partition_write(const esp_partition_t* partition, size_t offset,
                              const void* data, size_t len);
esp_err_t esp_partition_erase_range(const esp_partition_t* partition,
                                    size_t offset, size_t len);

#endif
