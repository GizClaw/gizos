#define _DARWIN_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "h2_bk7258_board.h"
#include "h2_h202_board_config.h"
#include "h2_h202_board_test_sdk.h"
#include <assert.h>
#include <fcntl.h>
#include <string.h>
static int mount_error, unmount_error, mount_count, unmount_count;
static unsigned volume_size = H2_H202_EXTFLASH_SIZE_BYTES;
static unsigned last_mount_size;
static int read_dir_error, remove_dir_error, mkdir_error;
struct dirent* h2_h202_test_readdir(DIR* dir) {
  return read_dir_error ? NULL : readdir(dir);
}
int h2_h202_test_rmdir(const char* path) {
  return remove_dir_error ? LFS_ERR_INVAL : (rmdir(path) == 0 ? 0 : -errno);
}
int h2_h202_test_open(const char* path, int flags) {
  return open(path, flags, 0600);
}
int h2_bk7258_board_init(void) {
  assert(!"filesystem operations must not reconfigure peripheral rails");
  return H2_PAL_ERR_IO;
}
int bk_vfs_mount(const char* source, const char* path, const char* type,
                 unsigned flags, void* data) {
  (void)source;
  (void)path;
  (void)flags;
  struct bk_little_fs_partition* p = data;
  assert(strcmp(type, "littlefs") == 0);
  assert(p->part_flash.start_addr == 0);
  assert(p->part_flash.size == H2_H202_EXTFLASH_SIZE_BYTES);
  last_mount_size = p->part_flash.size;
  ++mount_count;
  return mount_error ? mount_error : (last_mount_size == volume_size ? 0 : -1);
}
int bk_vfs_umount(const char* path) {
  (void)path;
  ++unmount_count;
  return unmount_error;
}
int bk_vfs_mkdir(const char* path, unsigned mode) {
  return mkdir_error ? LFS_ERR_INVAL
                     : (mkdir(path, (mode_t)mode) == 0 ? 0 : -errno);
}
int bk_vfs_stat(const char* path, struct stat* info) {
  return stat(path, info) == 0 ? 0 : -errno;
}
int main(void) {
  char temporary[] = "/tmp/h202-fs-XXXXXX";
  char* directory = mkdtemp(temporary);
  assert(directory && chdir(directory) == 0);
  h2_pal_fs_api_t fs;
  assert(h2_bk7258_board_fs_init(NULL) == H2_PAL_ERR_INVALID_ARG);
  assert(h2_bk7258_board_fs_init(&fs) == 0 && mount_count == 0);
  /* Progress restore starts with stat on a fresh boot, before any open/mkdir.
   */
  h2_pal_fs_stat_t info = {.size = 99, .is_dir = true};
  mount_error = -1;
  assert(h2_pal_fs_stat(&fs, "/data/progress", &info) == H2_PAL_ERR_IO);
  assert(info.size == 0 && !info.is_dir);
  assert(mount_count == 1); /* No fallback, format or success cache. */
  mount_error = 0;
  assert(h2_pal_fs_stat(&fs, "/data/progress", &info) == H2_PAL_ERR_NOT_FOUND);
  assert(h2_pal_fs_mkdir(&fs, "/dl") == 0);
  assert(last_mount_size == H2_H202_EXTFLASH_SIZE_BYTES);
  int mounted = mount_count;
  assert(h2_pal_fs_mkdir(&fs, "/data") == 0 && mount_count == mounted);
  /* An existing regular file must not masquerade as a mounted directory. */
  assert(rmdir("h2loader/data") == 0);
  int conflict = open("h2loader/data", O_CREAT | O_WRONLY, 0600);
  assert(conflict >= 0 && close(conflict) == 0);
  assert(h2_pal_fs_mkdir(&fs, "/data") == H2_PAL_ERR_IO);
  assert(unlink("h2loader/data") == 0);
  assert(h2_pal_fs_mkdir(&fs, "/data") == 0);

  h2_pal_fs_file_t* f = NULL;
  assert(h2_pal_fs_open(&fs, "/data/../outside", H2_PAL_FS_OPEN_WRITE_TRUNCATE,
                        &f) == H2_PAL_ERR_INVALID_ARG);
  assert(h2_pal_fs_open(&fs, "/dlx/escape", H2_PAL_FS_OPEN_READ, &f) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(h2_pal_fs_open(&fs, "/dl/package", H2_PAL_FS_OPEN_WRITE_TRUNCATE,
                        &f) == 0);
  size_t count = 99;
  assert(h2_pal_fs_write(&fs, f, "package", 7, &count) == 0 && count == 7);
  assert(h2_pal_fs_sync(&fs, f) == 0 && h2_pal_fs_close(&fs, f) == 0);
  assert(h2_pal_fs_rename(&fs, "/dl/package", "/dl/staged") == 0);
  assert(h2_pal_fs_open(&fs, "/dl/staged", H2_PAL_FS_OPEN_READ, &f) == 0);
  char bytes[8] = {0};
  assert(h2_pal_fs_read(&fs, f, bytes, 7, &count) == 0 && count == 7 &&
         strcmp(bytes, "package") == 0);
  assert(h2_pal_fs_seek(&fs, f, 7) == 0);
  assert(h2_pal_fs_read(&fs, f, bytes, 1, &count) == 0 && count == 0);
  assert(h2_pal_fs_close(&fs, f) == 0);
  read_dir_error = 1;
  assert(h2_pal_fs_clear(&fs, "/dl") != H2_PAL_OK);
  assert(access("h2loader/dl/staged", F_OK) == 0);
  read_dir_error = 0;
  remove_dir_error = 1;
  assert(h2_pal_fs_clear(&fs, "/dl") == H2_PAL_ERR_INVALID_ARG);
  remove_dir_error = 0;
  assert(mkdir("h2loader/dl/nested", 0700) == 0);
  int nested = open("h2loader/dl/nested/child", O_CREAT | O_WRONLY, 0600);
  assert(nested >= 0 && close(nested) == 0);
  assert(h2_pal_fs_clear(&fs, "/dl") == 0);
  assert(access("h2loader/dl/nested", F_OK) != 0);
  struct stat cleared;
  assert(stat("h2loader/dl", &cleared) == 0 && S_ISDIR(cleared.st_mode));
  mkdir_error = 1;
  assert(h2_pal_fs_clear(&fs, "/dl") == H2_PAL_ERR_INVALID_ARG);
  mkdir_error = 0;
  assert(h2_pal_fs_mkdir(&fs, "/dl") == H2_PAL_OK);
  assert(h2_pal_fs_open(&fs, "/dl/staged", H2_PAL_FS_OPEN_READ, &f) ==
             H2_PAL_ERR_NOT_FOUND &&
         !f);
  unmount_error = -1;
  assert(h2_bk7258_board_fs_deinit() == H2_PAL_ERR_IO);
  assert(h2_pal_fs_mkdir(&fs, "/dl") == 0 && mount_count == mounted + 1);
  unmount_error = 0;
  assert(h2_bk7258_board_fs_deinit() == 0);
  assert(h2_bk7258_board_fs_deinit() == 0 && unmount_count == 2);
  /* An incompatible existing volume fails without modifying its contents. */
  int retained = open("h2loader/data/keep", O_CREAT | O_WRONLY, 0600);
  assert(retained >= 0 && write(retained, "keep", 4) == 4 &&
         close(retained) == 0);
  volume_size = 4u * 1024u * 1024u;
  mounted = mount_count;
  assert(h2_pal_fs_mkdir(&fs, "/dl") == H2_PAL_ERR_IO &&
         mount_count == mounted + 1);
  assert(last_mount_size == H2_H202_EXTFLASH_SIZE_BYTES);
  retained = open("h2loader/data/keep", O_RDONLY);
  assert(retained >= 0 && read(retained, bytes, 4) == 4 &&
         memcmp(bytes, "keep", 4) == 0 && close(retained) == 0);
  /* A later cold stat mounts and sees retained progress before opening it. */
  volume_size = H2_H202_EXTFLASH_SIZE_BYTES;
  assert(h2_pal_fs_stat(&fs, "/data/keep", &info) == H2_PAL_OK);
  assert(info.size == 4 && !info.is_dir && mount_count == mounted + 2);
  assert(h2_pal_fs_open(&fs, "/data/keep", H2_PAL_FS_OPEN_READ, &f) == 0);
  assert(h2_pal_fs_read(&fs, f, bytes, 4, &count) == 0 && count == 4 &&
         memcmp(bytes, "keep", 4) == 0);
  assert(h2_pal_fs_close(&fs, f) == 0);
  assert(h2_bk7258_board_fs_deinit() == 0);
  assert(unlink("h2loader/data/keep") == 0);
  assert(rmdir("h2loader/dl") == 0 && rmdir("h2loader/data") == 0 &&
         rmdir("h2loader") == 0);
  assert(chdir("/") == 0 && rmdir(directory) == 0);
  return 0;
}
