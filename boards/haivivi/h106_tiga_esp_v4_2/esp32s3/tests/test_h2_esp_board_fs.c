#include "h2_esp_board_private.h"
#include "h2_esp_platform_core.h"
#include "esp_partition.h"
#include <assert.h>
#include <stdbool.h>
#include <string.h>

enum { LITTLEFS, SPIFFS, EXTFLASH };
static int s_backend;
static int s_init_error;
static int s_cleanup_error[3];
static unsigned s_cleanup_calls[3];
static unsigned s_extflash_mount_calls;
static unsigned s_extflash_clear_calls;
static int s_extflash_mount_result;
static int s_littlefs_init_result;
static int s_use_base_path_result;
static unsigned s_provider_clear_calls;
static int s_provider_clear_result;
static int s_extflash_clear_result;
static unsigned s_mkdir_calls;
static int s_mkdir_result;
static int s_stat_result;
static bool s_path_exists[2];
static bool s_path_is_dir[2];

static unsigned path_index(const char* path) {
  if (strcmp(path, "/dl") == 0) return 0u;
  assert(strcmp(path, "/data") == 0);
  return 1u;
}

static int provider_mkdir(void* user, const char* path) {
  assert(user == &s_backend);
  ++s_mkdir_calls;
  if (s_mkdir_result != H2_PAL_OK) return s_mkdir_result;
  const unsigned index = path_index(path);
  if (!s_path_exists[index]) {
    s_path_exists[index] = true;
    s_path_is_dir[index] = true;
  }
  return H2_PAL_OK;
}

static int provider_stat(void* user, const char* path, h2_pal_fs_stat_t* st) {
  assert(user == &s_backend);
  if (s_stat_result != H2_PAL_OK) return s_stat_result;
  const unsigned index = path_index(path);
  if (!s_path_exists[index]) return H2_PAL_ERR_NOT_FOUND;
  *st = (h2_pal_fs_stat_t){.is_dir = s_path_is_dir[index]};
  return H2_PAL_OK;
}

static int provider_clear(void* user, const char* path) {
  assert(user == &s_backend && strcmp(path, "/data") == 0);
  ++s_provider_clear_calls;
  return s_provider_clear_result;
}

static const h2_pal_fs_vtable_t s_provider_vtable = {
    .mkdir = provider_mkdir,
    .stat = provider_stat,
    .clear = provider_clear,
};
static void set_provider_api(h2_pal_fs_api_t* fs) {
  *fs = (h2_pal_fs_api_t){.user = &s_backend, .vtable = &s_provider_vtable};
}

