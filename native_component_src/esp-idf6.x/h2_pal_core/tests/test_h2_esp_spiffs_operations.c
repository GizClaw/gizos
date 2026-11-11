#include "h2_esp_platform_core.h"
#include "h2_esp_platform_safe_call.h"
#include "esp_heap_caps.h"
#include "esp_spiffs.h"
#include "freertos/semphr.h"

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Compile the production provider. Seek uses real stdio; directory operations
 * expose the flat slash-containing names returned by the ESP SPIFFS VFS. */
static int s_locked, s_worker, s_safe_error, s_seek_error;
static int s_open_error, s_read_error, s_remove_error, s_close_error;
static bool s_mounted, s_format_policy;
static esp_err_t s_mount_result;
static unsigned s_mounts, s_calls, s_opens, s_closes, s_removes;
static const char* s_names[12];
static bool s_alive[12];
static size_t s_count, s_cursor;
static struct dirent s_entry;

static FILE* test_fopen(const char* path, const char* mode) {
  assert(s_locked && s_worker && strcmp(path, "/spiffs/data/file") == 0);
  assert(strcmp(mode, "rb") == 0);
  FILE* file = tmpfile();
  assert(file != NULL);
  assert(fwrite("0123456789", 1u, 10u, file) == 10u);
  rewind(file);
  return file;
}
static int test_fseek(FILE* file, long position, int whence) {
  assert(s_locked && s_worker && whence == SEEK_SET);
  if (s_seek_error != 0) {
    errno = s_seek_error;
    return -1;
  }
  return fseek(file, position, whence);
}
static DIR* test_opendir(const char* path) {
  assert(s_locked && s_worker && strcmp(path, "/spiffs") == 0);
  ++s_opens;
  if (s_open_error != 0) {
    errno = s_open_error;
    return NULL;
  }
  s_cursor = 0u;
  return (DIR*)&s_cursor;
}
static struct dirent* test_readdir(DIR* dir) {
  assert(s_locked && s_worker && dir == (DIR*)&s_cursor);
  if (s_read_error != 0) {
    errno = s_read_error;
    return NULL;
  }
  while (s_cursor < s_count) {
    const size_t index = s_cursor++;
    if (!s_alive[index]) continue;
    assert(strlen(s_names[index]) < sizeof(s_entry.d_name));
    strcpy(s_entry.d_name, s_names[index]);
    return &s_entry;
  }
  return NULL;
}
static int test_closedir(DIR* dir) {
  assert(s_locked && s_worker && dir == (DIR*)&s_cursor);
  ++s_closes;
  if (s_close_error != 0) {
    errno = s_close_error;
    return -1;
  }
  return 0;
}
static int test_remove(const char* path) {
  assert(s_locked && s_worker && strncmp(path, "/spiffs/", 8u) == 0);
  ++s_removes;
  if (s_remove_error != 0) {
    errno = s_remove_error;
    return -1;
  }
  for (size_t index = 0u; index < s_count; ++index) {
    if (s_alive[index] && strcmp(path + 8u, s_names[index]) == 0) {
      s_alive[index] = false;
      return 0;
    }
  }
  assert(!"removal must address an enumerated object");
  return -1;
}

#define fopen(path, mode) test_fopen(path, mode)
#define fseek(file, position, whence) test_fseek(file, position, whence)
#define opendir(path) test_opendir(path)
#define readdir(dir) test_readdir(dir)
#define closedir(dir) test_closedir(dir)
#define remove(path) test_remove(path)
#include "src/h2_esp_platform_spiffs.c"
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
  assert(mutex != NULL && !s_locked && timeout == portMAX_DELAY);
  s_locked = 1;
  return pdTRUE;
}
BaseType_t xSemaphoreGive(SemaphoreHandle_t mutex) {
  assert(mutex != NULL && s_locked && !s_worker);
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
  _Alignas(max_align_t) unsigned char copied[1024];
  assert(s_locked && !s_worker && context_size <= sizeof(copied));
  assert(stack_depth == 4096u);
  ++s_calls;
  if (s_safe_error != 0) return s_safe_error;
  memcpy(copied, context, context_size);
  s_worker = 1;
  callback(copied);
  s_worker = 0;
  memcpy(context, copied, context_size);
  return H2_PAL_OK;
}
bool esp_spiffs_mounted(const char* label) {
  assert(s_locked && s_worker && strcmp(label, "storage") == 0);
  return s_mounted;
}
esp_err_t esp_vfs_spiffs_register(const esp_vfs_spiffs_conf_t* config) {
  assert(s_locked && s_worker);
  assert(strcmp(config->base_path, "/spiffs") == 0);
  assert(strcmp(config->partition_label, "storage") == 0);
  assert(config->max_files == 16u);
  assert(config->format_if_mount_failed == s_format_policy);
  ++s_mounts;
  if (s_mount_result == ESP_OK) s_mounted = true;
  return s_mount_result;
}
esp_err_t esp_vfs_spiffs_unregister(const char* label) {
  assert(s_locked && s_worker && strcmp(label, "storage") == 0);
  s_mounted = false;
  return ESP_OK;
}

