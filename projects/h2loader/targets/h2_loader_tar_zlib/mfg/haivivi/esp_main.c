#include "h2_esp_board.h"
#include "h2_esp_target_task_policy.h"
#include "h106_mfg_entry.h"

static void image_entry(void* unused) {
  (void)unused;
  h2_runtime_config_t config = {0};
  h2_esp_board_set_nfc_enabled(true);
  int rc = h2_esp_board_runtime_config(&config);
  if (rc == H2_PAL_OK) {
    h2_device_loader_config_t loader = {
        .loader_partition_id = 1u,
        .app_partition_id = 2u,
        .coredump_partition_id = 3u,
        .hardware_capabilities = H2_LOADER_CAPABILITIES_ALL,
        .identity_policy = H2_DEVICE_IDENTITY_NATIVE,
        .digest_yield_ms = 1u,
    };
    h106_mfg_run(&config, &loader, 1);
  } else {
    printf("H2_MFG_INIT_FAIL board=%s code=%d\n", h2_mfg_board_profile.board,
           rc);
  }
  h2_esp_board_set_nfc_enabled(false);
}

void app_main(void) {
  int rc = h2_esp_target_task_policy_install_with_configure(
      h2_esp_board_task_configure);
  if (rc == H2_PAL_OK)
    rc = h2_esp_board_start_entry_task("h106/entry", image_entry, NULL);
  if (rc != H2_PAL_OK) printf("H2_MFG_ENTRY_FAIL code=%d\n", rc);
}
