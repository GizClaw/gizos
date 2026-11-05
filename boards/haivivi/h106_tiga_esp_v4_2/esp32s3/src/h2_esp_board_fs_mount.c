#include "h2_esp_board_private.h"

#include "h2_esp_board_config.h"
#include "h2_esp_platform_core.h"

#include <string.h>

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

int h2_esp_board_fs_unmount_all(void) {
  return h2_esp_board_h2loader_fs_deinit();
}
