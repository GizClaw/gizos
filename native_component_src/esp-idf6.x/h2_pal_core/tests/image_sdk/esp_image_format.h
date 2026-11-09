#ifndef IMAGE_TEST_ESP_IMAGE_FORMAT_H
#define IMAGE_TEST_ESP_IMAGE_FORMAT_H
#include "esp_partition.h"
typedef struct esp_image_metadata {
  uint32_t image_len;
} esp_image_metadata_t;
esp_err_t esp_image_get_metadata(const esp_partition_pos_t* position,
                                 esp_image_metadata_t* metadata);
#endif
