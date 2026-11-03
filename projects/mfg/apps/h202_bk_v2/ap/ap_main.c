#include "h2_bk7258_board.h"
#include "h2_bk_h2loader.h"
#include "h2_bk_platform_core.h"
#include "h2_bk_target_task_policy.h"
#include "h2_h202_board_config.h"
#include "h2_h202_mfg.h"
#include "h2_loader_boot.h"
#include "h2_loader_command.h"
#include "h2_loader_package.h"

#include "bk_private/bk_init.h"
#include "driver/flash.h"
#include "driver/wdt.h"
#include "mbedtls/sha256.h"
#include "os/os.h"

#include <stdio.h>
#include <string.h>

#if CONFIG_BLUETOOTH || CONFIG_BT || CONFIG_BLE
#error "H202 MFG requires the UART-only Bluetooth-disabled layout"
#endif

#define H2_H202_MFG_STAGE_PATH "/dl/update.tar.zlib"
#define H2_H202_MFG_STAGE_PREV_PATH "/dl/update.tar.zlib.prev"
#define H2_H202_MFG_RETRY_DELAY_MS 10000u
#define H2_H202_MFG_FLASH_SECTOR_SIZE 4096u
#define H2_H202_MFG_COREDUMP_ADDR H2_BK_H2LOADER_COREDUMP_ADDR
#define H2_H202_MFG_COREDUMP_SIZE H2_BK_H2LOADER_COREDUMP_SIZE

extern const h2_h202_mfg_profile_t h2_h202_mfg_profile;

static h2_runtime_t* s_runtime;
static h2_loader_t s_loader;
static h2_loader_command_t s_command;
static h2_pal_mutex_t* s_operation;
static h2_h202_mfg_service_t* s_service;
static mbedtls_sha256_context s_sha;

static void hold(void) {
  for (;;) rtos_delay_milliseconds(1000);
}

static void fail(const char* stage, int rc) {
  printf("H202_MFG_BOOT_FAIL stage=%s rc=%d\r\n", stage, rc);
  hold();
}

static void on_event(void* user, h2_loader_startup_event_t event, int code) {
  (void)user;
  /* SDK force_feed also starts the AON watchdog without periodic servicing. */
  (void)bk_wdt_feed();
  printf("H2_BK_H2LOADER_EVENT event=%d code=%d\r\n", (int)event, code);
}

static void install_progress(void* user, const h2_bundle_entry_t* entry,
                             const h2_bundle_install_stats_t* stats) {
  (void)user;
  (void)bk_wdt_feed();
  if (entry == NULL || stats == NULL) return;
  printf("H2_BK_H2LOADER_INSTALL entry=%lu files=%lu payload=%llu path=%s\r\n",
         (unsigned long)stats->entry_count, (unsigned long)stats->file_count,
         (unsigned long long)stats->payload_bytes, entry->path);
}

static int mount_file_point(void* user, const char* path) {
  (void)user;
  return h2_pal_fs_mkdir(s_runtime->fs, path);
}

static int clear_data(void* user, const char* path) {
  (void)user;
  return h2_pal_fs_clear(s_runtime->fs, path);
}

static int digest_start(void* user) {
  (void)user;
  mbedtls_sha256_init(&s_sha);
  return mbedtls_sha256_starts(&s_sha, 0) == 0 ? H2_PAL_OK : H2_PAL_ERR_IO;
}

static int digest_update(void* user, const uint8_t* data, size_t len) {
  (void)user;
  if (data == NULL && len != 0u) return H2_PAL_ERR_INVALID_ARG;
  (void)bk_wdt_feed();
  return mbedtls_sha256_update(&s_sha, data, len) == 0 ? H2_PAL_OK
                                                       : H2_PAL_ERR_IO;
}

static int digest_finish(void* user, uint8_t out[32]) {
  (void)user;
  if (out == NULL) return H2_PAL_ERR_INVALID_ARG;
  return mbedtls_sha256_finish(&s_sha, out) == 0 ? H2_PAL_OK : H2_PAL_ERR_IO;
}

