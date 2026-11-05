#include "h2_zero_bk_fs_test_sdk.h"
#include <assert.h>
#include <string.h>
static unsigned s_mounts, s_unmounts;
static int s_unmount_result;
int bk_vfs_mount(const char* source, const char* path, const char* type,
                 unsigned flags, void* partition) {
  (void)source;
  (void)flags;
  assert(strcmp(path, "/qspi") == 0 && strcmp(type, "littlefs") == 0);
  assert(partition != NULL);
  ++s_mounts;
  return 0;
}
int bk_vfs_umount(const char* path) {
  assert(strcmp(path, "/qspi") == 0);
  ++s_unmounts;
  return s_unmount_result;
}
int bk_vfs_mkdir(const char* path, unsigned mode) {
  (void)mode;
  assert(strncmp(path, "/qspi/", 6) == 0);
  return LFS_ERR_EXIST;
}
h2_pal_result_t h2_zero_bk_peripheral_power_enable(void) { return H2_PAL_OK; }
void rtos_delay_milliseconds(unsigned ms) { assert(ms == 10u); }
int main(void) {
  h2_pal_fs_api_t fs;
  assert(h2_bk7258_board_fs_deinit() == H2_PAL_OK);
  assert(s_unmounts == 0u);
  assert(h2_bk7258_board_fs_init(&fs) == H2_PAL_OK);
  assert(s_mounts == 1u);
  s_unmount_result = -1;
  assert(h2_bk7258_board_fs_deinit() == H2_PAL_ERR_IO);
  assert(h2_bk7258_board_fs_init(&fs) == H2_PAL_OK);
  assert(s_mounts == 1u);
  assert(h2_bk7258_board_fs_deinit() == H2_PAL_ERR_IO);
  assert(s_unmounts == 2u);
  s_unmount_result = 0;
  assert(h2_bk7258_board_fs_deinit() == H2_PAL_OK);
  assert(h2_bk7258_board_fs_deinit() == H2_PAL_OK);
  assert(s_unmounts == 3u);
  assert(h2_bk7258_board_fs_init(&fs) == H2_PAL_OK);
  assert(s_mounts == 2u);
  assert(h2_bk7258_board_fs_deinit() == H2_PAL_OK);
  return 0;
}
