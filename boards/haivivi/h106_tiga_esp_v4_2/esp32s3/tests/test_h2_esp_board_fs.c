#include "h2_esp_board_private.h"
#include "h2_esp_platform_core.h"
#include "esp_partition.h"
#include <assert.h>

enum { LITTLEFS, SPIFFS, EXTFLASH };
static int s_backend;
static int s_init_error;
static int s_cleanup_error[3];
static unsigned s_cleanup_calls[3];
int h2_esp_platform_data_partition_subtype(const char* label,
                                           uint8_t* subtype) {
  assert(label != NULL);
  *subtype = s_backend == SPIFFS ? ESP_PARTITION_SUBTYPE_DATA_SPIFFS : 0;
  return s_backend == EXTFLASH ? H2_PAL_ERR_NOT_FOUND : H2_PAL_OK;
}
int h2_esp_platform_littlefs_fs_init(
    h2_pal_fs_api_t* fs, const h2_esp_platform_littlefs_config_t* config) {
  assert(fs != NULL && config != NULL);
  return s_init_error;
}
int h2_esp_platform_spiffs_fs_init(
    h2_pal_fs_api_t* fs, const h2_esp_platform_spiffs_config_t* config) {
  assert(fs != NULL && config != NULL);
  return s_init_error;
}
int h2_esp_platform_littlefs_fs_deinit(const char* label) {
  assert(label != NULL);
  ++s_cleanup_calls[LITTLEFS];
  return s_cleanup_error[LITTLEFS];
}
int h2_esp_platform_spiffs_fs_deinit(const char* label) {
  assert(label != NULL);
  ++s_cleanup_calls[SPIFFS];
  return s_cleanup_error[SPIFFS];
}
int h2_esp_board_extflash_mount_data(void) { return s_init_error; }
int h2_esp_board_extflash_unmount_data(void) {
  ++s_cleanup_calls[EXTFLASH];
  return s_cleanup_error[EXTFLASH];
}
int h2_esp_board_extflash_clear_data(void) { return H2_PAL_OK; }
int h2_esp_platform_littlefs_fs_use_base_path(h2_pal_fs_api_t* fs,
                                              const char* path) {
  assert(fs != NULL && path != NULL);
  return H2_PAL_OK;
}
int main(void) {
  h2_pal_fs_api_t fs;
  assert(h2_esp_board_fs_deinit() == H2_PAL_OK);
  for (s_backend = LITTLEFS; s_backend <= EXTFLASH; ++s_backend) {
    assert(h2_esp_board_fs_init(&fs) == H2_PAL_OK);
    unsigned before = s_cleanup_calls[s_backend];
    s_cleanup_error[s_backend] = H2_PAL_ERR_TIMEOUT;
    assert(h2_esp_board_fs_deinit() == H2_PAL_ERR_TIMEOUT);
    assert(s_cleanup_calls[s_backend] == before + 1);
    /* Inactive providers return OK; neither can hide the retained owner error.
     */
    assert(h2_esp_board_fs_deinit() == H2_PAL_ERR_TIMEOUT);
    s_cleanup_error[s_backend] = H2_PAL_OK;
    assert(h2_esp_board_fs_deinit() == H2_PAL_OK);
    assert(s_cleanup_calls[s_backend] == before + 3);
  }
  s_cleanup_error[LITTLEFS] = H2_PAL_ERR_TIMEOUT;
  s_cleanup_error[SPIFFS] = H2_PAL_ERR_IO;
  s_cleanup_error[EXTFLASH] = H2_PAL_ERR_NO_MEMORY;
  assert(h2_esp_board_fs_deinit() == H2_PAL_ERR_TIMEOUT);
  s_cleanup_error[LITTLEFS] = H2_PAL_OK;
  assert(h2_esp_board_fs_deinit() == H2_PAL_ERR_IO);
  s_cleanup_error[SPIFFS] = H2_PAL_OK;
  assert(h2_esp_board_fs_deinit() == H2_PAL_ERR_NO_MEMORY);
  s_cleanup_error[EXTFLASH] = H2_PAL_OK;
  assert(h2_esp_board_fs_deinit() == H2_PAL_OK);
  assert(h2_esp_board_fs_mount_all() == H2_PAL_OK);
  s_cleanup_error[EXTFLASH] = H2_PAL_ERR_TIMEOUT;
  assert(h2_esp_board_fs_unmount_all() == H2_PAL_ERR_TIMEOUT);
  s_cleanup_error[EXTFLASH] = H2_PAL_OK;
  assert(h2_esp_board_fs_unmount_all() == H2_PAL_OK);
  return 0;
}