static void digest_abort(void* user) {
  (void)user;
  mbedtls_sha256_free(&s_sha);
}

static uint64_t now_ms(void* user) {
  (void)user;
  return (uint64_t)rtos_get_time();
}

static void sleep_ms(void* user, uint32_t delay_ms) {
  (void)user;
  rtos_delay_milliseconds(delay_ms);
}

static h2_pal_result_t coredump_get_partition(void* user, uint32_t id,
                                              h2_pal_disk_partition_t* out) {
  (void)user;
  if (id != H2_H202_MFG_COREDUMP_ADDR || out == NULL) {
    return H2_PAL_ERR_NOT_FOUND;
  }
  memset(out, 0, sizeof(*out));
  out->id = H2_H202_MFG_COREDUMP_ADDR;
  out->flags =
      H2_PAL_DISK_PARTITION_FLAG_READABLE | H2_PAL_DISK_PARTITION_FLAG_ERASABLE;
  out->size = H2_H202_MFG_COREDUMP_SIZE;
  out->erase_block_size = H2_H202_MFG_FLASH_SECTOR_SIZE;
  out->write_alignment = 1u;
  snprintf(out->name, sizeof(out->name), "coredump");
  return H2_PAL_OK;
}

static h2_pal_result_t coredump_read(void* user, uint32_t id, uint64_t offset,
                                     void* data, size_t len) {
  (void)user;
  if (id != H2_H202_MFG_COREDUMP_ADDR || data == NULL ||
      offset > H2_H202_MFG_COREDUMP_SIZE ||
      len > H2_H202_MFG_COREDUMP_SIZE - offset) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  return bk_flash_read_bytes(H2_H202_MFG_COREDUMP_ADDR + (uint32_t)offset, data,
                             (uint32_t)len) == 0
             ? H2_PAL_OK
             : H2_PAL_ERR_IO;
}

static h2_pal_result_t coredump_erase(void* user, uint32_t id, uint64_t offset,
                                      uint64_t len) {
  (void)user;
  if (id != H2_H202_MFG_COREDUMP_ADDR || offset > H2_H202_MFG_COREDUMP_SIZE ||
      len > H2_H202_MFG_COREDUMP_SIZE - offset ||
      (offset % H2_H202_MFG_FLASH_SECTOR_SIZE) != 0u ||
      (len % H2_H202_MFG_FLASH_SECTOR_SIZE) != 0u) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  flash_protect_type_t protect = bk_flash_get_protect_type();
  int rc = bk_flash_set_protect_type(FLASH_PROTECT_NONE);
  for (uint64_t off = offset; rc == 0 && off < offset + len;
       off += H2_H202_MFG_FLASH_SECTOR_SIZE) {
    rc = bk_flash_erase_sector(H2_H202_MFG_COREDUMP_ADDR + (uint32_t)off);
  }
  const int restore_rc = bk_flash_set_protect_type(protect);
  return rc == 0 && restore_rc == 0 ? H2_PAL_OK : H2_PAL_ERR_IO;
}

