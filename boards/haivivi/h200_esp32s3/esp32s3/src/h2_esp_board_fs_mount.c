#include "h2_esp_board_private.h"

#include "h2_esp_platform_core.h"
#include "h2_esp_board_config.h"
#include "esp_partition.h"

#include <string.h>

int h2_esp_board_fs_init(h2_pal_fs_api_t* fs) {
  if (fs == NULL) return H2_PAL_FS_ERR_INVALID_ARG;
  uint8_t partition_subtype;
  const h2_esp_platform_spiffs_config_t config = {
      .base_path = H2_ESP_BOARD_FS_BASE_PATH,
      .partition_label = H2_ESP_BOARD_FS_PARTITION_LABEL,
      .max_files = 16u,
      .format_if_mount_failed = true,
  };
  h2_pal_result_t rc = h2_esp_platform_data_partition_subtype(
      H2_ESP_BOARD_FS_PARTITION_LABEL, &partition_subtype);
  if (rc == H2_PAL_OK) {
    return h2_esp_platform_spiffs_fs_init(fs, &config);
  }
  if (rc != H2_PAL_ERR_NOT_FOUND) {
    return rc;
  }
  return h2_esp_board_h2loader_fs_init(fs);
}

int h2_esp_board_fs_deinit(void) {
  int legacy_rc =
      h2_esp_platform_spiffs_fs_deinit(H2_ESP_BOARD_FS_PARTITION_LABEL);
  int h2loader_rc = h2_esp_board_h2loader_fs_deinit();
  return legacy_rc != H2_PAL_FS_OK ? legacy_rc : h2loader_rc;
}

static h2_pal_fs_api_t s_mount_fs;
static int s_mount_fs_ready;

static int is_storage_path(const char* path) {
  return path != NULL && ((strncmp(path, "/dl", 3u) == 0 &&
                           (path[3] == '\0' || path[3] == '/')) ||
                          (strncmp(path, "/data", 5u) == 0 &&
                           (path[5] == '\0' || path[5] == '/')));
}

static int mount_partition(const char* path) {
  if (!is_storage_path(path) || strchr(path + 1, '/') != NULL) {
    return H2_PAL_FS_ERR_INVALID_ARG;
  }
  const h2_esp_platform_littlefs_config_t config = {
      .base_path = path,
      .partition_label = path + 1,
      .format_if_mount_failed = true,
  };
  int rc = h2_esp_platform_littlefs_mount(&config);
  return rc;
}

int h2_esp_board_h2loader_fs_init(h2_pal_fs_api_t* fs) {
  if (fs == NULL) {
    return H2_PAL_FS_ERR_INVALID_ARG;
  }
  int rc = mount_partition("/dl");
  if (rc != H2_PAL_FS_OK) {
    return rc;
  }
  rc = mount_partition("/data");
  if (rc != H2_PAL_FS_OK) {
    (void)h2_esp_board_h2loader_fs_deinit();
    return rc;
  }
  return h2_esp_platform_littlefs_fs_use_base_path(fs, "");
}

int h2_esp_board_h2loader_fs_deinit(void) {
  int dl_rc = h2_esp_platform_littlefs_fs_deinit("dl");
  int data_rc = h2_esp_platform_littlefs_fs_deinit("data");
  return dl_rc != H2_PAL_FS_OK ? dl_rc : data_rc;
}

static int ensure_mount_fs(void) {
  if (s_mount_fs_ready) {
    return H2_PAL_FS_OK;
  }
  int rc = h2_esp_board_fs_init(&s_mount_fs);
  if (rc == H2_PAL_FS_OK) {
    s_mount_fs_ready = 1;
  }
  return rc;
}

static int make_dir_all(h2_pal_fs_api_t* fs, const char* path) {
  char current[384];
  size_t len = strlen(path);
  if (len == 0u || len >= sizeof(current)) {
    return H2_PAL_FS_ERR_INVALID_ARG;
  }
  memcpy(current, path, len + 1u);
  for (size_t i = 1u; i < len; ++i) {
    if (current[i] != '/') {
      continue;
    }
    current[i] = '\0';
    int rc = h2_pal_fs_mkdir(fs, current);
    if (rc != H2_PAL_FS_OK && rc != H2_PAL_FS_ERR_UNSUPPORTED) {
      return rc;
    }
    current[i] = '/';
  }
  return h2_pal_fs_mkdir(fs, current);
}

int h2_esp_board_fs_mount(const char* path) {
  if (path == NULL ||
      (strcmp(path, "/dl") != 0 && strcmp(path, "/data") != 0)) {
    return path == NULL ? H2_PAL_FS_ERR_INVALID_ARG : H2_PAL_FS_ERR_UNSUPPORTED;
  }
  int rc = mount_partition(path);
  if (rc == H2_PAL_FS_OK) {
    return rc;
  }
  rc = ensure_mount_fs();
  return rc == H2_PAL_FS_OK ? make_dir_all(&s_mount_fs, path) : rc;
}

int h2_esp_board_fs_unmount(const char* path) {
  if (path == NULL) {
    return H2_PAL_FS_ERR_INVALID_ARG;
  }
  if (strcmp(path, "/dl") != 0 && strcmp(path, "/data") != 0) {
    return H2_PAL_FS_ERR_UNSUPPORTED;
  }
  return h2_esp_platform_littlefs_fs_deinit(path + 1);
}

int h2_esp_board_fs_clear(const char* path) {
  if (path == NULL) {
    return H2_PAL_FS_ERR_INVALID_ARG;
  }
  if (strcmp(path, "/data") != 0) {
    return H2_PAL_FS_ERR_UNSUPPORTED;
  }
  int rc = h2_esp_board_fs_unmount(path);
  if (rc != H2_PAL_FS_OK && rc != H2_PAL_FS_ERR_NOT_FOUND) {
    return rc;
  }
  rc = h2_esp_platform_littlefs_format(path + 1);
  return rc == H2_PAL_FS_OK ? mount_partition(path) : rc;
}

int h2_esp_board_fs_mount_all(void) {
  int rc = h2_esp_board_fs_mount("/dl");
  return rc == H2_PAL_FS_OK ? h2_esp_board_fs_mount("/data") : rc;
}

int h2_esp_board_fs_unmount_all(void) {
  int rc = h2_esp_board_fs_deinit();
  if (rc == H2_PAL_FS_OK) s_mount_fs_ready = 0;
  return rc;
}
