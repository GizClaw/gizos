#include "h2_esp_extflash_clear_test_sdk.h"
#include <assert.h>
#include <string.h>
#undef stat
static int s_dir, s_reads, s_closes, s_mode;
static struct dirent s_entry;
int h2_esp_board_extflash_clear_data(void);
int h2_esp_board_extflash_mount_data(void) { return H2_PAL_OK; }
int test_ensure_directory(const char* p) {
  (void)p;
  return H2_PAL_OK;
}
int test_log_filesystem_usage(const char* p) {
  (void)p;
  return H2_PAL_OK;
}
DIR* test_opendir(const char* p) {
  assert(strcmp(p, "/fake/data") == 0);
  return (DIR*)&s_dir;
}
struct dirent* test_readdir(DIR* dir) {
  assert(dir == (DIR*)&s_dir);
  assert(errno == 0);
  ++s_reads;
  if (s_mode == 0) {
    errno = EIO;
    return NULL;
  }
  if (s_reads == 1) {
    strcpy(s_entry.d_name, "file");
    errno = EIO;
    return &s_entry;
  }
  return NULL;
}
int test_closedir(DIR* dir) {
  assert(dir == (DIR*)&s_dir);
  ++s_closes;
  return s_mode == 2 ? -1 : 0;
}
int test_stat(const char* path, struct stat* result) {
  (void)path;
  memset(result, 0, sizeof(*result));
  result->st_mode = S_IFREG;
  return 0;
}
int test_unlink(const char* path) {
  assert(strcmp(path, "/fake/data/file") == 0);
  return 0;
}
int test_rmdir(const char* path) {
  (void)path;
  return 0;
}
int main(void) {
  assert(h2_esp_board_extflash_clear_data() == H2_PAL_ERR_IO);
  assert(s_reads == 1 && s_closes == 1);
  s_mode = 1;
  s_reads = s_closes = 0;
  assert(h2_esp_board_extflash_clear_data() == H2_PAL_OK);
  assert(s_reads == 2 && s_closes == 1);
  s_mode = 2;
  s_reads = s_closes = 0;
  assert(h2_esp_board_extflash_clear_data() == H2_PAL_ERR_IO);
  assert(s_closes == 1);
  return 0;
}
