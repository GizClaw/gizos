#include "h2_bk_platform_storage.h"

#if defined(H2_BK_SD_FS_HOST_TEST) || \
    (defined(H2_BK_SD_FS_ENABLED) && H2_BK_SD_FS_ENABLED)

#ifdef H2_BK_SD_FS_HOST_TEST
#include "h2_bk_sd_fs_test_sdk.h"
#else
#include "driver/pwr_clk.h"
#include "driver/sd_card.h"
#include "driver/sdio_host.h"
#include "diskio.h"
#include "ff.h"
#include "gpio_driver.h"
#include "core/h2_pal_errors.h"
#include "os/mem.h"
#include "os/os.h"
#endif

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define H2_BK_FS_PATH_MAX 192u
#define H2_BK_SD_MOUNT_ATTEMPTS 5u
#define H2_BK_SD_POWER_OFF_MS 1000u
#define H2_BK_SD_POWER_ON_MS 2000u

struct h2_pal_fs_file {
  FIL file;
};

static h2_bk_sd_fs_config_t s_config;
static FATFS s_fs;
static int s_mounted;

static void sd_power_set(gpio_output_state_e state) {
#if CONFIG_SDCARD_POWER_GPIO_CTRL
  (void)bk_pm_module_vote_ctrl_external_ldo(GPIO_CTRL_LDO_MODULE_SDIO,
                                            CONFIG_LDO3V3_CTRL_GPIO, state);
#else
  (void)state;
#endif
}

static void sd_map_pins(void) {
#if CONFIG_SDIO_4LINES_EN
  (void)gpio_sdio_sel(GPIO_SDIO_MAP_MODE0);
#else
  (void)gpio_sdio_one_line_sel(GPIO_SDIO_MAP_MODE0);
#endif
}

void h2_bk_platform_sd_fs_release(void) {
  if (s_config.drive) (void)f_mount(NULL, s_config.drive, 0);
  (void)disk_uninitialize(DISK_NUMBER_SDIO_SD);
  s_mounted = 0;
}

static int prepare_sd_storage(void) {
  h2_bk_platform_sd_fs_release();
  (void)bk_sd_card_deinit();
  (void)bk_sdio_host_driver_deinit();
  sd_power_set(GPIO_OUTPUT_STATE_LOW);
  rtos_delay_milliseconds(H2_BK_SD_POWER_OFF_MS);
  if (s_config.select_pins != NULL) {
    int rc = s_config.select_pins();
    if (rc != H2_PAL_OK) return rc;
  } else {
    sd_map_pins();
  }
  sd_power_set(GPIO_OUTPUT_STATE_HIGH);
  rtos_delay_milliseconds(H2_BK_SD_POWER_ON_MS);
  return bk_sdio_host_driver_init() == 0 ? H2_PAL_OK : H2_PAL_ERR_IO;
}

void h2_bk_platform_sd_fs_prepare(void) { (void)prepare_sd_storage(); }

static int map_fresult(FRESULT fr) {
  switch (fr) {
    case FR_OK:
      return H2_PAL_OK;
    case FR_NO_FILE:
    case FR_NO_PATH:
      return H2_PAL_ERR_NOT_FOUND;
    case FR_DENIED:
    case FR_WRITE_PROTECTED:
      return H2_PAL_ERR_IO;
    case FR_NOT_ENOUGH_CORE:
      return H2_PAL_ERR_NO_MEMORY;
    case FR_TIMEOUT:
      return H2_PAL_ERR_TIMEOUT;
    default:
      return H2_PAL_ERR_IO;
  }
}

