#ifndef H2_ZERO_BK_FS_TEST_SDK_H
#define H2_ZERO_BK_FS_TEST_SDK_H
#include "os/h2_pal_fs.h"
#include <dirent.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#define H2_BK7258_DL_MOUNT_PATH "/dl"
#define H2_BK7258_DATA_MOUNT_PATH "/data"
#define H2_ZERO_BK_EXTFLASH_SIZE_BYTES (16u * 1024u * 1024u)
#define VFS_QSPI_FLASH_0_PATITION_0 "/qspi"
enum {
  LFS_QSPI_FLASH,
  LFS_ERR_OK = 0,
  LFS_ERR_NOENT = -2,
  LFS_ERR_NOATTR = -61,
  LFS_ERR_NOMEM = -12,
  LFS_ERR_NOSPC = -28,
  LFS_ERR_FBIG = -27,
  LFS_ERR_INVAL = -22,
  LFS_ERR_EXIST = -17
};
struct bk_little_fs_partition {
  int part_type;
  const char* mount_path;
  struct {
    unsigned start_addr, size;
  } part_flash;
};
int bk_vfs_mount(const char*, const char*, const char*, unsigned, void*);
int bk_vfs_umount(const char*);
int bk_vfs_mkdir(const char*, unsigned);
h2_pal_result_t h2_zero_bk_peripheral_power_enable(void);
void rtos_delay_milliseconds(unsigned);
int h2_bk7258_board_fs_init(h2_pal_fs_api_t*);
int h2_bk7258_board_fs_deinit(void);
#define os_printf(...) ((void)0)
#define os_malloc malloc
#define os_free free
#define bk_vfs_stat stat
#define bk_vfs_open open
#define bk_vfs_read read
#define bk_vfs_write write
#define bk_vfs_lseek lseek
#define bk_vfs_fsync fsync
#define bk_vfs_close close
#define bk_vfs_unlink unlink
#define bk_vfs_rename rename
#define bk_vfs_opendir opendir
#define bk_vfs_readdir readdir
#define bk_vfs_closedir closedir
#define bk_vfs_rmdir rmdir
#endif
