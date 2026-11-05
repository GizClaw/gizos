/* Exercise the actual startup entry, initialization lock, UART publication and
 * retry loop. Only native platform setup and the MFG worker service are faked.
 */
#include "h2_h202_mfg.h"
#include "h2_loader_command.h"
#include "h2_loader_metadata.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int test_runtime_init(const h2_runtime_config_t*, h2_runtime_t**);
static int test_identity(const h2_loader_config_t*, const char*,
                         h2_loader_image_identity_t*);
static int test_firmware(const h2_pal_firmware_info_api_t*,
                         h2_pal_firmware_info_t*);
static int test_loader_init(h2_loader_t*, const h2_loader_config_t*);
static h2_pal_result_t test_service_start(const h2_h202_mfg_service_config_t*,
                                          h2_h202_mfg_service_t**);
static h2_pal_result_t test_feedback(h2_runtime_t*,
                                     const h2_h202_mfg_profile_t*);
#define h2_runtime_init test_runtime_init
#define h2_loader_read_current_loader_identity test_identity
#define h2_pal_firmware_info_get_current test_firmware
#define h2_loader_init test_loader_init
#define h2_h202_mfg_service_start test_service_start
#define h2_h202_mfg_boot_feedback test_feedback
#define main native_launcher_main
#include "../ap_main.c"
#undef main
#undef h2_loader_init

const h2_h202_mfg_profile_t h2_h202_mfg_profile = {0};
static jmp_buf stopped;
static struct {
  unsigned locked, locks, unlocks, uart, service, retries, validations;
  unsigned mounts, publish_stats, binds, identities, storage_calls;
  bool active, fail_reset, fail_validation, fail_dl, fail_data, fail_publish;
  bool initial_failure, current_revision;
} f;
static bool fail_bind, fail_storage_once;

typedef struct entry {
  char key[40];
  unsigned char data[1024];
  size_t len;
  uint32_t value;
  bool used, scalar;
} entry_t;
static entry_t store[16];
static h2_pal_pref_namespace_t ns;

static entry_t* find(const char* key, bool create) {
  for (size_t i = 0; i < 16u; ++i)
    if (store[i].used && strcmp(store[i].key, key) == 0) return &store[i];
  if (!create) return NULL;
  for (size_t i = 0; i < 16u; ++i) {
    if (store[i].used) continue;
    store[i].used = true;
    snprintf(store[i].key, sizeof(store[i].key), "%s", key);
    return &store[i];
  }
  abort();
}

static int close_ns(h2_pal_pref_namespace_t* n) {
  (void)n;
  return H2_PAL_OK;
}
static int get_blob(h2_pal_pref_namespace_t* n, const h2_pal_mem_api_t* mem,
                    const char* key, void** out, size_t* len) {
  (void)n;
  entry_t* e = find(key, false);
  if (e == NULL || e->scalar) return H2_PAL_ERR_NOT_FOUND;
  *out = h2_pal_mem_alloc(mem, e->len);
  assert(*out != NULL);
  memcpy(*out, e->data, e->len);
  *len = e->len;
  return H2_PAL_OK;
}
static int set_blob(h2_pal_pref_namespace_t* n, const char* key,
                    const void* data, size_t len) {
  (void)n;
  if (f.active && f.fail_reset && strcmp(key, "mfg") == 0) return H2_PAL_ERR_IO;
  entry_t* e = find(key, true);
  assert(len <= sizeof(e->data));
  memcpy(e->data, data, len);
  e->len = len;
  e->scalar = false;
  return H2_PAL_OK;
}
static int get_u32(h2_pal_pref_namespace_t* n, const char* key, uint32_t* out) {
  (void)n;
  if (f.active && strcmp(key, "mfg_acceptance_revision") == 0) {
    assert(f.locked == 1u);
    ++f.validations;
    if (f.fail_validation) return H2_PAL_ERR_IO;
  }
  entry_t* e = find(key, false);
  if (e == NULL || !e->scalar) return H2_PAL_ERR_NOT_FOUND;
  *out = e->value;
  return H2_PAL_OK;
}
static int set_u32(h2_pal_pref_namespace_t* n, const char* key,
                   uint32_t value) {
  (void)n;
  entry_t* e = find(key, true);
  e->scalar = true;
  e->value = value;
  return H2_PAL_OK;
}
static int set_i32(h2_pal_pref_namespace_t* n, const char* key, int32_t value) {
  return set_u32(n, key, (uint32_t)value);
}
static int erase_key(h2_pal_pref_namespace_t* n, const char* key) {
  (void)n;
  entry_t* e = find(key, false);
  if (e == NULL) return H2_PAL_ERR_NOT_FOUND;
  memset(e, 0, sizeof(*e));
  return H2_PAL_OK;
}
static int open_pref(void* user, const char* name, h2_pal_pref_open_mode_t mode,
                     h2_pal_pref_namespace_t** out) {
  (void)user;
  (void)mode;
  assert(strcmp(name, "h2loader") == 0);
  if (f.active) assert(f.locked == 1u);
  *out = &ns;
  return H2_PAL_OK;
}
static const h2_pal_pref_vtable_t pref_vtable = {.open = open_pref};
static const h2_pal_pref_api_t pref = {.vtable = &pref_vtable};
static void* allocate(void* user, size_t len) {
  (void)user;
  return malloc(len);
}
static void release(void* user, void* p) {
  (void)user;
  free(p);
}
static const h2_pal_mem_vtable_t mem_vtable = {.alloc = allocate,
                                               .free = release};
