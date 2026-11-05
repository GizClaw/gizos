#include "h2_bk7258_board.h"
#include "h2_bk7258_board_private.h"

#include "h2_zero_bk_1_0_board_config.h"

#include "bk_filesystem.h"
#include "bk_partition.h"
#include "bk_vfs.h"
#include "driver/gpio.h"
#include "gpio_driver.h"
#include "h2_bk_platform_core.h"
#include "lfs.h"
#include "os/mem.h"
#include "os/os.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#define H2_ZERO_BK_FS_PATH_MAX 192u
#define H2_ZERO_BK_QSPI_LFS_MOUNT VFS_QSPI_FLASH_0_PATITION_0
#define H2_ZERO_BK_STORAGE_ROOT H2_ZERO_BK_QSPI_LFS_MOUNT "/h2loader"
#define H2_ZERO_BK_STORAGE_DL_ROOT H2_ZERO_BK_STORAGE_ROOT "/dl"
#define H2_ZERO_BK_STORAGE_DATA_ROOT H2_ZERO_BK_STORAGE_ROOT "/data"
#define H2_ZERO_BK_PERI_EN_PIN GPIO_13

struct h2_pal_fs_file {
  int fd;
};

static int s_storage_mounted;

static int map_errno(int error) {
  switch (error) {
    case ENOENT:
      return H2_PAL_ERR_NOT_FOUND;
    case ENOMEM:
      return H2_PAL_ERR_NO_MEMORY;
    case EINVAL:
      return H2_PAL_ERR_INVALID_ARG;
    default:
      return H2_PAL_ERR_IO;
  }
}

