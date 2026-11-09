#include "h2_bk_platform_storage.h"
#include "h2_bk_sd_fs_test_sdk.h"
#include <assert.h>
#include <string.h>
static unsigned attempts, power_off_waits, power_on_waits, unmounts,
    uninitializes;
static unsigned failures;
static FATFS* work_area;
static char last_path[192];
int gpio_sdio_sel(int x) {
  (void)x;
  return 0;
}
int gpio_sdio_one_line_sel(int x) {
  (void)x;
  return 0;
}
int disk_uninitialize(int x) {
  (void)x;
  ++uninitializes;
  return 0;
}
int bk_sd_card_deinit(void) { return 0; }
int bk_sdio_host_driver_deinit(void) { return 0; }
int bk_sdio_host_driver_init(void) { return 0; }
void rtos_delay_milliseconds(uint32_t ms) {
  if (ms == 1000)
    ++power_off_waits;
  else {
    assert(ms == 2000);
    ++power_on_waits;
  }
}
FRESULT f_mount(FATFS* fs, const char* drive, int mode) {
  assert(!strcmp(drive, "1:"));
  if (!fs) {
    assert(mode == 0);
    ++unmounts;
    return FR_OK;
  }
  assert(mode == 1);
  if (work_area) assert(work_area == fs);
  work_area = fs;
  ++attempts;
  return attempts <= failures ? FR_DISK_ERR : FR_OK;
}
FRESULT f_mkdir(const char* p) {
  (void)p;
  return FR_EXIST;
}
FRESULT f_open(FIL* f, const char* p, BYTE mode) {
  assert(f && mode == FA_READ);
  strcpy(last_path, p);
  return FR_OK;
}
FRESULT f_read(FIL* f, void* p, UINT n, UINT* count) {
  (void)f;
  memset(p, 7, n);
  *count = n;
  return FR_OK;
}
FRESULT f_write(FIL* f, const void* p, UINT n, UINT* count) {
  (void)f;
  (void)p;
  *count = n;
  return FR_OK;
}
FRESULT f_sync(FIL* f) {
  (void)f;
  return FR_OK;
}
FRESULT f_close(FIL* f) {
  (void)f;
  return FR_OK;
}
FRESULT f_stat(const char* p, FILINFO* i) {
  (void)p;
  memset(i, 0, sizeof(*i));
  return FR_OK;
}
FRESULT f_unlink(const char* p) {
  (void)p;
  return FR_OK;
}
FRESULT f_rename(const char* p, const char* q) {
  (void)p;
  (void)q;
  return FR_OK;
}
FRESULT f_opendir(DIR* d, const char* p) {
  (void)d;
  strcpy(last_path, p);
  return FR_OK;
}
FRESULT f_readdir(DIR* d, FILINFO* i) {
  (void)d;
  i->fname[0] = '\0';
  return FR_OK;
}
FRESULT f_closedir(DIR* d) {
  (void)d;
  return FR_OK;
}
int main(void) {
  const h2_bk_sd_mount_t mounts[] = {{"/dl", "1:/h2loader/dl"},
                                     {"/data", "1:/h2loader/data"}};
  const h2_bk_sd_fs_config_t config = {
      .drive = "1:", .root = "1:/h2loader", .mounts = mounts, .mount_count = 2};
  h2_pal_fs_api_t fs;
  assert(h2_bk_platform_sd_fs_init(&config, &fs) == 0);
  assert(h2_bk_platform_sd_fs_mount(NULL, "/other") == H2_PAL_ERR_INVALID_ARG &&
         !attempts);
  failures = 2;
  assert(h2_pal_fs_prepare(&fs, "/dl") == 0 && attempts == 3 &&
         power_off_waits == 3 && power_on_waits == 3 && unmounts == 3 &&
         uninitializes == 3);
  assert(h2_bk_platform_sd_fs_mount(NULL, "/data") == 0 && attempts == 3);
  h2_pal_fs_file_t* file;
  assert(h2_pal_fs_open(&fs, "/dl/file", H2_PAL_FS_OPEN_READ, &file) == 0 &&
         !strcmp(last_path, "1:/h2loader/dl/file"));
  uint8_t value;
  size_t count;
  assert(h2_pal_fs_read(&fs, file, &value, 1, &count) == 0 && count == 1 &&
         value == 7);
  assert(h2_pal_fs_close(&fs, file) == 0);
  assert(h2_pal_fs_clear(&fs, "/data") == 0 &&
         !strcmp(last_path, "1:/h2loader/data"));
  h2_bk_platform_sd_fs_release();
  attempts = 0;
  failures = 5;
  assert(h2_bk_platform_sd_fs_mount(NULL, "/dl") == H2_PAL_ERR_IO &&
         attempts == 5);
  return 0;
}