static h2_pal_mem_api_t mem = {.vtable = &mem_vtable};

static int mutex_storage;
static int create_mutex(void* user, const h2_pal_mutex_config_t* config,
                        h2_pal_mutex_t** out) {
  (void)user;
  assert(config->flags == H2_PAL_MUTEX_FLAG_RECURSIVE);
  *out = (h2_pal_mutex_t*)&mutex_storage;
  return H2_PAL_OK;
}
static int lock_mutex(void* user, h2_pal_mutex_t* mutex) {
  (void)user;
  assert(mutex == (h2_pal_mutex_t*)&mutex_storage);
  ++f.locked;
  ++f.locks;
  return H2_PAL_OK;
}
static int unlock_mutex(void* user, h2_pal_mutex_t* mutex) {
  (void)user;
  assert(mutex == (h2_pal_mutex_t*)&mutex_storage && f.locked != 0u);
  --f.locked;
  ++f.unlocks;
  return H2_PAL_OK;
}
static const h2_pal_sync_vtable_t sync_vtable = {.create_mutex = create_mutex,
                                                 .lock_mutex = lock_mutex,
                                                 .unlock_mutex = unlock_mutex};
static const h2_pal_sync_api_t sync = {.vtable = &sync_vtable};

static int mkdir_fs(void* user, const char* path) {
  (void)user;
  assert(f.locked == 1u && f.validations >= 2u && f.uart == 1u);
  ++f.mounts;
  if ((f.fail_dl && strcmp(path, "/dl") == 0) ||
      (f.fail_data && strcmp(path, "/data") == 0))
    return H2_PAL_ERR_IO;
  return H2_PAL_OK;
}
static int stat_fs(void* user, const char* path, h2_pal_fs_stat_t* out) {
  (void)user;
  (void)path;
  (void)out;
  assert(f.locked == 1u && f.validations >= 2u);
  ++f.publish_stats;
  return f.fail_publish ? H2_PAL_ERR_IO : H2_PAL_ERR_NOT_FOUND;
}
static const h2_pal_fs_vtable_t fs_vtable = {.mkdir = mkdir_fs,
                                             .stat = stat_fs};
static const h2_pal_fs_api_t fs = {.vtable = &fs_vtable};
static int running_partition(void* user, h2_pal_power_boot_partition_t* out) {
  (void)user;
  *out = (h2_pal_power_boot_partition_t){.id = 1u};
  return H2_PAL_OK;
}
static const h2_pal_power_vtable_t power_vtable = {.get_running_boot_partition =
                                                       running_partition};
