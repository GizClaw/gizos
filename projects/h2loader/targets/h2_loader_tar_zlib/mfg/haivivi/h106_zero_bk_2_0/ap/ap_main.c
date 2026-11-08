#include "h2_bk7258_board.h"
#include "h2_bk_platform_core.h"
#include "h2_bk_target_task_policy.h"
#include "h106_mfg_entry.h"

#include "bk_private/bk_init.h"

#if CONFIG_BLUETOOTH || CONFIG_BT || CONFIG_BLE
#error "H106 Zero BK MFG uses the board's UART-only Bluetooth-disabled layout"
#endif

/* Retained for the firmware lifetime; fatal entry returns remain suspended. */
static h2_pal_task_t* image_task;

static int extend_loader(h2_runtime_t* runtime, h2loader_app_config_t* config) {
  (void)runtime;
  config->loader.accepted_board_alias = "bk7258_v3_202405";
  return H2_PAL_OK;
}

static void image_entry(void* unused) {
  (void)unused;
  h2_runtime_config_t config = {0};
  int rc = h2_bk7258_board_runtime_config(&config);
  if (rc != H2_PAL_OK) {
    printf("H2_MFG_INIT_FAIL board=%s code=%d\n", h2_mfg_board_profile.board,
           rc);
    return;
  }
  h2_device_loader_config_t loader = {
      .app_entry_path = "app/bk/app_ab_crc.rbl",
      .loader_partition_id = 1u,
      .app_partition_id = 2u,
      .coredump_partition_id = 0x7a0000u,
      .hardware_capabilities =
          H2_LOADER_CAPABILITY_UART | H2_LOADER_CAPABILITY_WIFI,
      .boot_policy_flags = H2_DEVICE_BOOT_PREPARE_SELECTION,
      .identity_policy = H2_DEVICE_IDENTITY_PADDED_WINDOW,
      .serial_post_write_poll_ms = 1u,
      .extend_app = extend_loader,
  };
  h106_mfg_run(&config, &loader, 0);
}

static void image_task_entry(void* user) {
  image_entry(user);
  (void)h2_bk7258_board_runtime_deinit();
}

int main(void) {
  int rc = h2_bk_target_task_policy_install();
  if (rc != H2_PAL_OK) return -1;
  bk_init();
  const h2_pal_task_options_t options = {.name = "bk/h2loader",
                                         .min_stack_size = 48u * 1024u};
  rc = h2_pal_task_start(h2_bk_platform_task_api(), &options, image_task_entry,
                         NULL, &image_task);
  if (rc != H2_PAL_OK) printf("H2_MFG_ENTRY_FAIL code=%d\n", rc);
  return 0;
}
