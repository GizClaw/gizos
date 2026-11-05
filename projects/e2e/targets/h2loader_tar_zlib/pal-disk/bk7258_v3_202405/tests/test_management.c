#ifdef NDEBUG
#undef NDEBUG
#endif
#include "sdk.h"
#include <assert.h>
#include <string.h>
#include "h2/pal/h2_pal_unsupported.h"
/* PSRAM placement is a native linker concern; Mach-O uses different sections.
 */
#define __attribute__(attributes)
#include "boards/bk7258_v3_202405/bk7258/ap/src/h2_bk7258_board_runtime.c"
#undef __attribute__
#include "projects/e2e/targets/h2loader_tar_zlib/pal-disk/bk7258_v3_202405/ap/management.c"

static int mounted, mounts, mount_failures, mkdir_error, removals;
static int runtime_calls, starts, confirms, runtime_error, start_error;
static const h2_pal_fs_api_t* active_fs;
FRESULT f_mount(FATFS* fs, const char* path, int immediate) {
  assert(strcmp(path, "1:") == 0);
  if (!fs) {
    mounted = 0;
    return FR_OK;
  }
  assert(immediate == 1);
  mounts++;
  if (mount_failures) {
    mount_failures--;
    return FR_NOT_READY;
  }
  mounted = 1;
  return FR_OK;
}
FRESULT f_mkdir(const char* path) {
  assert(mounted && strncmp(path, "1:/h2loader", 11) == 0);
  return mkdir_error ? FR_DENIED : FR_EXIST;
}
FRESULT f_unlink(const char* path) {
  if (!mounted) return FR_NOT_READY;
  assert(strncmp(path, "1:/h2loader/dl/", 14) == 0);
  removals++;
  return FR_NO_FILE;
}
int disk_uninitialize(int n) {
  assert(n == 1);
  return 0;
}
int bk_sd_card_deinit(void) { return 0; }
int bk_sdio_host_driver_deinit(void) { return 0; }
int bk_sdio_host_driver_init(void) { return 0; }
int gpio_sdio_sel(int n) {
  (void)n;
  return 0;
}
int gpio_sdio_one_line_sel(int n) {
  (void)n;
  return 0;
}
void rtos_delay_milliseconds(uint32_t n) { (void)n; }
int os_printf(const char* format, ...) {
  (void)format;
  return 0;
}
FRESULT f_open(FIL* f, const char* p, BYTE m) {
  (void)f;
  (void)p;
  (void)m;
  return FR_NO_FILE;
}
FRESULT f_read(FIL* f, void* p, UINT n, UINT* out) {
  (void)f;
  (void)p;
  (void)n;
  *out = 0;
  return FR_OK;
}
FRESULT f_write(FIL* f, const void* p, UINT n, UINT* out) {
  (void)f;
  (void)p;
  *out = n;
  return FR_OK;
}
FRESULT f_lseek(FIL* f, FSIZE_t p) {
  f->pos = p;
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
  (void)i;
  return FR_NO_FILE;
}
FRESULT f_rename(const char* p, const char* q) {
  (void)p;
  (void)q;
  return FR_OK;
}
FRESULT f_opendir(DIR* d, const char* p) {
  (void)d;
  (void)p;
  return FR_NO_PATH;
}
FRESULT f_readdir(DIR* d, FILINFO* i) {
  (void)d;
  i->fname[0] = 0;
  return FR_OK;
}
FRESULT f_closedir(DIR* d) {
  (void)d;
  return FR_OK;
}

h2_pal_result_t h2_runtime_init(const h2_runtime_config_t* config,
                                h2_runtime_t** out) {
  runtime_calls++;
  assert(mounted);
  active_fs = config->fs;
  *out = (h2_runtime_t*)&runtime_calls;
  return runtime_error;
}
int h2_bk_h2loader_start_app_iostreamikcp_with_capabilities(
    h2_runtime_t* r, const char* name, uint32_t capabilities) {
  assert(r && strcmp(name, "pal-disk") == 0 &&
         capabilities == H2_LOADER_CAPABILITY_UART);
  starts++;
  return start_error;
}
int h2_bk_h2loader_confirm_current_app(h2_runtime_t* r) {
  assert(r && starts);
  confirms++;
  /* Same cleanup boundary used by finish_stage: absent staging files are OK,
   * an unmounted filesystem is not. Execute the actual board remove callback.
   */
  int rc = active_fs->vtable->remove(active_fs->user, "/dl/stage.tar");
  return rc == H2_PAL_ERR_NOT_FOUND ? H2_PAL_OK : rc;
}
#define PROVIDER(type, name) \
  const type* name(void) { return NULL; }
PROVIDER(h2_pal_firmware_info_api_t, h2_bk_platform_firmware_info_api)
PROVIDER(h2_pal_log_api_t, h2_bk_platform_log_api)
PROVIDER(h2_pal_time_api_t, h2_bk_platform_time_api)
PROVIDER(h2_pal_timer_api_t, h2_bk_platform_timer_api)
PROVIDER(h2_pal_task_api_t, h2_bk_platform_task_api)
PROVIDER(h2_pal_queue_api_t, h2_bk_platform_queue_api)
PROVIDER(h2_pal_sync_api_t, h2_bk_platform_sync_api)
PROVIDER(h2_pal_pref_api_t, h2_bk_platform_pref_api)
PROVIDER(h2_pal_power_api_t, h2_bk_h2loader_power_api)
h2_pal_mem_api_t* h2_bk_platform_psram_allocator(void) { return NULL; }
static void reset(void) {
  release_sd_storage();
  mounts = removals = mount_failures = mkdir_error = 0;
  runtime_calls = starts = confirms = runtime_error = start_error = 0;
}
int main(void) {
  reset();
  assert(h2_bk_pal_disk_management_start() == H2_PAL_OK);
  assert(mounts == 1 && runtime_calls == 1 && starts == 1 && confirms == 1 &&
         removals == 1);
  h2_pal_fs_api_t fs = {0};
  assert(h2_bk7258_board_fs_init(&fs) == 0 && mounts == 1);
  assert(h2_bk7258_board_fs_init(NULL) == H2_PAL_ERR_INVALID_ARG);
  reset();
  mount_failures = 2;
  assert(h2_bk_pal_disk_management_start() == 0 && mounts == 3 &&
         confirms == 1);
  reset();
  mount_failures = 3;
  assert(h2_bk_pal_disk_management_start() == H2_PAL_ERR_IO);
  assert(mounts == 3 && !runtime_calls && !starts && !confirms);
  assert(h2_bk_pal_disk_management_start() == 0 && confirms == 1);
  reset();
  mkdir_error = 1;
  assert(h2_bk_pal_disk_management_start() == H2_PAL_ERR_IO);
  assert(!mounted && !runtime_calls && !confirms);
  reset();
  runtime_error = H2_PAL_ERR_NO_MEMORY;
  assert(h2_bk_pal_disk_management_start() == runtime_error && !starts &&
         !confirms);
  reset();
  start_error = H2_PAL_ERR_IO;
  assert(h2_bk_pal_disk_management_start() == start_error && starts == 1 &&
         !confirms);
  return 0;
}