static const h2_pal_power_api_t power = {.vtable = &power_vtable};
static h2_runtime_t runtime;

h2_pal_mem_api_t* h2_bk_platform_psram_allocator(void) { return &mem; }
const h2_pal_power_api_t* h2_bk_h2loader_power_api(void) { return &power; }
const h2_pal_task_api_t* h2_bk_platform_task_api(void) { return NULL; }
int h2_bk_target_task_policy_install(void) { return H2_PAL_OK; }
void bk_init(void) {}
int h2_bk7258_board_wait_for_wakeup(uint32_t hold_ms) {
  (void)hold_ms;
  return H2_PAL_OK;
}
int h2_bk7258_board_runtime_config(h2_runtime_config_t* config) {
  memset(config, 0, sizeof(*config));
  return H2_PAL_OK;
}
int h2_bk7258_board_fs_mount(bool format_on_mount_failure) {
  assert(f.uart == 1u && f.locked == 1u);
  assert(format_on_mount_failure == (f.storage_calls == 0u));
  ++f.storage_calls;
  return fail_storage_once && f.storage_calls == 1u ? H2_PAL_ERR_IO : H2_PAL_OK;
}
int h2_bk_h2loader_get_device_uid(char* out, size_t capacity) {
  assert(capacity >= 13u);
  memcpy(out, "123456abcdef", 13u);
  return H2_PAL_OK;
}
int h2_bk_h2loader_confirm_active_loader(void* user) {
  (void)user;
  assert(f.locked == 1u);
  return H2_PAL_OK;
}
static int test_runtime_init(const h2_runtime_config_t* config,
                             h2_runtime_t** out) {
  (void)config;
  *out = &runtime;
  return H2_PAL_OK;
}
static h2_pal_result_t test_feedback(h2_runtime_t* r,
                                     const h2_h202_mfg_profile_t* profile) {
  (void)r;
  (void)profile;
  return H2_PAL_OK;
}
int h2_bk_h2loader_boot_context_bind(const h2_pal_pref_api_t* pref_api,
                                     const h2_pal_mem_api_t* allocator) {
  assert(f.locked == 1u && f.uart == 0u && f.identities == 0u);
  assert(pref_api == runtime.pref && allocator == runtime.mem);
  ++f.binds;
  return fail_bind ? H2_PAL_ERR_INVALID_STATE : H2_PAL_OK;
}
static int test_firmware(const h2_pal_firmware_info_api_t* api,
                         h2_pal_firmware_info_t* out) {
  (void)api;
  assert(f.binds == 1u);
  snprintf(out->version, sizeof(out->version), "test-loader");
  return H2_PAL_OK;
}
static int test_identity(const h2_loader_config_t* config, const char* version,
                         h2_loader_image_identity_t* out) {
  assert(f.locked == 1u && f.binds == 1u);
  ++f.identities;
  *out = config->active_identity;
  snprintf(out->version, sizeof(out->version), "%s", version);
  memset(out->image_sha256, 'a', 64u);
  out->image_sha256[64] = '\0';
  out->image_size = 4096u;
  return H2_PAL_OK;
}
static int test_loader_init(h2_loader_t* loader,
                            const h2_loader_config_t* config) {
  int rc = h2_loader_init(loader, config);
  assert(rc == H2_PAL_OK);
  /* Core has really loaded all-PASSED results and a valid App before the
   * launcher's revision gate runs. This is the stale-cache regression. */
  assert(loader->status.partition_2.valid);
  for (unsigned i = 0; i < H2_H202_MFG_ITEM_COUNT; ++i)
    assert(loader->status.mfg.step_status[i] == H2_H202_MFG_PASSED);
  return rc;
}