static h2_pal_result_t coredump_write(void* user, uint32_t id, uint64_t offset,
                                      const void* data, size_t len) {
  (void)user;
  (void)id;
  (void)offset;
  (void)data;
  (void)len;
  return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t coredump_flush(void* user, uint32_t id) {
  (void)user;
  (void)id;
  return H2_PAL_ERR_UNSUPPORTED;
}

static const h2_pal_disk_vtable_t s_coredump_vtable = {
    .get_partition = coredump_get_partition,
    .read = coredump_read,
    .erase = coredump_erase,
    .write = coredump_write,
    .flush = coredump_flush,
};

static const h2_pal_disk_api_t s_coredump = {.vtable = &s_coredump_vtable};

/* Loader Core boot, as the H202 Loader does it but UART-only like the H106
 * MFG: the Loader identity is hashed once, Core confirms the running image
 * and boots partition 2 only after all ten MFG results passed. A management
 * reboot ends MFG without saving, as before. */
static void start_loader(char device_uid[13]) {
  h2_pal_firmware_info_t firmware;
  h2_loader_config_t config = {
      .package = {.fs = s_runtime->fs,
                  .allocator = h2_bk_platform_psram_allocator(),
                  .digest = {.start = digest_start,
                             .update = digest_update,
                             .finish = digest_finish,
                             .abort = digest_abort},
                  .app_entry_path = H2_BK_H2LOADER_APP_ENTRY_PATH,
                  .app_writer = h2_bk_h2loader_ota_app_writer(),
                  .image_reader = h2_bk_h2loader_image_reader(),
                  .image_writer = h2_bk_h2loader_image_writer(),
                  .clear_data = clear_data},
      .pref = s_runtime->pref,
      .power = h2_bk_h2loader_power_api(),
      .board = H2_H202_BOARD_NAME,
      .target = "bk7258",
      .chip = "bk7258",
      .device_uid = device_uid,
      .h2loader_partition_id = H2_BK_H2LOADER_PRIMARY_PARTITION_ID,
      .app_partition_id = H2_BK_H2LOADER_APP_PARTITION_ID,
      .mfg_required_total = H2_H202_MFG_ITEM_COUNT,
      .hardware_capabilities =
          H2_LOADER_CAPABILITY_UART | H2_LOADER_CAPABILITY_WIFI,
      .active_identity = {.format = 1u,
                          .role = H2_LOADER_IMAGE_ROLE_H2LOADER,
                          .board = H2_H202_BOARD_NAME,
                          .target = "bk7258"},
      .confirm_active_image = h2_bk_h2loader_confirm_active_loader,
      .mount_file_point = mount_file_point,
      .on_event = on_event};
  static h2_loader_command_config_t commands;
  commands = (h2_loader_command_config_t){
      .loader = &s_loader,
      .fs = s_runtime->fs,
      .http = s_runtime->http,
      .wifi = s_runtime->wifi_sta,
      .wifi_settings = s_runtime->wifi_settings,
      .disk = &s_coredump,
      .digest = config.package.digest,
      .now_ms = now_ms,
      .sleep_ms = sleep_ms,
      .coredump_partition_id = H2_H202_MFG_COREDUMP_ADDR};
  const h2_pal_mutex_config_t mutex = {.name = "h2loader-operation",
                                       .allocator = s_runtime->mem,
                                       .flags = H2_PAL_MUTEX_FLAG_RECURSIVE};
  int rc = h2_pal_mutex_create(s_runtime->sync, &mutex, &s_operation);
  if (rc != H2_PAL_OK) fail("operation_mutex", rc);
  commands.operation_sync = s_runtime->sync;
  commands.operation_mutex = s_operation;
  /* Management Wi-Fi scan/connect/disconnect lock only this one; share the
   * recursive operation mutex so they never interleave with the MFG Wi-Fi
   * qualification, which holds it. */
  commands.wifi_operation_sync = s_runtime->sync;
  commands.wifi_operation_mutex = s_operation;
  rc = h2_pal_mutex_lock(s_runtime->sync, s_operation);
  if (rc != H2_PAL_OK) fail("initialization_lock", rc);
  rc = h2_pal_firmware_info_get_current(s_runtime->firmware_info, &firmware);
  if (rc == H2_PAL_OK) {
    rc = h2_loader_read_current_loader_identity(&config, firmware.version,
                                                &config.active_identity);
  }
  if (rc != H2_PAL_OK) fail("active_identity", rc);
  rc = h2_loader_init(&s_loader, &config);
  if (rc != H2_PAL_OK) fail("loader_init", rc);
  h2_bundle_installer_set_progress(&s_loader.package.installer,
                                   install_progress, NULL);
  rc = h2_bk_h2loader_start_loader_iostreamikcp(s_runtime, &s_command,
                                                &commands);
  if (rc != H2_PAL_OK) fail("iostreamikcp", rc);
  rc = h2_pal_mutex_unlock(s_runtime->sync, s_operation);
  if (rc != H2_PAL_OK) fail("initialization_unlock", rc);
}

/* Retries mount and publish recovery like the Loader; returns once Core
 * settled in command mode. A successful reboot never returns. */
static void run_startup(void) {
  for (;;) {
    h2_loader_startup_action_t action = H2_LOADER_STARTUP_ACTION_COMMAND_MODE;
    int rc = h2_pal_mutex_lock(s_runtime->sync, s_operation);
    if (rc != H2_PAL_OK) fail("startup_lock", rc);
    rc = mount_file_point(NULL, H2_BK7258_DL_MOUNT_PATH);
    if (rc == H2_PAL_OK) rc = mount_file_point(NULL, H2_BK7258_DATA_MOUNT_PATH);
    if (rc == H2_PAL_OK) {
      rc = h2_loader_package_recover_publish(
          s_runtime->fs, s_runtime->pref, s_runtime->mem,
          H2_H202_MFG_STAGE_PATH, H2_H202_MFG_STAGE_PREV_PATH);
    }
    const int prepared = rc == H2_PAL_OK;
    if (prepared) {
      /* Core gates the App on the stored results alone; drop a record from
       * other acceptance criteria first. If that fails, stay in command mode
       * rather than boot an App the gate cannot vouch for. */
      const int revision = h2_h202_mfg_acceptance_check(s_runtime->pref);
      if (revision != H2_PAL_OK) {
        printf("H202_MFG_REVISION_FAIL rc=%d\r\n", revision);
        s_loader.force_command_mode = 1;
      }
      rc = h2_loader_startup(&s_loader, &action);
    }
    if (rc != H2_PAL_OK) (void)h2_loader_set_last_result(&s_loader, rc);
    const int unlock_rc = h2_pal_mutex_unlock(s_runtime->sync, s_operation);
    if (unlock_rc != H2_PAL_OK) fail("startup_unlock", unlock_rc);
    printf("H2_BK_H2LOADER_READY rc=%d action=%d prepared=%d\r\n", rc,
           (int)action, prepared);
    if (prepared && action == H2_LOADER_STARTUP_ACTION_COMMAND_MODE) return;
    rtos_delay_milliseconds(H2_H202_MFG_RETRY_DELAY_MS);
  }
}

static void mfg_entry(void* user) {
  (void)user;
  int rc = h2_bk7258_board_wait_for_wakeup(
      h2_h202_mfg_profile.controller.power_hold_ms);
  if (rc != H2_PAL_OK) fail("wake", rc);
  h2_runtime_config_t config;
  rc = h2_bk7258_board_runtime_config(&config);
  if (rc == H2_PAL_OK) rc = h2_runtime_init(&config, &s_runtime);
  if (rc != H2_PAL_OK) fail("runtime_init", rc);
  /* Acknowledge the accepted boot before Core hashes the Loader image. */
  (void)h2_h202_mfg_boot_feedback(s_runtime, &h2_h202_mfg_profile);
  static char device_uid[13];
  rc = h2_bk_h2loader_get_device_uid(device_uid, sizeof(device_uid));
  if (rc != H2_PAL_OK) fail("device_uid", rc);
  start_loader(device_uid);
  run_startup();
  const h2_h202_mfg_service_config_t service = {
      .runtime = s_runtime,
      .profile = &h2_h202_mfg_profile,
      .operation_sync = s_runtime->sync,
      .operation_mutex = s_operation,
      .drain_runtime_events = true};
  rc = h2_h202_mfg_service_start(&service, &s_service);
  /* UART management stays reachable whatever the result. */
  printf("H202_MFG_BOOT rc=%d\r\n", rc);
  hold();
}

int main(void) {
  if (h2_bk_target_task_policy_install() != H2_PAL_OK) return -1;
  bk_init();
  h2_pal_task_t* entry = NULL;
  const h2_pal_task_options_t options = {.name = "h202/entry",
                                         .min_stack_size = 32u * 1024u};
  int rc = h2_pal_task_start(h2_bk_platform_task_api(), &options, mfg_entry,
                             NULL, &entry);
  if (rc != H2_PAL_OK) printf("H202_MFG_ENTRY_FAIL rc=%d\r\n", rc);
  return 0;
}