int h2_esp_platform_data_partition_subtype(const char* label,
                                           uint8_t* subtype) {
  assert(label != NULL);
  *subtype = s_backend == SPIFFS ? ESP_PARTITION_SUBTYPE_DATA_SPIFFS : 0;
  return s_backend == EXTFLASH ? H2_PAL_ERR_NOT_FOUND : H2_PAL_OK;
}
int h2_esp_platform_littlefs_fs_init(
    h2_pal_fs_api_t* fs, const h2_esp_platform_littlefs_config_t* config) {
  assert(fs != NULL && config != NULL);
  set_provider_api(fs);
  return s_init_error != H2_PAL_OK ? s_init_error : s_littlefs_init_result;
}
int h2_esp_platform_spiffs_fs_init(
    h2_pal_fs_api_t* fs, const h2_esp_platform_spiffs_config_t* config) {
  assert(fs != NULL && config != NULL);
  set_provider_api(fs);
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
int h2_esp_board_extflash_mount_data(void) {
  ++s_extflash_mount_calls;
  return s_init_error != H2_PAL_OK ? s_init_error : s_extflash_mount_result;
}
int h2_esp_board_extflash_unmount_data(void) {
  ++s_cleanup_calls[EXTFLASH];
  return s_cleanup_error[EXTFLASH];
}
int h2_esp_board_extflash_clear_data(void) {
  ++s_extflash_clear_calls;
  return s_extflash_clear_result;
}
int h2_esp_platform_littlefs_fs_use_base_path(h2_pal_fs_api_t* fs,
                                              const char* path) {
  assert(fs != NULL && path != NULL);
  set_provider_api(fs);
  return s_use_base_path_result;
}

static void test_prepare_uses_selected_backend(void) {
  h2_pal_fs_api_t fs;
  assert(h2_esp_board_fs_prepare(NULL) == H2_PAL_ERR_INVALID_ARG);
  assert(h2_esp_board_fs_prepare("") == H2_PAL_ERR_INVALID_ARG);
  assert(h2_esp_board_fs_prepare("/other") == H2_PAL_ERR_UNSUPPORTED);
  assert(h2_esp_board_fs_prepare("/dl") == H2_PAL_ERR_INVALID_STATE);
  for (s_backend = LITTLEFS; s_backend <= EXTFLASH; ++s_backend) {
    assert(h2_esp_board_fs_init(&fs) == H2_PAL_OK);
    const unsigned mounts = s_extflash_mount_calls;
    s_extflash_mount_result = H2_PAL_ERR_IO;
    const int expected = s_backend == EXTFLASH ? H2_PAL_ERR_IO : H2_PAL_OK;
    assert(h2_esp_board_fs_prepare("/dl") == expected);
    assert(h2_esp_board_fs_prepare("/data") == expected);
    assert(s_extflash_mount_calls ==
           mounts + (s_backend == EXTFLASH ? 2u : 0u));
    assert(s_extflash_clear_calls == 0u);
    s_extflash_mount_result = H2_PAL_OK;
    assert(h2_esp_board_fs_prepare("/data") == H2_PAL_OK);
    assert(h2_esp_board_fs_deinit() == H2_PAL_OK);
    assert(h2_esp_board_fs_prepare("/dl") == H2_PAL_ERR_INVALID_STATE);
  }

  /* Failed internal initialization must not select external storage. */
  s_backend = SPIFFS;
  s_init_error = H2_PAL_ERR_IO;
  unsigned mounts = s_extflash_mount_calls;
  assert(h2_esp_board_fs_init(&fs) == H2_PAL_ERR_IO);
  assert(h2_esp_board_fs_prepare("/dl") == H2_PAL_ERR_INVALID_STATE);
  assert(s_extflash_mount_calls == mounts);
  s_init_error = H2_PAL_OK;
  assert(h2_esp_board_fs_deinit() == H2_PAL_OK);

  /* The existing LittleFS fallback selects external storage only after the
   * failed internal provider has been cleaned up and external setup succeeds.
   */
  s_backend = LITTLEFS;
  s_littlefs_init_result = H2_PAL_ERR_IO;
  s_cleanup_error[LITTLEFS] = H2_PAL_ERR_TIMEOUT;
  assert(h2_esp_board_fs_init(&fs) == H2_PAL_ERR_TIMEOUT);
  assert(h2_esp_board_fs_prepare("/dl") == H2_PAL_ERR_INVALID_STATE);
  assert(s_extflash_mount_calls == mounts);
  s_cleanup_error[LITTLEFS] = H2_PAL_OK;
  assert(h2_esp_board_fs_init(&fs) == H2_PAL_OK);
  assert(h2_esp_board_fs_prepare("/dl") == H2_PAL_OK);
  assert(s_extflash_mount_calls == mounts + 2u);
  assert(h2_esp_board_fs_deinit() == H2_PAL_OK);
  s_littlefs_init_result = H2_PAL_OK;

  s_backend = EXTFLASH;
  s_use_base_path_result = H2_PAL_ERR_IO;
  assert(h2_esp_board_fs_init(&fs) == H2_PAL_ERR_IO);
  mounts = s_extflash_mount_calls;
  assert(h2_esp_board_fs_prepare("/dl") == H2_PAL_ERR_INVALID_STATE);
  assert(s_extflash_mount_calls == mounts);
  s_use_base_path_result = H2_PAL_OK;
  assert(h2_esp_board_fs_deinit() == H2_PAL_OK);
  assert(s_extflash_clear_calls == 0u);
}

static void test_prepare_creates_internal_directories(void) {
  s_backend = LITTLEFS;
  memset(s_path_exists, 0, sizeof(s_path_exists));
  memset(s_path_is_dir, 0, sizeof(s_path_is_dir));
  h2_pal_fs_api_t fs;
  assert(h2_esp_board_fs_init(&fs) == H2_PAL_OK);
  const unsigned mounts = s_extflash_mount_calls;
  const unsigned mkdirs = s_mkdir_calls;
  for (unsigned round = 0u; round < 2u; ++round) {
    assert(h2_esp_board_fs_prepare("/dl") == H2_PAL_OK);
    assert(h2_esp_board_fs_prepare("/data") == H2_PAL_OK);
    assert(s_path_exists[0] && s_path_is_dir[0]);
    assert(s_path_exists[1] && s_path_is_dir[1]);
  }
  assert(s_mkdir_calls == mkdirs + 4u);
  s_mkdir_result = H2_PAL_ERR_NO_SPACE;
  assert(h2_esp_board_fs_prepare("/data") == H2_PAL_ERR_NO_SPACE);
  s_mkdir_result = H2_PAL_OK;
  s_stat_result = H2_PAL_ERR_IO;
  assert(h2_esp_board_fs_prepare("/dl") == H2_PAL_ERR_IO);
  s_stat_result = H2_PAL_OK;
  s_path_is_dir[0] = false;
  assert(h2_esp_board_fs_prepare("/dl") == H2_PAL_ERR_INVALID_STATE);
  assert(s_extflash_mount_calls == mounts);
  assert(h2_esp_board_fs_deinit() == H2_PAL_OK);
  s_path_is_dir[0] = true;
}

static void test_clear_uses_selected_backend(void) {
  assert(h2_esp_board_fs_clear("/data") == H2_PAL_ERR_INVALID_STATE);
  assert(h2_esp_board_fs_clear(NULL) == H2_PAL_ERR_INVALID_ARG);
  assert(h2_esp_board_fs_clear("/dl") == H2_PAL_ERR_UNSUPPORTED);
  for (s_backend = LITTLEFS; s_backend <= EXTFLASH; ++s_backend) {
    h2_pal_fs_api_t fs;
    assert(h2_esp_board_fs_init(&fs) == H2_PAL_OK);
    /* The board owns a copy of the raw API, independent of Runtime wrapping
     * or changes to the caller's API object. */
    fs.vtable = NULL;
    const unsigned external = s_extflash_clear_calls;
    const unsigned internal = s_provider_clear_calls;
    s_provider_clear_result = H2_PAL_ERR_IO;
    s_extflash_clear_result = H2_PAL_ERR_TIMEOUT;
    const int expected =
        s_backend == EXTFLASH ? H2_PAL_ERR_TIMEOUT : H2_PAL_ERR_IO;
    assert(h2_esp_board_fs_clear("/data") == expected);
    assert(s_extflash_clear_calls ==
           external + (s_backend == EXTFLASH ? 1u : 0u));
    assert(s_provider_clear_calls ==
           internal + (s_backend == EXTFLASH ? 0u : 1u));
    const unsigned mounts = s_extflash_mount_calls;
    assert(h2_esp_board_fs_clear("/data-other") == H2_PAL_ERR_UNSUPPORTED);
    assert(h2_esp_board_fs_clear("/data/../dl") == H2_PAL_ERR_UNSUPPORTED);
    assert(s_extflash_mount_calls == mounts);
    s_provider_clear_result = H2_PAL_OK;
    s_extflash_clear_result = H2_PAL_OK;
    assert(h2_esp_board_fs_clear("/data") == H2_PAL_OK);
    assert(h2_esp_board_fs_deinit() == H2_PAL_OK);
    assert(h2_esp_board_fs_clear("/data") == H2_PAL_ERR_INVALID_STATE);
  }
}

int main(void) {
  h2_pal_fs_api_t fs;
  assert(h2_esp_board_fs_deinit() == H2_PAL_OK);
  test_prepare_uses_selected_backend();
  test_prepare_creates_internal_directories();
  test_clear_uses_selected_backend();
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
