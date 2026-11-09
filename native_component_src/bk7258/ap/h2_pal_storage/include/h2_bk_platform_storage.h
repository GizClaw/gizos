#ifndef H2_BK_PLATFORM_STORAGE_H
#define H2_BK_PLATFORM_STORAGE_H
#include "os/h2_pal_disk.h"
#include "os/h2_pal_fs.h"
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct h2_bk_disk_region {
  uint32_t id, offset, size;
  const char* name;
} h2_bk_disk_region_t;
/* The board owns physical geometry. Configure once before publishing the API;
 * an identical later configuration is harmless. The name is copied. */
int h2_bk_platform_disk_configure(const h2_bk_disk_region_t* region);
const h2_pal_disk_api_t* h2_bk_platform_disk_api(void);

typedef struct h2_bk_sd_mount {
  const char* path;
  const char* native_path;
} h2_bk_sd_mount_t;
typedef struct h2_bk_sd_fs_config {
  const char* drive;
  const char* root;
  const h2_bk_sd_mount_t* mounts;
  size_t mount_count;
} h2_bk_sd_fs_config_t;
/* Configuration strings are borrowed for the filesystem lifetime. Each mount
 * maps a logical path prefix onto a board-selected FATFS directory. */
int h2_bk_platform_sd_fs_init(const h2_bk_sd_fs_config_t* config,
                              h2_pal_fs_api_t* out_fs);
int h2_bk_platform_sd_fs_mount(void* user, const char* path);
int h2_bk_platform_sd_fs_clear(void* user, const char* path);
void h2_bk_platform_sd_fs_prepare(void);
void h2_bk_platform_sd_fs_release(void);
#ifdef __cplusplus
}
#endif
#endif