static void assert_recovery_available(bool app_allowed) {
  uint32_t available =
      h2_loader_get_command_availability(&s_loader, &s_loader.status);
  assert(((available & H2_LOADER_COMMAND_AVAILABLE_REBOOT_APP) != 0u) ==
         app_allowed);
  assert((available & H2_LOADER_COMMAND_AVAILABLE_REBOOT_LOADER) != 0u);
  assert((available & H2_LOADER_COMMAND_AVAILABLE_REBOOT_UPGRADE) != 0u);
  assert((available & H2_LOADER_COMMAND_AVAILABLE_STAGE_PAYLOAD) != 0u);
}
static int command_write(void* user, const void* data, size_t len,
                         size_t* written, uint32_t timeout_ms) {
  (void)user;
  (void)data;
  (void)timeout_ms;
  *written = len;
  return H2_PAL_OK;
}
static int command_read(void* user, void* data, size_t len, size_t* read,
                        uint32_t timeout_ms) {
  (void)user;
  (void)data;
  (void)len;
  (void)timeout_ms;
  *read = 0u;
  return H2_PAL_ERR_TIMEOUT;
}
static int command_flush(void* user) {
  (void)user;
  return H2_PAL_OK;
}
static const h2_command_io_vtable_t command_io = {
    .read = command_read, .write = command_write, .flush = command_flush};
static void assert_reboot_app_rejected(void) {
  const char* const argv[] = {"h2loader", "reboot", "app"};
  assert(h2_loader_command_execute(&s_command, 3u, argv) ==
         H2_PAL_ERR_INVALID_STATE);
}
int h2_bk_h2loader_start_loader_iostreamikcp(
    h2_runtime_t* r, h2_loader_command_t* command,
    const h2_loader_command_config_t* config) {
  assert(r == &runtime && config->loader == &s_loader);
  assert(config->operation_sync == &sync &&
         config->operation_mutex == s_operation);
  assert(f.locked == 1u && f.unlocks == 0u && f.validations == 1u);
  assert(f.mounts == 0u && f.publish_stats == 0u);
  h2_loader_command_config_t commands = *config;
  commands.io.vtable = &command_io;
  assert(h2_loader_command_init(command, &commands) == H2_PAL_OK);
  assert_recovery_available(f.current_revision);
  assert((s_loader.force_command_mode != 0) == f.initial_failure);
  ++f.uart;
  return H2_PAL_OK;
}
void rtos_delay_milliseconds(uint32_t delay_ms) {
  assert(f.locked == 0u);
  if (fail_bind) {
    assert(delay_ms == 1000u && f.binds == 1u && f.uart == 0u &&
           f.service == 0u && f.identities == 0u);
    longjmp(stopped, 1);
  }
  if (delay_ms == H2_H202_MFG_RETRY_DELAY_MS) {
    assert(f.service == 0u && f.retries == 0u && f.validations == 2u);
    assert_recovery_available(false);
    assert_reboot_app_rejected();
    assert((s_loader.force_command_mode != 0) == f.initial_failure);
    if (f.fail_dl || f.fail_data) assert(f.publish_stats == 0u);
    if (f.fail_publish) assert(f.publish_stats != 0u);
    if (fail_storage_once) assert(f.mounts == 0u && f.publish_stats == 0u);
    ++f.retries;
    f.fail_reset = f.fail_validation = f.fail_dl = f.fail_data =
        f.fail_publish = false;
    return;
  }
  assert(delay_ms == 1000u && f.service == 1u);
  longjmp(stopped, 1);
}
static h2_pal_result_t test_service_start(
    const h2_h202_mfg_service_config_t* config, h2_h202_mfg_service_t** out) {
  (void)out;
  assert(f.locked == 0u && f.uart == 1u && !s_loader.force_command_mode);
  assert_recovery_available(f.current_revision);
  assert(config->progress_changed != NULL && config->progress_user == NULL);
  assert(h2_pal_mutex_lock(config->operation_sync, config->operation_mutex) ==
         H2_PAL_OK);
  h2_loader_mfg_summary_t summary = {.total = H2_H202_MFG_ITEM_COUNT};
  assert(config->progress_changed(config->progress_user, &summary) ==
         H2_PAL_OK);
  assert_recovery_available(false);
  memset(summary.step_status, H2_H202_MFG_PASSED, H2_H202_MFG_ITEM_COUNT);
  assert(config->progress_changed(config->progress_user, &summary) ==
         H2_PAL_OK);
  assert_recovery_available(true);
  assert(h2_pal_mutex_unlock(config->operation_sync, config->operation_mutex) ==
         H2_PAL_OK);
  ++f.service;
  return H2_PAL_OK;
}

