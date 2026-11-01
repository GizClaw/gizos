#include "h2_bk7258_board.h"
#include "h2_bk_h2loader.h"
#include "h2_bk_platform_core.h"
#include "h2_bk_target_task_policy.h"
#include "h2_h202_mfg.h"

#include "bk_private/bk_init.h"
#include "os/os.h"

#include <stdio.h>

#define H2_H202_MFG_IMAGE "h202-mfg"

extern const h2_h202_mfg_profile_t h2_h202_mfg_profile;

static h2_runtime_t* s_runtime;
static h2_h202_mfg_service_t* s_service;

static void hold(void) {
  for (;;) rtos_delay_milliseconds(1000);
}

/* Without management this App cannot be replaced; return to Loader. */
static void fail_to_loader(const char* stage, int rc) {
  printf("H202_MFG_BOOT_FAIL stage=%s rc=%d\r\n", stage, rc);
  rc = h2_bk_h2loader_reboot_to_loader();
  printf("H202_MFG_BOOT_FAIL stage=reboot_to_loader rc=%d\r\n", rc);
  hold();
}

/* Public BK App services only: no Loader Core, no before_disruptive. A
 * management reboot ends MFG without saving progress. */
static void mfg_entry(void* user) {
  (void)user;
  h2_runtime_config_t config;
  int rc = h2_bk7258_board_runtime_config(&config);
  if (rc != H2_PAL_OK) fail_to_loader("runtime_config", rc);
  rc = h2_runtime_init(&config, &s_runtime);
  if (rc != H2_PAL_OK) fail_to_loader("runtime_init", rc);
  /* Board init already enabled WAKE_UP; mounting never touches its GPIOs.
   * Report a failed mount instead of falling back; MFG itself needs none. */
  int dl = h2_pal_fs_mkdir(s_runtime->fs, H2_BK7258_DL_MOUNT_PATH);
  int data = h2_pal_fs_mkdir(s_runtime->fs, H2_BK7258_DATA_MOUNT_PATH);
  printf("H202_MFG_MOUNT dl=%d data=%d\r\n", dl, data);
  rc = h2_bk_h2loader_start_app_iostreamikcp(s_runtime, H2_H202_MFG_IMAGE);
  if (rc != H2_PAL_OK) fail_to_loader("app_uart", rc);
  rc = h2_bk_h2loader_start_app_ble(s_runtime, H2_H202_MFG_IMAGE);
  if (rc != H2_PAL_OK) fail_to_loader("app_ble", rc);
  h2_loader_app_client_config_t commands;
  rc = h2_bk_h2loader_app_commands_get_config(&commands);
  if (rc != H2_PAL_OK) fail_to_loader("app_commands", rc);
  const h2_h202_mfg_service_config_t service = {
      .runtime = s_runtime,
      .profile = &h2_h202_mfg_profile,
      .operation_sync = commands.operation_sync,
      .operation_mutex = commands.operation_mutex,
      .drain_runtime_events = true};
  rc = h2_h202_mfg_service_start(&service, &s_service);
  if (rc != H2_PAL_OK) {
    /* Management stays reachable; the unconfirmed App rolls back. */
    printf("H202_MFG_BOOT_FAIL stage=mfg rc=%d\r\n", rc);
    hold();
  }
  rc = h2_bk_h2loader_confirm_current_app(s_runtime);
  printf("H202_MFG_BOOT confirm=%d\r\n", rc);
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
