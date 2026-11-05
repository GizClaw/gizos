#include "h2_esp_board_private.h"
#include "h2_esp_platform_core.h"
#include <assert.h>
#include <string.h>
static int s_legacy, s_spiffs_error, s_dl_error, s_data_error;
static unsigned s_spiffs_calls, s_dl_calls, s_data_calls;
int h2_esp_platform_data_partition_subtype(const char* label,
                                           uint8_t* subtype) {
  assert(label != NULL);
  *subtype = 0;
  return s_legacy ? H2_PAL_OK : H2_PAL_ERR_NOT_FOUND;
}
int h2_esp_platform_spiffs_fs_init(h2_pal_fs_api_t* fs,
                                   const h2_esp_platform_spiffs_config_t* cfg) {
  assert(fs != NULL && cfg != NULL);
  return H2_PAL_OK;
}
int h2_esp_platform_spiffs_fs_deinit(const char* label) {
  assert(label != NULL);
  ++s_spiffs_calls;
  return s_spiffs_error;
}
int h2_esp_platform_littlefs_mount(
    const h2_esp_platform_littlefs_config_t* cfg) {
  assert(cfg != NULL);
  return H2_PAL_OK;
}
int h2_esp_platform_littlefs_fs_deinit(const char* label) {
  if (strcmp(label, "dl") == 0) {
    ++s_dl_calls;
    return s_dl_error;
  }
  assert(strcmp(label, "data") == 0);
  ++s_data_calls;
  return s_data_error;
}
int h2_esp_platform_littlefs_fs_use_base_path(h2_pal_fs_api_t* fs,
                                              const char* path) {
  assert(fs != NULL && path != NULL);
  return H2_PAL_OK;
}
int h2_esp_platform_littlefs_format(const char* label) {
  (void)label;
  return H2_PAL_OK;
}
int main(void) {
  h2_pal_fs_api_t fs;
  assert(h2_esp_board_fs_init(&fs) == H2_PAL_OK);
  s_dl_error = H2_PAL_ERR_TIMEOUT;
  assert(h2_esp_board_fs_deinit() == H2_PAL_ERR_TIMEOUT);
  assert(s_dl_calls == 1u && s_data_calls == 1u && s_spiffs_calls == 1u);
  s_dl_error = H2_PAL_OK;
  s_data_error = H2_PAL_ERR_IO;
  assert(h2_esp_board_fs_deinit() == H2_PAL_ERR_IO);
  s_data_error = H2_PAL_OK;
  assert(h2_esp_board_fs_deinit() == H2_PAL_OK);
  s_legacy = 1;
  assert(h2_esp_board_fs_init(&fs) == H2_PAL_OK);
  s_spiffs_error = H2_PAL_ERR_TIMEOUT;
  assert(h2_esp_board_fs_deinit() == H2_PAL_ERR_TIMEOUT);
  s_spiffs_error = H2_PAL_OK;
  assert(h2_esp_board_fs_deinit() == H2_PAL_OK);
  assert(h2_esp_board_fs_mount_all() == H2_PAL_OK);
  s_data_error = H2_PAL_ERR_TIMEOUT;
  assert(h2_esp_board_fs_unmount_all() == H2_PAL_ERR_TIMEOUT);
  s_data_error = H2_PAL_OK;
  assert(h2_esp_board_fs_unmount_all() == H2_PAL_OK);
  return 0;
}
