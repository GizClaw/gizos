#ifndef H2_TEST_EXTFLASH_LITTLEFS_H
#define H2_TEST_EXTFLASH_LITTLEFS_H
#include "esp_partition.h"
#include <stdbool.h>
typedef struct {
  const char* base_path;
  const esp_partition_t* partition;
  bool format_if_mount_failed;
  bool dont_mount;
} esp_vfs_littlefs_conf_t;
esp_err_t esp_vfs_littlefs_register(const esp_vfs_littlefs_conf_t*);
esp_err_t esp_littlefs_format_partition(const esp_partition_t*);
#endif
