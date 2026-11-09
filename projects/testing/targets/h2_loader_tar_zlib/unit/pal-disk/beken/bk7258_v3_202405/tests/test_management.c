#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <string.h>
#include "h2_pal_unsupported.h"
#define H2_BK7258_BOARD_RUNTIME_HOST_TEST 1
#include "boards/beken/bk7258_v3_202405/bk7258/ap/src/h2_bk7258_board_runtime.c"
#include "projects/testing/targets/h2_loader_tar_zlib/unit/pal-disk/beken/bk7258_v3_202405/ap/management.c"

static int mounted, mounts, mount_failures, mkdir_error, removals;
static int dl_ready, data_ready, boot_calls, boot_error;
static int runtime_calls, starts, confirms, runtime_error, start_error;
static const h2_pal_fs_api_t* active_fs;
FRESULT f_mount(FATFS* fs, const char* path, int immediate) {
  assert(strcmp(path, "1:") == 0);
  if (!fs) {
    mounted = dl_ready = data_ready = 0;
    return FR_OK;
  }
  assert(immediate == 1);
  mounts++;
  if (mount_failures) {
    mount_failures--;
    return FR_DISK_ERR;
  }
  mounted = 1;
  return FR_OK;
}
FRESULT f_mkdir(const char* path) {
  assert(mounted && strncmp(path, "1:/h2loader", 11) == 0);
  if (mkdir_error == 1 ||
      (mkdir_error == 2 && strcmp(path, H2_BK7258_SD_DATA_ROOT) == 0))
    return FR_DENIED;
  if (strcmp(path, H2_BK7258_SD_DL_ROOT) == 0) dl_ready = 1;
  if (strcmp(path, H2_BK7258_SD_DATA_ROOT) == 0) data_ready = 1;
  return FR_EXIST;
}
FRESULT f_unlink(const char* path) {
  if (!mounted) return FR_DISK_ERR;
  assert(strncmp(path, "1:/h2loader/dl/", 14) == 0);
  removals++;
  return FR_NO_FILE;
}
int disk_uninitialize(int n) {
  assert(n == DISK_NUMBER_SDIO_SD);
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
  f->marker = (int)p;
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

int h2_bk_platform_boot_startup(void) {
  assert(mounted && dl_ready && data_ready);
  boot_calls++;
  return boot_error;
}
h2_pal_result_t h2_runtime_init(const h2_runtime_config_t* config,
                                h2_runtime_t** out) {
  runtime_calls++;
  assert(mounted && dl_ready && data_ready && boot_calls == 1);
  assert(config->nfc == h2_pal_unsupported_nfc_api());
  assert(config->nfc_card_emulation ==
         h2_pal_unsupported_nfc_card_emulation_api());
  assert(config->display == h2_pal_unsupported_display_api());
  assert(config->audio == h2_pal_unsupported_audio_api());
  assert(config->modem == h2_pal_unsupported_modem_api());
  assert(config->input == h2_pal_unsupported_input_api());
  active_fs = config->fs;
  *out = runtime_error ? NULL : (h2_runtime_t*)&runtime_calls;
  return runtime_error;
}
int h2_device_app_commands_start_serial_named(h2_device_app_commands_t* state,
                                              h2_runtime_t* r, const char* name,
                                              uint32_t capabilities) {
  assert(r && state == &management && strcmp(name, "pal-disk") == 0 &&
         capabilities == H2_LOADER_CAPABILITY_UART);
  assert(mounted && dl_ready && data_ready);
  starts++;
  return start_error;
}
int h2_device_app_confirm(h2_device_app_commands_t* state, h2_runtime_t* r) {
  assert(state == &management && r && starts);
  confirms++;
  /* Same cleanup boundary used by finish_stage: absent staging files are OK,
   * an unmounted filesystem is not. Execute the actual board remove callback.
   */
  int rc = active_fs->vtable->remove(active_fs->user, "/dl/stage.tar");
  return rc == H2_PAL_ERR_NOT_FOUND ? H2_PAL_OK : rc;
}
#define PROVIDER(type, name, capability) \
  const type* name(void) { return h2_pal_unsupported_##capability##_api(); }
PROVIDER(h2_pal_firmware_info_api_t, h2_bk_platform_firmware_info_api,
         firmware_info)
PROVIDER(h2_pal_device_identity_api_t, h2_bk_platform_device_identity_api,
         device_identity)
PROVIDER(h2_pal_image_api_t, h2_bk_platform_image_api, image)
PROVIDER(h2_pal_io_stream_api_t, h2_bk_platform_io_stream_api, io_stream)
PROVIDER(h2_pal_log_api_t, h2_bk_platform_log_api, log)
PROVIDER(h2_pal_time_api_t, h2_bk_platform_time_api, time)
PROVIDER(h2_pal_timer_api_t, h2_bk_platform_timer_api, timer)
PROVIDER(h2_pal_task_api_t, h2_bk_platform_task_api, task)
PROVIDER(h2_pal_queue_api_t, h2_bk_platform_queue_api, queue)
PROVIDER(h2_pal_sync_api_t, h2_bk_platform_sync_api, sync)
PROVIDER(h2_pal_pref_api_t, h2_bk_platform_pref_api, pref)
PROVIDER(h2_pal_power_api_t, h2_bk_platform_power_api, power)
#undef PROVIDER
h2_pal_mem_api_t* h2_bk_platform_psram_allocator(void) {
  static h2_pal_mem_api_t api;
  api = *h2_pal_unsupported_mem_api();
  return &api;
}
static void reset(void) {
  release_sd_storage();
  mounts = removals = mount_failures = mkdir_error = 0;
  boot_calls = boot_error = 0;
  runtime_calls = starts = confirms = runtime_error = start_error = 0;
  runtime = NULL;
  active_fs = NULL;
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
  assert(mounts == 3 && !boot_calls && !runtime_calls && !starts && !confirms);
  assert(h2_bk_pal_disk_management_start() == 0 && confirms == 1);
  reset();
  mkdir_error = 1;
  assert(h2_bk_pal_disk_management_start() == H2_PAL_ERR_IO);
  assert(!mounted && !boot_calls && !runtime_calls && !confirms);
  reset();
  mkdir_error = 2;
  assert(h2_bk_pal_disk_management_start() == H2_PAL_ERR_IO);
  assert(dl_ready && !data_ready && !boot_calls && !runtime_calls && !confirms);
  reset();
  boot_error = H2_PAL_ERR_IO;
  assert(h2_bk_pal_disk_management_start() == boot_error);
  assert(!runtime_calls && !starts && !confirms);
  reset();
  runtime_error = H2_PAL_ERR_NO_MEMORY;
  assert(h2_bk_pal_disk_management_start() == runtime_error && !starts &&
         !confirms);
  reset();
  start_error = H2_PAL_ERR_IO;
  assert(h2_bk_pal_disk_management_start() == start_error && starts == 1 &&
         !confirms);
  reset();
  return 0;
}
