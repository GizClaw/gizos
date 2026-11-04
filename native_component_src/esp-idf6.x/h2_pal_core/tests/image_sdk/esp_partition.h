#ifndef IMAGE_TEST_ESP_PARTITION_H
#define IMAGE_TEST_ESP_PARTITION_H
#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>
typedef unsigned esp_partition_subtype_t;
#define ESP_PARTITION_TYPE_APP 0u
#define ESP_PARTITION_SUBTYPE_APP_OTA_0 16u
#define ESP_PARTITION_SUBTYPE_APP_OTA_1 17u
typedef struct esp_partition {
  uint32_t type, subtype, address, size;
  char label[17];
} esp_partition_t;
typedef struct esp_partition_pos {
  uint32_t offset, size;
} esp_partition_pos_t;
const esp_partition_t* esp_partition_find_first(unsigned type, unsigned subtype,
                                                const char* label);
esp_err_t esp_partition_read(const esp_partition_t* partition, size_t offset,
                             void* data, size_t size);
#endif
