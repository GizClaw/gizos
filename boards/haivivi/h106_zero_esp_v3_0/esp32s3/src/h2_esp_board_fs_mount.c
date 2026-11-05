#include "h2_esp_board_private.h"

#include "h2_esp_board_config.h"
#include "h2_esp_platform_core.h"
#include "h2_esp_board_config.h"
#include "esp_partition.h"

#include <string.h>

int h2_esp_board_fs_init(h2_pal_fs_api_t* fs) {
  if (fs == NULL) return H2_PAL_FS_ERR_INVALID_ARG;
  uint8_t partition_subtype;
  const h2_esp_platform_littlefs_config_t littlefs_config = {
      .base_path = H2_ESP_BOARD_FS_BASE_PATH,
      .partition_label = H2_ESP_BOARD_FS_PARTITION_LABEL,
      .format_if_mount_failed = true,
  };
  const h2_esp_platform_spiffs_config_t spiffs_config = {
      .base_path = H2_ESP_BOARD_FS_BASE_PATH,
      .partition_label = H2_ESP_BOARD_FS_PARTITION_LABEL,
      .max_files = 16u,
      .format_if_mount_failed = true,
  };

  h2_pal_result_t partition_rc = h2_esp_platform_data_partition_subtype(
      H2_ESP_BOARD_FS_PARTITION_LABEL, &partition_subtype);
  if (partition_rc == H2_PAL_OK &&
      partition_subtype == ESP_PARTITION_SUBTYPE_DATA_SPIFFS) {
    return h2_esp_platform_spiffs_fs_init(fs, &spiffs_config);
  }
  if (partition_rc == H2_PAL_OK) {
    int rc = h2_esp_platform_littlefs_fs_init(fs, &littlefs_config);
    if (rc == H2_PAL_FS_OK) {
      return rc;
    }
    int cleanup_rc =
        h2_esp_platform_littlefs_fs_deinit(H2_ESP_BOARD_FS_PARTITION_LABEL);
    if (cleanup_rc != H2_PAL_FS_OK) return cleanup_rc;
  } else if (partition_rc != H2_PAL_ERR_NOT_FOUND) {
    return partition_rc;
  }
  return h2_esp_board_h2loader_fs_init(fs);
}

int h2_esp_board_fs_deinit(void) {
  /* Providers return success when inactive, and retain failed resources for
   * retry. */
  int littlefs_rc =
      h2_esp_platform_littlefs_fs_deinit(H2_ESP_BOARD_FS_PARTITION_LABEL);
  int spiffs_rc =
      h2_esp_platform_spiffs_fs_deinit(H2_ESP_BOARD_FS_PARTITION_LABEL);
  int h2loader_rc = h2_esp_board_h2loader_fs_deinit();
  if (littlefs_rc != H2_PAL_FS_OK) return littlefs_rc;
  return spiffs_rc != H2_PAL_FS_OK ? spiffs_rc : h2loader_rc;
}

int h2_esp_board_h2loader_fs_init(h2_pal_fs_api_t* fs) {
  if (fs == NULL) {
    return H2_PAL_FS_ERR_INVALID_ARG;
  }
  int rc = h2_esp_board_extflash_mount_data();
  if (rc != H2_PAL_FS_OK) {
    return rc;
  }
  return h2_esp_platform_littlefs_fs_use_base_path(fs,
                                                   H2_ESP_BOARD_FS_BASE_PATH);
}

int h2_esp_board_h2loader_fs_deinit(void) {
  return h2_esp_board_extflash_unmount_data();
}

int h2_esp_board_fs_mount(const char* path) {
  if (path == NULL) {
    return H2_PAL_FS_ERR_INVALID_ARG;
  }
  if (strcmp(path, "/dl") == 0 || strcmp(path, "/data") == 0) {
    return h2_esp_board_extflash_mount_data();
  }
  return H2_PAL_FS_ERR_UNSUPPORTED;
}

int h2_esp_board_fs_unmount(const char* path) {
  if (path == NULL) {
    return H2_PAL_FS_ERR_INVALID_ARG;
  }
  if (strcmp(path, "/dl") == 0 || strcmp(path, "/data") == 0) {
    return h2_esp_board_extflash_unmount_data();
  }
  return H2_PAL_FS_ERR_UNSUPPORTED;
}

int h2_esp_board_fs_clear(const char* path) {
  if (path == NULL) {
    return H2_PAL_FS_ERR_INVALID_ARG;
  }
  return strcmp(path, "/data") == 0 ? h2_esp_board_extflash_clear_data()
                                    : H2_PAL_FS_ERR_UNSUPPORTED;
}

int h2_esp_board_fs_mount_all(void) {
  return h2_esp_board_extflash_mount_data();
}

int h2_esp_board_fs_unmount_all(void) { return h2_esp_board_fs_deinit(); }
