#include "h2_bk_platform_storage.h"
#include "h2_bk_sd_fs_test_sdk.h"
#include <assert.h>
#include <string.h>
static unsigned attempts, power_off_waits, power_on_waits, unmounts,
    uninitializes;
static unsigned failures, formats, selection_calls, mkdir_calls,
    remove_on_mount;
static FRESULT mount_failure = FR_DISK_ERR, format_result = FR_OK;
static bool inserted = true;
static bool card_present(void) { return inserted; }
static int select_pins(void) {
  ++selection_calls;
  return H2_PAL_OK;
}
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
  if (attempts == remove_on_mount) inserted = false;
  return attempts <= failures ? mount_failure : FR_OK;
}
FRESULT f_mkfs(const char* drive, BYTE mode, uint32_t unit, void* work,
               UINT len) {
  assert(!strcmp(drive, "1:") && mode == FM_ANY && unit == 0 && work != NULL &&
         len == FF_MAX_SS && inserted);
  ++formats;
  return format_result;
}
FRESULT f_mkdir(const char* p) {
  (void)p;
  ++mkdir_calls;
  return FR_EXIST;
}
FRESULT f_open(FIL* f, const char* p, BYTE mode) {
  assert(f && mode == FA_READ);
  strcpy(last_path, p);
  f->marker = 0;
  f->size = 50;
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
FRESULT f_lseek(FIL* f, FSIZE_t position) {
  f->marker = position;
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
  assert(h2_pal_fs_seek(&fs, file, 20) == H2_PAL_OK);
  assert(h2_pal_fs_seek(&fs, file, 51) == H2_PAL_ERR_INVALID_ARG);
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
  h2_bk_sd_fs_config_t factory = config;
  factory.card_present = card_present;
  factory.select_pins = select_pins;
  assert(h2_bk_platform_sd_fs_init(&factory, &fs) == H2_PAL_OK);
  attempts = 0;
  inserted = false;
  assert(h2_bk_platform_sd_fs_factory_mount("/dl") == H2_PAL_ERR_UNAVAILABLE);
  assert(attempts == 0 && selection_calls == 0 && formats == 0);
  inserted = true;
  failures = 0;
  assert(h2_bk_platform_sd_fs_factory_mount("/dl") == H2_PAL_OK);
  assert(attempts == 1 && selection_calls == 1 && formats == 0);
  h2_bk_platform_sd_fs_release();
  attempts = 0;
  failures = 1;
  mount_failure = FR_NO_FILESYSTEM;
  assert(h2_bk_platform_sd_fs_factory_mount("/dl") == H2_PAL_OK);
  assert(attempts == 2 && formats == 1);
  h2_bk_platform_sd_fs_release();
  attempts = 0;
  remove_on_mount = 1;
  assert(h2_bk_platform_sd_fs_factory_mount("/dl") == H2_PAL_ERR_IO);
  assert(attempts == 1 && formats == 1);
  remove_on_mount = 0;
  inserted = true;
  attempts = 0;
  format_result = FR_DISK_ERR;
  assert(h2_bk_platform_sd_fs_factory_mount("/dl") == H2_PAL_ERR_IO);
  assert(attempts == 1 && formats == 2);
  attempts = 0;
  format_result = FR_OK;
  failures = 2;
  assert(h2_bk_platform_sd_fs_factory_mount("/dl") == H2_PAL_ERR_IO);
  assert(attempts == 2 && formats == 3);
  attempts = 0;
  failures = 5;
  assert(h2_pal_fs_prepare(&fs, "/dl") == H2_PAL_ERR_IO);
  assert(attempts == 5 && formats == 3);
  return 0;
}