static int append_suffix(char* out, size_t out_len, const char* root,
                         const char* suffix) {
  int written;

  if (out == NULL || root == NULL || suffix == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  written = snprintf(out, out_len, "%s%s", root, suffix);
  if (written < 0 || (size_t)written >= out_len) {
    return H2_PAL_ERR_NO_MEMORY;
  }
  return H2_PAL_OK;
}

static int translate_path(const char* path, char* out, size_t out_len) {
  if (path == NULL || out == NULL || out_len == 0u) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  for (size_t i = 0; i < s_config.mount_count; ++i) {
    const h2_bk_sd_mount_t* mount = &s_config.mounts[i];
    size_t prefix = strlen(mount->path);
    if (!strncmp(path, mount->path, prefix) &&
        (path[prefix] == '\0' || path[prefix] == '/'))
      return append_suffix(out, out_len, mount->native_path, path + prefix);
  }
  return H2_PAL_ERR_INVALID_ARG;
}

static int ensure_dir(const char* path) {
  FRESULT fr = f_mkdir(path);
  return fr == FR_OK || fr == FR_EXIST ? H2_PAL_OK : map_fresult(fr);
}

static int fs_mkdir(void* user, const char* path) {
  char mapped[H2_BK_FS_PATH_MAX];
  int rc;

  (void)user;
  rc = translate_path(path, mapped, sizeof(mapped));
  if (rc != H2_PAL_OK) {
    return rc;
  }
  return ensure_dir(mapped);
}

static int fs_open(void* user, const char* path, h2_pal_fs_open_mode_t mode,
                   h2_pal_fs_file_t** out_file) {
  char mapped[H2_BK_FS_PATH_MAX];
  BYTE fatfs_mode;
  h2_pal_fs_file_t* file;
  int rc;
  FRESULT fr;

  (void)user;
  if (out_file == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  *out_file = NULL;
  rc = translate_path(path, mapped, sizeof(mapped));
  if (rc != H2_PAL_OK) {
    return rc;
  }
  switch (mode) {
    case H2_PAL_FS_OPEN_WRITE_TRUNCATE:
      fatfs_mode = FA_WRITE | FA_CREATE_ALWAYS;
      break;
    case H2_PAL_FS_OPEN_READ:
      fatfs_mode = FA_READ;
      break;
    default:
      return H2_PAL_ERR_INVALID_ARG;
  }
  file = (h2_pal_fs_file_t*)os_malloc(sizeof(*file));
  if (file == NULL) {
    return H2_PAL_ERR_NO_MEMORY;
  }
  fr = f_open(&file->file, mapped, fatfs_mode);
  if (fr != FR_OK) {
    os_free(file);
    return map_fresult(fr);
  }
  *out_file = file;
  return H2_PAL_OK;
}

static int fs_read(void* user, h2_pal_fs_file_t* file, void* data, size_t len,
                   size_t* out_read) {
  UINT n = 0u;
  FRESULT fr;

  (void)user;
  if (file == NULL || (data == NULL && len != 0u) || out_read == NULL ||
      len > UINT_MAX) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  *out_read = 0u;
  fr = f_read(&file->file, data, (UINT)len, &n);
  if (fr != FR_OK) {
    return map_fresult(fr);
  }
  *out_read = (size_t)n;
  return H2_PAL_OK;
}

static int fs_write(void* user, h2_pal_fs_file_t* file, const void* data,
                    size_t len, size_t* out_written) {
  UINT n = 0u;
  FRESULT fr;

  (void)user;
  if (file == NULL || (data == NULL && len != 0u) || out_written == NULL ||
      len > UINT_MAX) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  *out_written = 0u;
  fr = f_write(&file->file, data, (UINT)len, &n);
  if (fr != FR_OK) {
    return map_fresult(fr);
  }
  *out_written = (size_t)n;
  return H2_PAL_OK;
}

static int fs_seek(void* user, h2_pal_fs_file_t* file, uint64_t position) {
  (void)user;
  if (file == NULL || (uint64_t)(FSIZE_t)position != position ||
      position > (uint64_t)f_size(&file->file))
    return H2_PAL_ERR_INVALID_ARG;
  return map_fresult(f_lseek(&file->file, (FSIZE_t)position));
}

static int fs_sync(void* user, h2_pal_fs_file_t* file) {
  (void)user;
  if (file == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  return map_fresult(f_sync(&file->file));
}

static int fs_close(void* user, h2_pal_fs_file_t* file) {
  FRESULT fr;

  (void)user;
  if (file == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  fr = f_close(&file->file);
  os_free(file);
  return map_fresult(fr);
}

static int fs_stat(void* user, const char* path, h2_pal_fs_stat_t* out_stat) {
  char mapped[H2_BK_FS_PATH_MAX];
  FILINFO info;
  int rc;
  FRESULT fr;

  (void)user;
  if (out_stat == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  memset(out_stat, 0, sizeof(*out_stat));
  rc = translate_path(path, mapped, sizeof(mapped));
  if (rc != H2_PAL_OK) {
    return rc;
  }
  fr = f_stat(mapped, &info);
  if (fr != FR_OK) {
    return map_fresult(fr);
  }
  out_stat->size = (uint64_t)info.fsize;
  out_stat->is_dir = (info.fattrib & AM_DIR) != 0u;
  return H2_PAL_OK;
}

static int fs_remove(void* user, const char* path) {
  char mapped[H2_BK_FS_PATH_MAX];
  int rc;

  (void)user;
  rc = translate_path(path, mapped, sizeof(mapped));
  if (rc != H2_PAL_OK) {
    return rc;
  }
  return map_fresult(f_unlink(mapped));
}

static int fs_rename(void* user, const char* old_path, const char* new_path) {
  char old_mapped[H2_BK_FS_PATH_MAX];
  char new_mapped[H2_BK_FS_PATH_MAX];
  int rc;

  (void)user;
  rc = translate_path(old_path, old_mapped, sizeof(old_mapped));
  if (rc != H2_PAL_OK) {
    return rc;
  }
  rc = translate_path(new_path, new_mapped, sizeof(new_mapped));
  if (rc != H2_PAL_OK) {
    return rc;
  }
  return map_fresult(f_rename(old_mapped, new_mapped));
}

static int join_fatfs_path(char* out, size_t out_len, const char* dir,
                           const char* name) {
  int written;

  if (out == NULL || dir == NULL || name == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  written = snprintf(out, out_len, "%s/%s", dir, name);
  if (written < 0 || (size_t)written >= out_len) {
    return H2_PAL_ERR_NO_MEMORY;
  }
  return H2_PAL_OK;
}

static int clear_dir_children(const char* dir_path) {
  DIR dir;
  FILINFO info;
  FRESULT fr;
  int rc = H2_PAL_OK;

  fr = f_opendir(&dir, dir_path);
  if (fr != FR_OK) {
    return map_fresult(fr);
  }

  for (;;) {
    char child[H2_BK_FS_PATH_MAX];

    fr = f_readdir(&dir, &info);
    if (fr != FR_OK) {
      rc = map_fresult(fr);
      break;
    }
    if (info.fname[0] == '\0') {
      break;
    }
    if (strcmp(info.fname, ".") == 0 || strcmp(info.fname, "..") == 0) {
      continue;
    }

    rc = join_fatfs_path(child, sizeof(child), dir_path, info.fname);
    if (rc != H2_PAL_OK) {
      break;
    }
    if ((info.fattrib & AM_DIR) != 0u) {
      rc = clear_dir_children(child);
      if (rc != H2_PAL_OK) {
        break;
      }
    }
    rc = map_fresult(f_unlink(child));
    if (rc != H2_PAL_OK) {
      break;
    }
  }

  fr = f_closedir(&dir);
  if (rc == H2_PAL_OK && fr != FR_OK) {
    rc = map_fresult(fr);
  }
  return rc;
}

static int mount_sd(const char* path, bool factory) {
  char mapped[H2_BK_FS_PATH_MAX];
  int rc = translate_path(path, mapped, sizeof(mapped));
  if (rc != H2_PAL_OK || !s_config.drive) return H2_PAL_ERR_INVALID_ARG;
  if (factory && s_config.card_present == NULL) return H2_PAL_ERR_UNSUPPORTED;
  if (s_config.card_present != NULL && !s_config.card_present()) {
    if (s_mounted) h2_bk_platform_sd_fs_release();
    return H2_PAL_ERR_UNAVAILABLE;
  }
  if (factory || !s_mounted) {
    FRESULT fr = FR_DISK_ERR;
    const unsigned limit = factory ? 1u : H2_BK_SD_MOUNT_ATTEMPTS;
    for (unsigned attempt = 0; attempt < limit; ++attempt) {
      rc = prepare_sd_storage();
      if (rc != H2_PAL_OK) return rc;
      if (s_config.card_present != NULL && !s_config.card_present())
        return H2_PAL_ERR_UNAVAILABLE;
      fr = f_mount(&s_fs, s_config.drive, 1);
      if (fr == FR_OK) break;
      os_printf("H2_BK_SD_MOUNT_RETRY attempt=%u fatfs=%d rc=%d\r\n",
                attempt + 1u, (int)fr, map_fresult(fr));
    }
    if (factory && fr == FR_NO_FILESYSTEM && s_config.card_present()) {
      BYTE* work = os_malloc(FF_MAX_SS);
      if (work == NULL) {
        h2_bk_platform_sd_fs_release();
        return H2_PAL_ERR_NO_MEMORY;
      }
      fr = f_mkfs(s_config.drive, FM_ANY, 0, work, FF_MAX_SS);
      os_free(work);
      os_printf("H2_BK_SD_FORMAT fatfs=%d\r\n", (int)fr);
      if (fr == FR_OK && !s_config.card_present()) {
        h2_bk_platform_sd_fs_release();
        return H2_PAL_ERR_UNAVAILABLE;
      }
      if (fr == FR_OK) fr = f_mount(&s_fs, s_config.drive, 1);
    }
    if (fr != FR_OK) {
      h2_bk_platform_sd_fs_release();
      return map_fresult(fr);
    }
    if (s_config.card_present != NULL && !s_config.card_present()) {
      h2_bk_platform_sd_fs_release();
      return H2_PAL_ERR_UNAVAILABLE;
    }
    rc = ensure_dir(s_config.root);
    if (rc != H2_PAL_OK) {
      h2_bk_platform_sd_fs_release();
      return rc;
    }
    s_mounted = 1;
  }
  return ensure_dir(mapped);
}

int h2_bk_platform_sd_fs_mount(void* user, const char* path) {
  (void)user;
  return mount_sd(path, false);
}

int h2_bk_platform_sd_fs_factory_mount(const char* path) {
  return mount_sd(path, true);
}

int h2_bk_platform_sd_fs_clear(void* user, const char* path) {
  (void)user;
  char mapped[H2_BK_FS_PATH_MAX];
  int rc = translate_path(path, mapped, sizeof(mapped));
  if (rc != H2_PAL_OK) return rc;
  rc = h2_bk_platform_sd_fs_mount(NULL, path);
  return rc == H2_PAL_OK ? clear_dir_children(mapped) : rc;
}

int h2_bk_platform_sd_fs_init(const h2_bk_sd_fs_config_t* config,
                              h2_pal_fs_api_t* fs) {
  static const h2_pal_fs_vtable_t api_vtable = {
      .mkdir = fs_mkdir,
      .open = fs_open,
      .read = fs_read,
      .write = fs_write,
      .seek = fs_seek,
      .sync = fs_sync,
      .close = fs_close,
      .stat = fs_stat,
      .clear = h2_bk_platform_sd_fs_clear,
      .remove = fs_remove,
      .rename = fs_rename,
      .prepare = h2_bk_platform_sd_fs_mount,
  };

  static const h2_pal_fs_api_t api = {
      .user = NULL,
      .vtable = &api_vtable,
  };

  if (fs == NULL || config == NULL || !config->drive || !config->root ||
      !config->mounts || !config->mount_count) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (s_config.drive && s_config.drive != config->drive)
    return H2_PAL_ERR_INVALID_STATE;
  for (size_t i = 0; i < config->mount_count; ++i)
    if (!config->mounts[i].path || !config->mounts[i].native_path ||
        config->mounts[i].path[0] != '/' || !config->mounts[i].path[1])
      return H2_PAL_ERR_INVALID_ARG;
  s_config = *config;
  *fs = api;
  return H2_PAL_OK;
}

#else

#include "h2_pal_unsupported.h"

int h2_bk_platform_sd_fs_init(const h2_bk_sd_fs_config_t* config,
                              h2_pal_fs_api_t* fs) {
  if (config == NULL || fs == NULL) return H2_PAL_ERR_INVALID_ARG;
  *fs = *h2_pal_unsupported_fs_api();
  return H2_PAL_ERR_UNSUPPORTED;
}

int h2_bk_platform_sd_fs_mount(void* user, const char* path) {
  (void)user;
  return path == NULL ? H2_PAL_ERR_INVALID_ARG : H2_PAL_ERR_UNSUPPORTED;
}

int h2_bk_platform_sd_fs_factory_mount(const char* path) {
  return path == NULL ? H2_PAL_ERR_INVALID_ARG : H2_PAL_ERR_UNSUPPORTED;
}

int h2_bk_platform_sd_fs_clear(void* user, const char* path) {
  (void)user;
  return path == NULL ? H2_PAL_ERR_INVALID_ARG : H2_PAL_ERR_UNSUPPORTED;
}

void h2_bk_platform_sd_fs_prepare(void) {}
void h2_bk_platform_sd_fs_release(void) {}

#endif
