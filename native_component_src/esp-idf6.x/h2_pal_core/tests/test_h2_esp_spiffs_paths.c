#include "h2_esp_platform_core.h"
#include "h2_esp_platform_safe_call.h"
#include "esp_heap_caps.h"
#include "esp_spiffs.h"
#include "freertos/semphr.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Exercise the real provider and host stdio; only the SDK mount and worker
 * boundaries are replaced. Slash and underscore paths are separate files. */
static char s_root[96];
static int s_locked;

SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t* storage) {
  return storage;
}
BaseType_t xSemaphoreTake(SemaphoreHandle_t mutex, uint32_t timeout) {
  assert(mutex != NULL && timeout == portMAX_DELAY && !s_locked);
  s_locked = 1;
  return pdTRUE;
}
BaseType_t xSemaphoreGive(SemaphoreHandle_t mutex) {
  assert(mutex != NULL && s_locked);
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
  assert(s_locked && context_size <= sizeof(copied) && stack_depth == 4096u);
  memcpy(copied, context, context_size);
  callback(copied);
  memcpy(context, copied, context_size);
  return H2_PAL_OK;
}
esp_err_t esp_vfs_spiffs_register(const esp_vfs_spiffs_conf_t* config) {
  assert(s_locked && strcmp(config->base_path, s_root) == 0);
  assert(strcmp(config->partition_label, "storage") == 0);
  assert(!config->format_if_mount_failed);
  return ESP_OK;
}
esp_err_t esp_vfs_spiffs_unregister(const char* label) {
  assert(s_locked && strcmp(label, "storage") == 0);
  return ESP_OK;
}

static void put(const h2_pal_fs_api_t* fs, const char* path, const char* text) {
  h2_pal_fs_file_t* file = NULL;
  size_t written = 0u;
  assert(h2_pal_fs_open(fs, path, H2_PAL_FS_OPEN_WRITE_TRUNCATE, &file) == 0);
  assert(h2_pal_fs_write(fs, file, text, strlen(text), &written) == 0);
  assert(written == strlen(text));
  assert(h2_pal_fs_sync(fs, file) == 0);
  assert(h2_pal_fs_close(fs, file) == 0);
}
static void expect(const h2_pal_fs_api_t* fs, const char* path,
                   const char* text) {
  h2_pal_fs_file_t* file = NULL;
  char data[64];
  size_t count = 0u;
  assert(h2_pal_fs_open(fs, path, H2_PAL_FS_OPEN_READ, &file) == 0);
  assert(h2_pal_fs_read(fs, file, data, sizeof(data), &count) == 0);
  assert(count == strlen(text) && memcmp(data, text, count) == 0);
  assert(h2_pal_fs_close(fs, file) == 0);
  h2_pal_fs_stat_t st;
  assert(h2_pal_fs_stat(fs, path, &st) == 0);
  assert(!st.is_dir && st.size == strlen(text));
}

int main(void) {
  const int root_len = snprintf(s_root, sizeof(s_root),
                                "/tmp/h2-spiffs-path-%ld", (long)getpid());
  assert(root_len > 0 && (size_t)root_len < sizeof(s_root));
  assert(mkdir(s_root, 0700) == 0);
  char directory[128];
  const int n = snprintf(directory, sizeof(directory), "%s/data", s_root);
  assert(n > 0 && (size_t)n < sizeof(directory));
  assert(mkdir(directory, 0700) == 0);
  const h2_esp_platform_spiffs_config_t config = {
      .base_path = s_root,
      .partition_label = "storage",
      .max_files = 16u,
      .format_if_mount_failed = false,
  };
  h2_pal_fs_api_t fs;
  assert(h2_esp_platform_spiffs_fs_init(&fs, &config) == 0);

  put(&fs, "/data/old", "native");
  put(&fs, "/data_old", "underscore source");
  put(&fs, "/data_new", "underscore destination");
  expect(&fs, "/data/old", "native");
  expect(&fs, "/data_old", "underscore source");
  assert(h2_pal_fs_rename(&fs, "/data/old", "/data/new") == 0);
  expect(&fs, "/data/new", "native");
  expect(&fs, "/data_old", "underscore source");
  expect(&fs, "/data_new", "underscore destination");
  assert(h2_pal_fs_remove(&fs, "/data/new") == 0);
  expect(&fs, "/data_new", "underscore destination");

  /* Failure must not retry a different name, including write/truncate. */
  put(&fs, "/missing_name", "preserve");
  h2_pal_fs_file_t* file = NULL;
  assert(h2_pal_fs_open(&fs, "/missing/name", H2_PAL_FS_OPEN_READ, &file) ==
         H2_PAL_FS_ERR_NOT_FOUND);
  assert(file == NULL);
  assert(h2_pal_fs_open(&fs, "/missing/name", H2_PAL_FS_OPEN_WRITE_TRUNCATE,
                        &file) == H2_PAL_FS_ERR_NOT_FOUND);
  assert(file == NULL);
  h2_pal_fs_stat_t st;
  assert(h2_pal_fs_stat(&fs, "/missing/name", &st) == H2_PAL_FS_ERR_NOT_FOUND);
  assert(h2_pal_fs_remove(&fs, "/missing/name") == H2_PAL_FS_ERR_NOT_FOUND);
  assert(h2_pal_fs_rename(&fs, "/missing/name", "/data/new") ==
         H2_PAL_FS_ERR_NOT_FOUND);
  expect(&fs, "/missing_name", "preserve");
  expect(&fs, "/data_new", "underscore destination");
  assert(h2_pal_fs_stat(&fs, "/data/new", &st) == H2_PAL_FS_ERR_NOT_FOUND);

  /* The explicit underscore name is still an ordinary accessible path. */
  assert(h2_pal_fs_rename(&fs, "/data_old", "/data_explicit") == 0);
  expect(&fs, "/data_explicit", "underscore source");
  assert(h2_pal_fs_remove(&fs, "/data_explicit") == 0);
  assert(h2_pal_fs_remove(&fs, "/data_new") == 0);
  assert(h2_pal_fs_remove(&fs, "/missing_name") == 0);
  assert(h2_esp_platform_spiffs_fs_deinit("storage") == 0);
  assert(rmdir(directory) == 0 && rmdir(s_root) == 0);
  return 0;
}
