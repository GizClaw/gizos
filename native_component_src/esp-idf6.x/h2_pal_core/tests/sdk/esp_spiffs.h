#ifndef H2_TEST_ESP_SPIFFS_H
#define H2_TEST_ESP_SPIFFS_H
#include "esp_attr.h"
#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
typedef struct {
  const char* base_path;
  const char* partition_label;
  size_t max_files;
  bool format_if_mount_failed;
} esp_vfs_spiffs_conf_t;
esp_err_t esp_vfs_spiffs_register(const esp_vfs_spiffs_conf_t* config);
esp_err_t esp_vfs_spiffs_unregister(const char* label);
#endif
