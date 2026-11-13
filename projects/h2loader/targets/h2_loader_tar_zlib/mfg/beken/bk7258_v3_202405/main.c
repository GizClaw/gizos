#include "h2_mfg.h"
#include "h2loader_board_extension.h"
#include "h2_bk7258_board.h"
#include "h2_bk_platform_core.h"
#include "h2_bk_target_task_policy.h"
#include "h2_device_service.h"

#include "bk_private/bk_init.h"
#include "os/os.h"

#include <stdio.h>

static h2_device_loader_t loader;
static h2_runtime_t* runtime;
/* The entry holds for the entire firmware lifetime, including terminal errors.
 */
static h2_pal_task_t* entry_task;

static void hold(void) {
  for (;;) rtos_delay_milliseconds(1000u);
}

int h2loader_board_extend_runtime(h2_runtime_config_t* config) {
  h2_runtime_config_t full;
  int rc = h2_bk7258_board_runtime_config(&full);
  if (rc != 0) return rc;
  /* Keep Loader's storage namespace and reboot adapter. */
  full.fs = config->fs;
  full.power = config->power;
  *config = full;
  return 0;
}

extern const h2_mfg_profile_t h2_mfg_board_profile;

int h2loader_board_extend_app(h2_runtime_t* instance,
                              h2loader_app_config_t* config) {
  h2_mfg_t* mfg = NULL;
  int rc = h2_mfg_create(instance, &h2_mfg_board_profile, &mfg);
  if (rc != 0) return rc;
  config->loader.mfg_required_total = h2_mfg_board_profile.total;
  config->loader.disruptive_user = mfg;
  config->loader.before_disruptive = h2_mfg_before_disruptive;
  config->command.mfg_user = mfg;
  config->command.mfg_command = h2_mfg_command;
  config->before_startup_user = mfg;
  config->before_startup = h2_mfg_run;
  config->rearm_before_startup = h2_mfg_rearm;
  config->loader.hardware_capabilities = H2_LOADER_CAPABILITIES_ALL;
  return 0;
}

static void mfg_entry(void* user) {
  (void)user;
  h2_runtime_config_t config;
  int rc = h2_bk7258_board_recovery_runtime_config(&config);
  if (!rc) rc = h2loader_board_extend_runtime(&config);
  if (!rc) rc = h2_runtime_init(&config, &runtime);
  if (rc) {
    printf("H2_BK_MFG_ENTRY_FAIL stage=runtime rc=%d\r\n", rc);
    hold();
  }
  const h2_device_loader_config_t device = {
      .board = runtime->board,
      .app_entry_path = "app/bk/app_ab_crc.rbl",
      .loader_partition_id = 1u,
      .app_partition_id = 2u,
      .coredump_partition_id = 0x7a0000u,
      .hardware_capabilities = H2_LOADER_CAPABILITIES_ALL,
      .boot_policy_flags = H2_DEVICE_BOOT_PREPARE_SELECTION,
      .identity_policy = H2_DEVICE_IDENTITY_PADDED_WINDOW,
      .serial_post_write_poll_ms = 1u,
      .ble_advertising_mode = H2_LOADER_BLE_ADVERTISING_LEGACY,
      .extend_app = h2loader_board_extend_app,
  };
  /* This entry owns main and directly references its extensions. Weak defaults
   * in the ordinary Loader entry must not satisfy the archive references. */
  printf("H2_BK_MFG_BOOT profile=%s total=%u\r\n", h2_mfg_board_profile.board,
         (unsigned)h2_mfg_board_profile.total);
  rc = h2_device_loader_run(&loader, runtime, &device, NULL, 1);
  printf("H2_BK_MFG_ENTRY_FAIL stage=loader rc=%d\r\n", rc);
  hold();
}

int main(void) {
  int rc = h2_bk_target_task_policy_install();
  if (rc) return -1;
  bk_init();
  /* The detached BSP helper has a fixed stack. This target's synchronous
   * package activation needs the installed task policy's larger reservation. */
  const h2_pal_task_options_t options = {
      .name = "bk/h2loader",
      .min_stack_size = 24u * 1024u,
  };
  rc = h2_pal_task_start(h2_bk_platform_task_api(), &options, mfg_entry, NULL,
                         &entry_task);
  if (rc) printf("H2_BK_MFG_ENTRY_FAIL stage=task rc=%d\r\n", rc);
  return rc ? -1 : 0;
}