static void inventory(const char* const* names, size_t count) {
  assert(count <= sizeof(s_names) / sizeof(s_names[0]));
  s_count = count;
  s_open_error = s_read_error = s_remove_error = s_close_error = 0;
  for (size_t index = 0u; index < count; ++index) {
    s_names[index] = names[index];
    s_alive[index] = true;
  }
}
static void test_seek(const h2_pal_fs_api_t* fs) {
  h2_pal_fs_file_t* file = NULL;
  assert(h2_pal_fs_open(fs, "/data/file", H2_PAL_FS_OPEN_READ, &file) == 0);
  char bytes[16];
  size_t size;
  assert(h2_pal_fs_seek(fs, file, 4u) == 0);
  assert(h2_pal_fs_read(fs, file, bytes, 3u, &size) == 0);
  assert(size == 3u && memcmp(bytes, "456", 3u) == 0);
  assert(h2_pal_fs_read(fs, file, bytes, sizeof(bytes), &size) == 0);
  assert(size == 3u);
  assert(h2_pal_fs_seek(fs, file, 1u) == 0);
  assert(h2_pal_fs_read(fs, file, bytes, 2u, &size) == 0);
  assert(size == 2u && memcmp(bytes, "12", 2u) == 0);
  const unsigned calls = s_calls;
  assert(h2_pal_fs_seek(fs, file, UINT64_MAX) == H2_PAL_ERR_INVALID_ARG);
  assert(h2_pal_fs_seek(fs, file, (uint64_t)LONG_MAX + 1u) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(h2_pal_fs_seek(fs, NULL, 0u) == H2_PAL_ERR_INVALID_ARG);
  assert(s_calls == calls);
  s_seek_error = EIO;
  assert(h2_pal_fs_seek(fs, file, 0u) == H2_PAL_ERR_IO);
  s_seek_error = 0;
  s_safe_error = H2_PAL_ERR_TIMEOUT;
  assert(h2_pal_fs_seek(fs, file, 0u) == H2_PAL_ERR_TIMEOUT);
  s_safe_error = 0;
  assert(h2_pal_fs_seek(fs, file, 0u) == 0);
  assert(h2_pal_fs_close(fs, file) == 0);
}
static void test_clear(const h2_pal_fs_api_t* fs) {
  const char* names[] = {"data/file",       "data/sub/a",
                         "data/sub/b",      "data_backup",
                         "data_name",       "data_.checksum",
                         "data-other/file", "database/file",
                         "dl/update",       "data"};
  const size_t count = sizeof(names) / sizeof(names[0]);
  inventory(names, count);
  assert(h2_pal_fs_clear(fs, "/data") == 0);
  for (size_t index = 0u; index < count; ++index)
    assert(s_alive[index] == (index >= 3u));
  assert(h2_pal_fs_clear(fs, "/data") == 0);
  inventory(names, count);
  assert(h2_pal_fs_clear(fs, "/data/sub") == 0);
  for (size_t index = 0u; index < count; ++index)
    assert(s_alive[index] == (index != 1u && index != 2u));
  const char* invalid[] = {"",         "/",           "data",      "/data/..",
                           "/data/.",  "/data/../dl", "/data/./x", "//data",
                           "/data//x", "/data/"};
  unsigned opens = s_opens, removes = s_removes;
  for (size_t index = 0u; index < sizeof(invalid) / sizeof(invalid[0]); ++index)
    assert(h2_pal_fs_clear(fs, invalid[index]) == H2_PAL_ERR_INVALID_ARG);
  assert(fs->vtable->clear(fs->user, NULL) == H2_PAL_ERR_INVALID_ARG);
  assert(h2_pal_fs_clear(fs, NULL) == H2_PAL_ERR_UNSUPPORTED);
  assert(s_opens == opens && s_removes == removes);
  inventory(names, 1u);
  s_open_error = ENOMEM;
  unsigned closes = s_closes;
  assert(h2_pal_fs_clear(fs, "/data") == H2_PAL_ERR_NO_MEMORY);
  assert(s_closes == closes && s_alive[0]);
  s_open_error = 0;
  s_read_error = EIO;
  assert(h2_pal_fs_clear(fs, "/data") == H2_PAL_ERR_IO);
  assert(s_closes == closes + 1u && s_alive[0]);
  s_read_error = 0;
  s_remove_error = ENOSPC;
  assert(h2_pal_fs_clear(fs, "/data") == H2_PAL_ERR_NO_SPACE);
  assert(s_closes == closes + 2u && s_alive[0]);
  s_remove_error = 0;
  s_close_error = EIO;
  assert(h2_pal_fs_clear(fs, "/data") == H2_PAL_ERR_IO);
  assert(!s_alive[0]);
  inventory(names, 1u);
  s_safe_error = H2_PAL_ERR_TIMEOUT;
  opens = s_opens;
  assert(h2_pal_fs_clear(fs, "/data") == H2_PAL_ERR_TIMEOUT);
  assert(s_opens == opens && s_alive[0]);
  s_safe_error = 0;
  char oversized[224];
  memset(oversized, 'x', sizeof(oversized));
  memcpy(oversized, "data/", 5u);
  oversized[sizeof(oversized) - 1u] = '\0';
  const char* long_name[] = {oversized};
  inventory(long_name, 1u);
  assert(h2_pal_fs_clear(fs, "/data") == H2_PAL_ERR_NO_SPACE);
  assert(s_alive[0]);
  inventory(NULL, 0u);
  assert(h2_pal_fs_clear(fs, "/data") == 0);
}
static void test_prepare(const h2_pal_fs_api_t* fs) {
  unsigned mounts = s_mounts;
  assert(h2_pal_fs_prepare(fs, "/dl/stage") == 0);
  assert(h2_pal_fs_prepare(fs, "/data") == 0);
  assert(s_mounts == mounts);
  const char* invalid[] = {"", "data", "/data/..", "/data/.", "/data//x"};
  unsigned calls = s_calls;
  for (size_t index = 0u; index < sizeof(invalid) / sizeof(invalid[0]); ++index)
    assert(h2_pal_fs_prepare(fs, invalid[index]) == H2_PAL_ERR_INVALID_ARG);
  assert(h2_pal_fs_prepare(fs, NULL) == H2_PAL_ERR_INVALID_ARG);
  assert(s_calls == calls && s_mounts == mounts);
  char oversized[224];
  memset(oversized, 'x', sizeof(oversized));
  oversized[0] = '/';
  oversized[sizeof(oversized) - 1u] = '\0';
  assert(h2_pal_fs_prepare(fs, oversized) == H2_PAL_ERR_NO_SPACE);
  assert(s_calls == calls && s_mounts == mounts);
  assert(h2_pal_fs_mkdir(fs, "/data/virtual") == H2_PAL_OK);
  assert(h2_pal_fs_mkdir(fs, "/data/..") == H2_PAL_ERR_INVALID_ARG);
  s_mounted = false;
  s_mount_result = ESP_ERR_INVALID_STATE;
  assert(h2_pal_fs_prepare(fs, "/data") == H2_PAL_ERR_IO);
  assert(!s_mounted);
  s_mount_result = ESP_OK;
  s_safe_error = H2_PAL_ERR_TIMEOUT;
  assert(h2_pal_fs_prepare(fs, "/data") == H2_PAL_ERR_TIMEOUT);
  s_safe_error = 0;
  assert(h2_pal_fs_prepare(fs, "/data") == 0);
  assert(s_mounted && s_mounts == mounts + 2u);
  assert(h2_pal_fs_prepare(fs, "/data") == 0);
  assert(s_mounts == mounts + 2u);
}
int main(void) {
  h2_esp_platform_spiffs_config_t config = {
      .base_path = "/spiffs",
      .partition_label = "storage",
      .max_files = 16u,
  };
  h2_pal_fs_api_t fs = {0};
  s_mount_result = -1;
  assert(h2_esp_platform_spiffs_fs_init(&fs, &config) == H2_PAL_ERR_IO);
  assert(fs.vtable == NULL);
  s_mount_result = ESP_OK;
  assert(h2_esp_platform_spiffs_fs_init(&fs, &config) == 0);
  assert(fs.vtable->seek && fs.vtable->clear && fs.vtable->prepare);
  test_seek(&fs);
  test_clear(&fs);
  test_prepare(&fs);
  assert(h2_esp_platform_spiffs_fs_deinit("storage") == 0);
  s_format_policy = config.format_if_mount_failed = true;
  assert(h2_esp_platform_spiffs_fs_init(&fs, &config) == 0);
  s_mounted = false;
  assert(h2_pal_fs_prepare(&fs, "/data") == 0);
  assert(h2_esp_platform_spiffs_fs_deinit("storage") == 0);
  assert(!s_locked && !s_worker);
  return 0;
}
