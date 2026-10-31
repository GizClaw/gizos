#include "h2_esp_board.h"
#include "h2_esp_h2loader_runtime.h"
#include "h2_esp_target_task_policy.h"
#include "h2_mfg.h"
#include "h2_mosaico_loader_usb.h"
#include "h2_mosaico_usb_console.h"
#include "bsp/esp_mosaico.h"

#include <stdio.h>

extern const h2_mfg_profile_t h2_mfg_board_profile;

static void image_entry(void* unused) {
  (void)unused;
  h2_runtime_config_t config;
  h2_runtime_t* runtime = NULL;
  h2_mfg_t* mfg = NULL;
  int rc = h2_esp_board_runtime_config(&config);
  if (rc == H2_PAL_OK) rc = h2_runtime_init(&config, &runtime);
  if (rc == H2_PAL_OK) rc = h2_mfg_create(runtime, &h2_mfg_board_profile, &mfg);
  if (rc != H2_PAL_OK) {
    printf("H2_MFG_INIT_FAIL board=%s code=%d\n", h2_mfg_board_profile.board,
           rc);
    if (runtime != NULL) h2_runtime_deinit(runtime);
    return;
  }

  h2_esp_h2loader_config_t loader = {
      .board = h2_mfg_board_profile.board,
      .mfg_required_total = h2_mfg_board_profile.total,
      .user = mfg,
      .mfg_command = h2_mfg_command,
      .run_mfg = h2_mfg_run,
      .rearm_mfg = h2_mfg_rearm,
      .before_disruptive = h2_mfg_before_disruptive,
      .install_progress = NULL,
      .show_installing = NULL,
      .show_install_failed = NULL,
      .show_launching = NULL,
  };
  h2_esp_h2loader_run_with_config(runtime, &loader);
  /* Returning also tears down the board; retain it until MFG releases every
   * provider and worker, including after a failed cleanup attempt. */
  while ((rc = h2_mfg_destroy(&mfg)) != H2_PAL_OK) {
    printf("H2_MFG_CLEANUP_FAIL board=%s code=%d\n", h2_mfg_board_profile.board,
           rc);
    (void)h2_pal_time_sleep_ms(runtime->time, 1000);
  }
  h2_runtime_deinit(runtime);
}

void app_main(void) {
  bsp_board_variant_t variant;
  int rc = bsp_board_variant_get(&variant);
  if (rc == ESP_OK) rc = h2_esp_target_task_policy_install();
  if (rc == H2_PAL_OK) rc = h2_mosaico_usb_console_init();
  if (rc == H2_PAL_OK) rc = h2_mosaico_loader_usb_init();
  if (rc == H2_PAL_OK)
    rc = h2_esp_board_start_entry_task("mosaico/mfg", image_entry, NULL);
  if (rc != H2_PAL_OK)
    printf("H2_MFG_ENTRY_FAIL board=esp_mosaico code=%d\n", rc);
}
