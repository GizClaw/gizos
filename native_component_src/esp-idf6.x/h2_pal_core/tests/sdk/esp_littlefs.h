#ifndef H2_TEST_ESP_LITTLEFS_H
#define H2_TEST_ESP_LITTLEFS_H
#include "esp_err.h"
#include <stdbool.h>
typedef struct esp_vfs_littlefs_conf {
    const char *base_path;
    const char *partition_label;
    bool format_if_mount_failed;
    bool dont_mount;
} esp_vfs_littlefs_conf_t;
bool esp_littlefs_mounted(const char *label);
esp_err_t esp_vfs_littlefs_register(const esp_vfs_littlefs_conf_t *config);
esp_err_t esp_vfs_littlefs_unregister(const char *label);
esp_err_t esp_littlefs_format(const char *label);
#endif
