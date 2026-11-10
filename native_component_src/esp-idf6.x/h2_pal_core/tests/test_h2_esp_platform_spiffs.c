#include "h2_esp_platform_core.h"
#include "h2_esp_platform_safe_call.h"
#include "esp_spiffs.h"
#include "freertos/semphr.h"

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The real provider runs against a flat SPIFFS object inventory, as returned
 * by ESP-IDF's root readdir. File contents use real host stdio, including EOF.
 */
static int s_worker, s_locked, s_safe_error, s_seek_error;
static int s_open_error, s_read_error, s_remove_error, s_close_error;
static esp_err_t s_mount_error;
static unsigned s_calls, s_dir_opens, s_dir_closes, s_removes;
static size_t s_cursor;
static const char* s_names[16];
static int s_alive[16];
static size_t s_count;
static struct dirent s_entry;
static FILE* s_file;

static FILE* test_fopen(const char* path, const char* mode) {
  assert(s_worker && s_locked && strcmp(mode, "rb") == 0);
  if (strcmp(path, "/spiffs/data/fixture") != 0) {
    errno = ENOENT;
    return NULL;
  }
  s_file = tmpfile();
  assert(s_file != NULL);
  assert(fwrite("0123456789", 1, 10, s_file) == 10);
  rewind(s_file);
  return s_file;
}
static int test_fseek(FILE* file, long offset, int whence) {
  assert(s_worker && s_locked && file == s_file && whence == SEEK_SET);
  if (s_seek_error) {
    errno = s_seek_error;
    return -1;
  }
  return fseek(file, offset, whence);
}
static DIR* test_opendir(const char* path) {
  assert(s_worker && s_locked && strcmp(path, "/spiffs") == 0);
  ++s_dir_opens;
  if (s_open_error) {
    errno = s_open_error;
    return NULL;
  }
  s_cursor = 0;
  return (DIR*)&s_cursor;
}
static struct dirent* test_readdir(DIR* dir) {
  assert(s_worker && s_locked && dir == (DIR*)&s_cursor);
  if (s_read_error) {
    errno = s_read_error;
    return NULL;
  }
  while (s_cursor < s_count) {
    size_t i = s_cursor++;
    if (!s_alive[i]) continue;
    assert(strlen(s_names[i]) < sizeof(s_entry.d_name));
    strcpy(s_entry.d_name, s_names[i]);
    return &s_entry;
  }
  return NULL;
}
static int test_closedir(DIR* dir) {
  assert(s_worker && s_locked && dir == (DIR*)&s_cursor);
  ++s_dir_closes;
  if (s_close_error) {
    errno = s_close_error;
    return -1;
  }
  return 0;
}
static int test_remove(const char* path) {
  assert(s_worker && s_locked && strncmp(path, "/spiffs/", 8) == 0);
  ++s_removes;
  if (s_remove_error) {
    errno = s_remove_error;
    return -1;
  }
  for (size_t i = 0; i < s_count; ++i) {
    if (s_alive[i] && strcmp(path + 8, s_names[i]) == 0) {
      s_alive[i] = 0;
      return 0;
    }
  }
  assert(!"remove must address an enumerated object exactly");
  return -1;
}

static int s_rename_error;
static unsigned s_rename_calls;
static char s_renamed_name[192];
static int test_rename(const char* old_path, const char* new_path) {
  assert(s_worker && s_locked);
  assert(strncmp(old_path, "/spiffs/", 8u) == 0);
  assert(strncmp(new_path, "/spiffs/", 8u) == 0);
  ++s_rename_calls;
  if (s_rename_error != 0) {
    errno = s_rename_error;
    return -1;
  }
  for (size_t i = 0u; i < s_count; ++i) {
    if (s_alive[i] && strcmp(old_path + 8u, s_names[i]) == 0) {
      assert(strlen(new_path + 8u) < sizeof(s_renamed_name));
      strcpy(s_renamed_name, new_path + 8u);
      s_names[i] = s_renamed_name;
      return 0;
    }
  }
  errno = ENOENT;
  return -1;
}

#define rename(old_path, new_path) test_rename(old_path, new_path)
#define fopen test_fopen
#define fseek test_fseek
#define opendir test_opendir
#define readdir test_readdir
#define closedir test_closedir
#define remove(path) test_remove(path)
#include "src/h2_esp_platform_spiffs.c"
#undef rename
#undef fopen
#undef fseek
#undef opendir
#undef readdir
#undef closedir
#undef remove

SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t* storage) {
  return storage;
}
BaseType_t xSemaphoreTake(SemaphoreHandle_t mutex, uint32_t timeout) {
  assert(mutex && timeout == portMAX_DELAY && !s_locked);
  s_locked = 1;
  return pdTRUE;
}
BaseType_t xSemaphoreGive(SemaphoreHandle_t mutex) {
  assert(mutex && s_locked && !s_worker);
  s_locked = 0;
  return pdTRUE;
}
void* heap_caps_malloc(size_t size, unsigned caps) {
  assert(caps == (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  return malloc(size);
}
void heap_caps_free(void* data) { free(data); }
h2_pal_result_t h2_esp_platform_safe_call(
    h2_esp_platform_safe_call_cb_t callback, void* context, size_t context_size,
    size_t stack_depth) {
  assert(s_locked && !s_worker && context_size <= 1024 && stack_depth == 4096);
  ++s_calls;
  if (s_safe_error) return s_safe_error;
  _Alignas(max_align_t) unsigned char internal[1024];
  memcpy(internal, context, context_size);
  s_worker = 1;
  callback(internal);
  s_worker = 0;
  memcpy(context, internal, context_size);
  return H2_PAL_OK;
}
esp_err_t esp_vfs_spiffs_register(const esp_vfs_spiffs_conf_t* config) {
  assert(s_worker && s_locked);
  assert(strcmp(config->base_path, "/spiffs") == 0);
  assert(strcmp(config->partition_label, "storage") == 0);
  assert(config->max_files == 16 && !config->format_if_mount_failed);
  return s_mount_error;
}
esp_err_t esp_vfs_spiffs_unregister(const char* label) {
  assert(s_worker && s_locked && strcmp(label, "storage") == 0);
  return ESP_OK;
}

static void inventory(const char* const* names, size_t count) {
  assert(count <= 16);
  s_count = count;
  s_open_error = s_read_error = s_remove_error = s_close_error = 0;
  for (size_t i = 0; i < count; ++i) {
    s_names[i] = names[i];
    s_alive[i] = 1;
  }
}
static void test_seek(const h2_pal_fs_api_t* fs) {
  h2_pal_fs_file_t* file = NULL;
  assert(h2_pal_fs_open(fs, "/data/fixture", H2_PAL_FS_OPEN_READ, &file) == 0);
  char data[16];
  size_t count;
  assert(h2_pal_fs_seek(fs, file, 4) == 0);
  assert(h2_pal_fs_read(fs, file, data, 3, &count) == 0);
  assert(count == 3 && memcmp(data, "456", 3) == 0);
  assert(h2_pal_fs_read(fs, file, data, sizeof(data), &count) == 0 &&
         count == 3);
  assert(feof(s_file));
  assert(h2_pal_fs_seek(fs, file, 1) == 0 && !feof(s_file));
  assert(h2_pal_fs_read(fs, file, data, 2, &count) == 0);
  assert(count == 2 && memcmp(data, "12", 2) == 0);
  unsigned calls = s_calls;
  assert(h2_pal_fs_seek(fs, file, UINT64_MAX) == H2_PAL_FS_ERR_INVALID_ARG);
  assert(h2_pal_fs_seek(fs, file, (uint64_t)LONG_MAX + 1u) ==
         H2_PAL_FS_ERR_INVALID_ARG);
  assert(fs->vtable->seek(fs->user, NULL, 0) == H2_PAL_FS_ERR_INVALID_ARG);
  assert(s_calls == calls);
  s_seek_error = EIO;
  assert(h2_pal_fs_seek(fs, file, 2) == H2_PAL_FS_ERR_IO);
  s_seek_error = EINVAL;
  assert(h2_pal_fs_seek(fs, file, 2) == H2_PAL_FS_ERR_INVALID_ARG);
  s_seek_error = 0;
  s_safe_error = H2_PAL_ERR_TIMEOUT;
  assert(h2_pal_fs_seek(fs, file, 0) == H2_PAL_ERR_TIMEOUT);
  s_safe_error = 0;
  assert(h2_pal_fs_seek(fs, file, 0) == 0);
  assert(h2_pal_fs_close(fs, file) == 0);
}
static void test_clear(const h2_pal_fs_api_t* fs) {
  const char* names[] = {"data/a",          "data/sub/b",   "data_sub_c",
                         "dl/image",        "dl_image",     "data-other/a",
                         "database/a",      "other",        "data",
                         "data-other_flat", "database_flat"};
  inventory(names, sizeof(names) / sizeof(names[0]));
  assert(h2_pal_fs_clear(fs, "/data") == 0);
  for (size_t i = 0; i < s_count; ++i) assert(s_alive[i] == (i >= 3));
  assert(h2_pal_fs_clear(fs, "/data") == 0);
  inventory(names, sizeof(names) / sizeof(names[0]));
  assert(h2_pal_fs_clear(fs, "/data/sub") == 0);
  for (size_t i = 0; i < s_count; ++i) assert(s_alive[i] == (i != 1 && i != 2));
  const char* bad[] = {"",          "/",        "data",     "/.",
                       "/..",       "/data/..", "/data/.",  "/data/../dl",
                       "/data/./a", "//data",   "/data//a", "/data/"};
  unsigned opens = s_dir_opens, removes = s_removes;
  for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i)
    assert(fs->vtable->clear(fs->user, bad[i]) == H2_PAL_FS_ERR_INVALID_ARG);
  assert(fs->vtable->clear(fs->user, NULL) == H2_PAL_FS_ERR_INVALID_ARG);
  char long_path[256];
  memset(long_path, 'a', sizeof(long_path));
  long_path[0] = '/';
  long_path[sizeof(long_path) - 1] = '\0';
  assert(h2_pal_fs_clear(fs, long_path) == H2_PAL_FS_ERR_NO_SPACE);
  assert(s_dir_opens == opens && s_removes == removes);
  inventory(names, 1);
  s_open_error = ENOMEM;
  unsigned closes = s_dir_closes;
  assert(h2_pal_fs_clear(fs, "/data") == H2_PAL_FS_ERR_NO_MEMORY);
  assert(s_dir_closes == closes && s_alive[0]);
  s_open_error = 0;
  s_read_error = EIO;
  assert(h2_pal_fs_clear(fs, "/data") == H2_PAL_FS_ERR_IO);
  assert(s_dir_closes == closes + 1 && s_alive[0]);
  s_read_error = 0;
  s_remove_error = ENOSPC;
  s_close_error = EIO;
  assert(h2_pal_fs_clear(fs, "/data") == H2_PAL_FS_ERR_NO_SPACE);
  assert(s_dir_closes == closes + 2 && s_alive[0]);
  s_remove_error = 0;
  assert(h2_pal_fs_clear(fs, "/data") == H2_PAL_FS_ERR_IO);
  assert(!s_alive[0]);
  inventory(names, 1);
  s_safe_error = H2_PAL_ERR_TIMEOUT;
  assert(h2_pal_fs_clear(fs, "/data") == H2_PAL_ERR_TIMEOUT && s_alive[0]);
  s_safe_error = 0;
  char long_name[224];
  memset(long_name, 'a', sizeof(long_name));
  memcpy(long_name, "data/", 5);
  long_name[sizeof(long_name) - 1] = '\0';
  const char* oversized[] = {long_name};
  inventory(oversized, 1);
  assert(h2_pal_fs_clear(fs, "/data") == H2_PAL_FS_ERR_NO_SPACE && s_alive[0]);
  inventory(NULL, 0);
  assert(h2_pal_fs_clear(fs, "/data") == 0);
}
static void test_staging_rename(const h2_pal_fs_api_t* fs) {
  const char* exact[] = {"dl/update.tmp", "dl/keep"};
  inventory(exact, 2u);
  unsigned calls = s_rename_calls;
  assert(h2_pal_fs_rename(fs, "/dl/update.tmp", "/dl/update") == H2_PAL_OK);
  assert(s_rename_calls == calls + 1u);
  assert(strcmp(s_names[0], "dl/update") == 0 && s_alive[1]);

  const char* flat[] = {"dl_update.tmp", "data/keep"};
  inventory(flat, 2u);
  calls = s_rename_calls;
  assert(h2_pal_fs_rename(fs, "/dl/update.tmp", "/dl/update") == H2_PAL_OK);
  assert(s_rename_calls == calls + 2u);
  assert(strcmp(s_names[0], "dl_update") == 0 && s_alive[1]);

  inventory(exact, 2u);
  s_rename_error = EACCES;
  calls = s_rename_calls;
  assert(h2_pal_fs_rename(fs, "/dl/update.tmp", "/dl/update") == H2_PAL_ERR_IO);
  assert(s_rename_calls == calls + 1u);
  assert(strcmp(s_names[0], "dl/update.tmp") == 0 && s_alive[0]);
  s_rename_error = 0;
}

int main(void) {
  h2_pal_fs_api_t fs = {0};
  h2_esp_platform_spiffs_config_t config = {
      .base_path = "/spiffs",
      .partition_label = "storage",
  };
  assert(h2_esp_platform_spiffs_fs_init(NULL, &config) ==
         H2_PAL_FS_ERR_INVALID_ARG);
  assert(h2_esp_platform_spiffs_fs_init(&fs, NULL) ==
         H2_PAL_FS_ERR_INVALID_ARG);
  s_mount_error = -1;
  assert(h2_esp_platform_spiffs_fs_init(&fs, &config) == H2_PAL_FS_ERR_IO);
  assert(fs.vtable == NULL);
  s_mount_error = ESP_OK;
  assert(h2_esp_platform_spiffs_fs_init(&fs, &config) == 0);
  assert(fs.vtable && fs.vtable->seek && fs.vtable->clear);
  test_seek(&fs);
  test_clear(&fs);
  test_staging_rename(&fs);
  assert(h2_esp_platform_spiffs_fs_deinit("storage") == 0);
  assert(!s_worker && !s_locked);
  return 0;
}