static void run_case(bool dl, bool data, bool publish, bool reset,
                     bool validation, bool current_revision) {
  memset(&f, 0, sizeof(f));
  memset(store, 0, sizeof(store));
  ns = (h2_pal_pref_namespace_t){.close = close_ns,
                                 .commit = close_ns,
                                 .get_blob = get_blob,
                                 .set_blob = set_blob,
                                 .get_u32 = get_u32,
                                 .set_u32 = set_u32,
                                 .set_i32 = set_i32,
                                 .remove = erase_key};
  static const h2_pal_http_api_t http = {0};
  static const h2_pal_wifi_sta_api_t wifi = {0};
  runtime = (h2_runtime_t){.mem = &mem,
                           .sync = &sync,
                           .pref = &pref,
                           .fs = &fs,
                           .http = &http,
                           .wifi_sta = &wifi};
  assert(h2_loader_mfg_ensure_acceptance_revision(
             &pref, H2_H202_MFG_ITEM_COUNT,
             current_revision ? H2_H202_MFG_REVISION
                              : H2_H202_MFG_REVISION - 1u) == H2_PAL_OK);
  h2_loader_mfg_summary_t summary = {.total = H2_H202_MFG_ITEM_COUNT};
  memset(summary.step_status, H2_H202_MFG_PASSED, H2_H202_MFG_ITEM_COUNT);
  assert(h2_loader_mfg_write(&pref, &summary) == H2_PAL_OK);
  h2_loader_metadata_t app = {.valid = 1,
                              .role = H2_LOADER_IMAGE_ROLE_APP,
                              .image_size = 4096u,
                              .version = "1.0.0",
                              .board = H2_H202_BOARD_NAME,
                              .target = "bk7258"};
  memset(app.image_checksum, 'b', 64u);
  assert(h2_loader_metadata_write(&pref, H2_LOADER_METADATA_SLOT_PARTITION_2,
                                  &app) == H2_PAL_OK);
  if (!current_revision)
    assert(set_u32(&ns, "boot_intent", H2_LOADER_BOOT_INTENT_AUTO) ==
           H2_PAL_OK);
  f.fail_dl = dl;
  f.fail_data = data;
  f.fail_publish = publish;
  f.fail_reset = reset;
  f.fail_validation = validation;
  f.initial_failure = reset || validation;
  f.current_revision = current_revision;
  f.active = true;
  if (setjmp(stopped) == 0) mfg_entry(NULL);
  if (fail_bind) {
    assert(f.uart == 0u && f.service == 0u && f.identities == 0u &&
           f.binds == 1u && f.validations == 0u && f.locks == f.unlocks);
    return;
  }
  assert(f.binds == 1u && f.identities == 1u);
  assert(f.uart == 1u && f.service == 1u && f.locked == 0u);
  assert(f.locks == f.unlocks);
  assert(f.retries == (unsigned)(dl || data || publish || reset || validation ||
                                 fail_storage_once));
  assert(f.storage_calls == 1u + f.retries);
  assert(f.validations == 2u + f.retries);
  h2_loader_deinit(&s_loader);
}
int main(void) {
  run_case(true, false, false, false, false, false);
  run_case(false, true, false, false, false, false);
  run_case(false, false, true, false, false, false);
  run_case(true, false, false, true, false, false);
  run_case(false, false, true, true, false, false);
  run_case(false, false, false, true, false, false);
  run_case(false, false, false, false, true, false);
  run_case(false, false, false, false, false, true);
  fail_storage_once = true;
  run_case(false, false, false, false, false, false);
  fail_storage_once = false;
  fail_bind = true;
  run_case(false, false, false, false, false, true);
  puts("H202 MFG launcher startup and boot-context regression passed");
  return 0;
}
