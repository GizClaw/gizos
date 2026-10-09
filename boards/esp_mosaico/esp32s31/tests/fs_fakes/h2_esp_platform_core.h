#ifndef H2_TEST_ESP_PLATFORM_CORE_H
#define H2_TEST_ESP_PLATFORM_CORE_H
#include <stdbool.h>
#include "h2/pal/os/h2_pal_fs.h"
typedef struct {
    const char *base_path;
    const char *partition_label;
    bool format_if_mount_failed;
} h2_esp_platform_littlefs_config_t;
int h2_esp_platform_littlefs_mount(const h2_esp_platform_littlefs_config_t *config);
int h2_esp_platform_littlefs_fs_deinit(const char *label);
int h2_esp_platform_littlefs_fs_use_base_path(h2_pal_fs_api_t *fs, const char *path);
int h2_esp_platform_littlefs_format(const char *label);
#endif
