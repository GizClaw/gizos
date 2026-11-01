#include "h2_runtime.h"
#include "h2_bk7258_board.h"
#include "h2_h202_board_config.h"
#include "h2/pal/h2_pal_unsupported.h"
#include <assert.h>
#include <string.h>
static int board_result, fs_result;
static h2_pal_fs_api_t s_h2loader_fs;
int h2_bk7258_board_init(void) { return board_result; }
int h2_bk7258_board_fs_init(h2_pal_fs_api_t* fs) {
  (void)fs;
  return fs_result;
}
const char* h2_bk7258_board_ble_local_name(void) { return "h202-ABCDEF"; }
#define h2_bk_platform_firmware_info_api() NULL
#define h2_bk_platform_default_allocator() NULL
#define h2_bk_platform_log_api() NULL
#define h2_bk_platform_time_api() NULL
#define h2_bk_platform_task_api() NULL
#define h2_bk_platform_queue_api() NULL
#define h2_bk_platform_sync_api() NULL
#define h2_bk_platform_pref_api() NULL
#define h2_bk_platform_crypto_api() NULL
#define h2_bk_platform_ble() NULL
#define h2_bk_platform_system_event_api() NULL
#define h2_bk_h2loader_power_api() NULL
#include "h2_loader_boot.h"

static int startup_result, unlock_result, saved_result, save_calls,
    unlock_calls;
static h2_loader_startup_action_t next_action;
static int fake_startup(h2_loader_t* loader,
                        h2_loader_startup_action_t* action) {
  (void)loader;
  *action = next_action;
  return startup_result;
}
static int fake_set_last_result(h2_loader_t* loader, int result) {
  (void)loader;
  saved_result = result;
  save_calls++;
  return H2_PAL_OK;
}
static int fake_unlock(void* user, h2_pal_mutex_t* mutex) {
  (void)user;
  (void)mutex;
  unlock_calls++;
  return unlock_result;
}
#define h2_loader_startup fake_startup
#define h2_loader_set_last_result fake_set_last_result
#include "../ap/ap_main.c"

static void test_startup(void) {
  const h2_pal_sync_vtable_t vtable = {.unlock_mutex = fake_unlock};
  const h2_pal_sync_api_t sync = {.vtable = &vtable};
  int mutex_storage;
  h2_pal_mutex_t* mutex = (h2_pal_mutex_t*)&mutex_storage;
  h2_loader_t loader = {0};
  h2_loader_startup_action_t action;
  int result;
  bool ready;

  saved_result = H2_PAL_ERR_UNSUPPORTED;
  next_action = H2_LOADER_STARTUP_ACTION_COMMAND_MODE;
  assert(loader_startup_attempt(&loader, &sync, mutex, &action, &result,
                                &ready) == H2_PAL_OK);
  assert(result == H2_PAL_OK && ready && save_calls == 0);
  assert(saved_result == H2_PAL_ERR_UNSUPPORTED && unlock_calls == 1);

  startup_result = H2_PAL_ERR_UNSUPPORTED;
  assert(loader_startup_attempt(&loader, &sync, mutex, &action, &result,
                                &ready) == H2_PAL_OK);
  assert(ready && result == startup_result && save_calls == 1);
  assert(saved_result == startup_result && unlock_calls == 2);

  next_action = H2_LOADER_STARTUP_ACTION_REBOOTING_APP;
  assert(loader_startup_attempt(&loader, &sync, mutex, &action, &result,
                                &ready) == H2_PAL_OK);
  assert(!ready);

  unlock_result = H2_PAL_ERR_IO;
  assert(loader_startup_attempt(&loader, &sync, mutex, &action, &result,
                                &ready) == H2_PAL_ERR_IO);
  assert(!ready);
  startup_result = H2_PAL_OK;
  next_action = H2_LOADER_STARTUP_ACTION_COMMAND_MODE;
  assert(loader_startup_attempt(&loader, &sync, mutex, &action, &result,
                                &ready) == H2_PAL_ERR_IO);
  assert(!ready);

  assert(loader_failed_attempt(&loader, &sync, mutex, H2_PAL_ERR_NO_MEMORY) ==
         H2_PAL_ERR_IO);
  assert(saved_result == H2_PAL_ERR_NO_MEMORY);
  unlock_result = H2_PAL_OK;
  assert(loader_failed_attempt(&loader, &sync, mutex, H2_PAL_ERR_IO) ==
         H2_PAL_OK);
  assert(saved_result == H2_PAL_ERR_IO);
}

int main(void) {
  test_startup();
  h2_runtime_config_t config = {0};
  board_result = H2_PAL_ERR_IO;
  assert(loader_runtime_config(&config) == H2_PAL_ERR_IO);
  board_result = 0;
  fs_result = H2_PAL_ERR_NO_MEMORY;
  assert(loader_runtime_config(&config) == H2_PAL_ERR_NO_MEMORY);
  fs_result = 0;
  assert(loader_runtime_config(&config) == H2_PAL_OK);
  assert(strcmp(config.board, "h202") == 0 &&
         strcmp(config.target, "bk7258") == 0);
  assert(config.fs == &s_h2loader_fs);
  assert(strcmp(config.ble_local_name, "h202-ABCDEF") == 0);
  assert(config.audio == h2_pal_unsupported_audio_api());
  assert(config.nfc == h2_pal_unsupported_nfc_api());
  assert(config.modem == h2_pal_unsupported_modem_api());
  assert(config.display == h2_pal_unsupported_display_api());
  assert(config.wifi_sta == h2_pal_unsupported_wifi_sta_api());
  return 0;
}
