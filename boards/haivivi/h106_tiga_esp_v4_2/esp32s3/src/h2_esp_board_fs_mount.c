#include "h2_esp_board_private.h"

#include "h2_esp_board_config.h"
#include "h2_esp_platform_core.h"
#include "h2_esp_board_config.h"
#include "esp_partition.h"

#include <string.h>

typedef enum {
  H2_ESP_BOARD_FS_NONE,
  H2_ESP_BOARD_FS_INTERNAL_LITTLEFS,
  H2_ESP_BOARD_FS_INTERNAL_SPIFFS,
  H2_ESP_BOARD_FS_EXTFLASH,
} h2_esp_board_fs_backend_t;

static h2_esp_board_fs_backend_t s_fs_backend;
/* Copy the unwrapped provider API before Runtime installs its board adapters.
 */
static h2_pal_fs_api_t s_selected_fs;

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
    int rc = h2_esp_platform_spiffs_fs_init(fs, &spiffs_config);
    if (rc == H2_PAL_FS_OK) {
      s_fs_backend = H2_ESP_BOARD_FS_INTERNAL_SPIFFS;
      s_selected_fs = *fs;
    }
    return rc;
  }
  if (partition_rc == H2_PAL_OK) {
    int rc = h2_esp_platform_littlefs_fs_init(fs, &littlefs_config);
    if (rc == H2_PAL_FS_OK) {
      s_fs_backend = H2_ESP_BOARD_FS_INTERNAL_LITTLEFS;
      s_selected_fs = *fs;
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
  if ((s_fs_backend == H2_ESP_BOARD_FS_INTERNAL_LITTLEFS &&
       littlefs_rc == H2_PAL_FS_OK) ||
      (s_fs_backend == H2_ESP_BOARD_FS_INTERNAL_SPIFFS &&
       spiffs_rc == H2_PAL_FS_OK)) {
    s_fs_backend = H2_ESP_BOARD_FS_NONE;
    memset(&s_selected_fs, 0, sizeof(s_selected_fs));
  }
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
  rc = h2_esp_platform_littlefs_fs_use_base_path(fs, H2_ESP_BOARD_FS_BASE_PATH);
  if (rc == H2_PAL_FS_OK) {
    s_fs_backend = H2_ESP_BOARD_FS_EXTFLASH;
    s_selected_fs = *fs;
  }
  return rc;
}

int h2_esp_board_h2loader_fs_deinit(void) {
  int rc = h2_esp_board_extflash_unmount_data();
  if (rc == H2_PAL_FS_OK && s_fs_backend == H2_ESP_BOARD_FS_EXTFLASH) {
    s_fs_backend = H2_ESP_BOARD_FS_NONE;
    memset(&s_selected_fs, 0, sizeof(s_selected_fs));
  }
  return rc;
}

int h2_esp_board_fs_prepare(const char* path) {
  if (path == NULL || path[0] == '\0') return H2_PAL_FS_ERR_INVALID_ARG;
  if (strcmp(path, "/dl") != 0 && strcmp(path, "/data") != 0)
    return H2_PAL_FS_ERR_UNSUPPORTED;
  if (s_fs_backend == H2_ESP_BOARD_FS_INTERNAL_LITTLEFS) {
    /* A fresh mount has no Loader directories. mkdir is idempotent, but a
     * regular file at the requested root must not count as a ready directory.
     */
    int rc = h2_pal_fs_mkdir(&s_selected_fs, path);
    if (rc != H2_PAL_FS_OK) return rc;
    h2_pal_fs_stat_t st;
    rc = h2_pal_fs_stat(&s_selected_fs, path, &st);
    if (rc != H2_PAL_FS_OK) return rc;
    return st.is_dir ? H2_PAL_FS_OK : H2_PAL_ERR_INVALID_STATE;
  }
  if (s_fs_backend == H2_ESP_BOARD_FS_INTERNAL_SPIFFS) {
    /* Preserve the provider's unsupported prepare contract. This adapter
     * must not enable Loader operations that SPIFFS does not implement. */
    return h2_pal_fs_prepare(&s_selected_fs, path);
  }
  if (s_fs_backend == H2_ESP_BOARD_FS_EXTFLASH)
    return h2_esp_board_extflash_mount_data();
  return H2_PAL_ERR_INVALID_STATE;
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
  if (path == NULL) return H2_PAL_FS_ERR_INVALID_ARG;
  if (strcmp(path, "/data") != 0) return H2_PAL_FS_ERR_UNSUPPORTED;
  if (s_fs_backend == H2_ESP_BOARD_FS_INTERNAL_LITTLEFS ||
      s_fs_backend == H2_ESP_BOARD_FS_INTERNAL_SPIFFS)
    return h2_pal_fs_clear(&s_selected_fs, path);
  if (s_fs_backend == H2_ESP_BOARD_FS_EXTFLASH)
    return h2_esp_board_extflash_clear_data();
  return H2_PAL_ERR_INVALID_STATE;
}

int h2_esp_board_fs_mount_all(void) {
  return h2_esp_board_extflash_mount_data();
}

int h2_esp_board_fs_unmount_all(void) { return h2_esp_board_fs_deinit(); }