static int map_lfs_result(int result) {
  switch (result) {
    case LFS_ERR_OK:
      return H2_PAL_OK;
    case LFS_ERR_NOENT:
    case LFS_ERR_NOATTR:
      return H2_PAL_ERR_NOT_FOUND;
    case LFS_ERR_NOMEM:
      return H2_PAL_ERR_NO_MEMORY;
    case LFS_ERR_NOSPC:
    case LFS_ERR_FBIG:
      return H2_PAL_ERR_NO_SPACE;
    case LFS_ERR_INVAL:
      return H2_PAL_ERR_INVALID_ARG;
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
  return written >= 0 && (size_t)written < out_len ? H2_PAL_OK
                                                   : H2_PAL_ERR_NO_MEMORY;
}

static int translate_path(const char* path, char* out, size_t out_len) {
  if (path == NULL || out == NULL || out_len == 0u) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (strcmp(path, H2_BK7258_DL_MOUNT_PATH) == 0) {
    return append_suffix(out, out_len, H2_ZERO_BK_STORAGE_DL_ROOT, "");
  }
  if (strncmp(path, H2_BK7258_DL_MOUNT_PATH "/", 4u) == 0) {
    return append_suffix(out, out_len, H2_ZERO_BK_STORAGE_DL_ROOT, path + 3u);
  }
  if (strcmp(path, H2_BK7258_DATA_MOUNT_PATH) == 0) {
    return append_suffix(out, out_len, H2_ZERO_BK_STORAGE_DATA_ROOT, "");
  }
  if (strncmp(path, H2_BK7258_DATA_MOUNT_PATH "/", 6u) == 0) {
    return append_suffix(out, out_len, H2_ZERO_BK_STORAGE_DATA_ROOT, path + 5u);
  }
  return H2_PAL_ERR_INVALID_ARG;
}

static int ensure_dir(const char* path) {
  int result = bk_vfs_mkdir(path, 0777);

  if (result == LFS_ERR_OK || result == LFS_ERR_EXIST) {
    return H2_PAL_OK;
  }
  return map_lfs_result(result);
}

static void external_flash_power_enable(void) {
  gpio_config_t config = {
      .io_mode = GPIO_OUTPUT_ENABLE,
      .pull_mode = GPIO_PULL_DISABLE,
      .func_mode = GPIO_SECOND_FUNC_DISABLE,
  };

  (void)gpio_dev_unmap(H2_ZERO_BK_PERI_EN_PIN);
  (void)bk_gpio_set_config(H2_ZERO_BK_PERI_EN_PIN, &config);
  (void)bk_gpio_set_output_high(H2_ZERO_BK_PERI_EN_PIN);
  rtos_delay_milliseconds(10);
}

static int ensure_storage_dirs(void) {
  int rc = ensure_dir(H2_ZERO_BK_STORAGE_ROOT);

  if (rc == H2_PAL_OK) {
    rc = ensure_dir(H2_ZERO_BK_STORAGE_DL_ROOT);
  }
  if (rc == H2_PAL_OK) {
    rc = ensure_dir(H2_ZERO_BK_STORAGE_DATA_ROOT);
  }
  return rc;
}

static int mount_storage(void) {
  struct bk_little_fs_partition partition = {
      .part_type = LFS_QSPI_FLASH,
      .mount_path = H2_ZERO_BK_QSPI_LFS_MOUNT,
      .part_flash =
          {
              .start_addr = 0u,
              .size = H2_ZERO_BK_1_0_EXTFLASH_SIZE_BYTES,
          },
  };
  int ret;

  if (s_storage_mounted) {
    return ensure_storage_dirs();
  }
  external_flash_power_enable();
  ret = bk_vfs_mount("SOURCE_NONE", partition.mount_path, "littlefs", 0,
                     &partition);
  if (ret != 0) {
    os_printf("H2_ZERO_BK_QSPI_LFS_MOUNT_FAIL rc=%d\r\n", ret);
    return H2_PAL_ERR_IO;
  }
  s_storage_mounted = 1;
  return ensure_storage_dirs();
}

static int fs_mkdir(void* user, const char* path) {
  char mapped[H2_ZERO_BK_FS_PATH_MAX];
  int rc;

  (void)user;
  rc = mount_storage();
  if (rc != H2_PAL_OK) {
    return rc;
  }
  rc = translate_path(path, mapped, sizeof(mapped));
  return rc == H2_PAL_OK ? ensure_dir(mapped) : rc;
}

static int fs_open(void* user, const char* path, h2_pal_fs_open_mode_t mode,
                   h2_pal_fs_file_t** out_file) {
  char mapped[H2_ZERO_BK_FS_PATH_MAX];
  h2_pal_fs_file_t* file;
  int flags;
  int rc;

  (void)user;
  if (out_file == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  *out_file = NULL;
  rc = mount_storage();
  if (rc != H2_PAL_OK) {
    return rc;
  }
  rc = translate_path(path, mapped, sizeof(mapped));
  if (rc != H2_PAL_OK) {
    return rc;
  }
  flags = mode == H2_PAL_FS_OPEN_READ             ? O_RDONLY
          : mode == H2_PAL_FS_OPEN_WRITE_TRUNCATE ? O_WRONLY | O_CREAT | O_TRUNC
                                                  : -1;
  if (flags < 0) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (mode == H2_PAL_FS_OPEN_READ) {
    struct stat info;
    int stat_result = bk_vfs_stat(mapped, &info);

    if (stat_result != LFS_ERR_OK) {
      return map_lfs_result(stat_result);
    }
  }
  file = os_malloc(sizeof(*file));
  if (file == NULL) {
    return H2_PAL_ERR_NO_MEMORY;
  }
  file->fd = bk_vfs_open(mapped, flags);
  if (file->fd < 0) {
    rc = map_errno(errno);
    os_free(file);
    return rc;
  }
  *out_file = file;
  return H2_PAL_OK;
}

static int fs_read(void* user, h2_pal_fs_file_t* file, void* data, size_t len,
                   size_t* out_read) {
  ssize_t result;

  (void)user;
  if (file == NULL || (data == NULL && len != 0u) || out_read == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  result = bk_vfs_read(file->fd, data, len);
  if (result < 0) {
    return map_lfs_result((int)result);
  }
  *out_read = (size_t)result;
  return H2_PAL_OK;
}

static int fs_seek(void* user, h2_pal_fs_file_t* file, uint64_t position) {
  (void)user;
  if (file == NULL || position > LONG_MAX) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  return bk_vfs_lseek(file->fd, (off_t)position, SEEK_SET) >= 0 ? H2_PAL_OK
                                                                : H2_PAL_ERR_IO;
}

static int fs_write(void* user, h2_pal_fs_file_t* file, const void* data,
                    size_t len, size_t* out_written) {
  ssize_t result;

  (void)user;
  if (file == NULL || (data == NULL && len != 0u) || out_written == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  result = bk_vfs_write(file->fd, data, len);
  if (result < 0) {
    return map_lfs_result((int)result);
  }
  *out_written = (size_t)result;
  return H2_PAL_OK;
}

static int fs_sync(void* user, h2_pal_fs_file_t* file) {
  (void)user;
  if (file == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  return map_lfs_result(bk_vfs_fsync(file->fd));
}

static int fs_close(void* user, h2_pal_fs_file_t* file) {
  int ret;

  (void)user;
  if (file == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  ret = bk_vfs_close(file->fd);
  os_free(file);
  return map_lfs_result(ret);
}

static int fs_stat(void* user, const char* path, h2_pal_fs_stat_t* out_stat) {
  char mapped[H2_ZERO_BK_FS_PATH_MAX];
  struct stat info;
  int rc;

  (void)user;
  if (out_stat == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  rc = translate_path(path, mapped, sizeof(mapped));
  if (rc != H2_PAL_OK) {
    return rc;
  }
  rc = bk_vfs_stat(mapped, &info);
  if (rc != LFS_ERR_OK) {
    return map_lfs_result(rc);
  }
  out_stat->size = (uint64_t)info.st_size;
  out_stat->is_dir = S_ISDIR(info.st_mode);
  return H2_PAL_OK;
}

static int fs_remove(void* user, const char* path) {
  char mapped[H2_ZERO_BK_FS_PATH_MAX];
  int rc;

  (void)user;
  rc = translate_path(path, mapped, sizeof(mapped));
  if (rc != H2_PAL_OK) {
    return rc;
  }
  return map_lfs_result(bk_vfs_unlink(mapped));
}

static int fs_rename(void* user, const char* old_path, const char* new_path) {
  char old_mapped[H2_ZERO_BK_FS_PATH_MAX];
  char new_mapped[H2_ZERO_BK_FS_PATH_MAX];
  int rc;

  (void)user;
  rc = translate_path(old_path, old_mapped, sizeof(old_mapped));
  if (rc == H2_PAL_OK) {
    rc = translate_path(new_path, new_mapped, sizeof(new_mapped));
  }
  if (rc != H2_PAL_OK) {
    return rc;
  }
  return map_lfs_result(bk_vfs_rename(old_mapped, new_mapped));
}

static int clear_dir_children(const char* path) {
  DIR* dir = bk_vfs_opendir(path);
  struct dirent* entry;

  if (dir == NULL) {
    return map_errno(errno);
  }
  while ((entry = bk_vfs_readdir(dir)) != NULL) {
    char child[H2_ZERO_BK_FS_PATH_MAX];
    struct stat info;
    int written;
    int rc;

    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
      continue;
    }
    written = snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
    if (written < 0 || (size_t)written >= sizeof(child)) {
      (void)bk_vfs_closedir(dir);
      return H2_PAL_ERR_NO_MEMORY;
    }
    int stat_result = bk_vfs_stat(child, &info);
    if (stat_result != LFS_ERR_OK) {
      (void)bk_vfs_closedir(dir);
      return map_lfs_result(stat_result);
    }
    if (S_ISDIR(info.st_mode)) {
      rc = clear_dir_children(child);
      int remove_result = rc == H2_PAL_OK ? bk_vfs_rmdir(child) : LFS_ERR_OK;
      if (rc != H2_PAL_OK || remove_result != LFS_ERR_OK) {
        (void)bk_vfs_closedir(dir);
        return rc != H2_PAL_OK ? rc : map_lfs_result(remove_result);
      }
    } else {
      int remove_result = bk_vfs_unlink(child);
      if (remove_result != LFS_ERR_OK) {
        (void)bk_vfs_closedir(dir);
        return map_lfs_result(remove_result);
      }
    }
  }
  return map_lfs_result(bk_vfs_closedir(dir));
}

static int fs_clear(void* user, const char* path) {
  char mapped[H2_ZERO_BK_FS_PATH_MAX];
  int rc;

  (void)user;
  rc = translate_path(path, mapped, sizeof(mapped));
  return rc == H2_PAL_OK ? clear_dir_children(mapped) : rc;
}

int h2_bk7258_board_fs_init(h2_pal_fs_api_t* fs) {
  static const h2_pal_fs_vtable_t vtable = {
      .mkdir = fs_mkdir,
      .open = fs_open,
      .read = fs_read,
      .seek = fs_seek,
      .write = fs_write,
      .sync = fs_sync,
      .close = fs_close,
      .stat = fs_stat,
      .clear = fs_clear,
      .remove = fs_remove,
      .rename = fs_rename,
  };
  static const h2_pal_fs_api_t api = {
      .user = NULL,
      .vtable = &vtable,
  };

  if (fs == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  *fs = api;
  return mount_storage();
}

int h2_bk7258_board_fs_deinit(void) {
  int result = 0;

  if (s_storage_mounted) {
    result = bk_vfs_umount(H2_ZERO_BK_QSPI_LFS_MOUNT);
    s_storage_mounted = 0;
  }
  return result == 0 ? H2_PAL_OK : H2_PAL_ERR_IO;
}
