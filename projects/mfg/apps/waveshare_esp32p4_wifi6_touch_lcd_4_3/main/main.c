#include "h2_esp_board.h"
#include "h2_esp_h2loader_ble.h"
#include "h2_esp_target_task_policy.h"
#include "h2_mfg.h"

#include <stdio.h>

extern const h2_mfg_profile_t h2_mfg_board_profile;

static void image_entry(void* unused) {
  (void)unused;
  h2_runtime_config_t config;
  h2_runtime_t* runtime = NULL;
  h2_mfg_t* mfg = NULL;
  int rc = h2_esp_board_runtime_config(&config);
  if (rc == 0) rc = h2_runtime_init(&config, &runtime);
  if (rc == 0) rc = h2_mfg_create(runtime, &h2_mfg_board_profile, &mfg);
  if (rc != 0) {
    printf("H2_MFG_INIT_FAIL board=%s code=%d\n", h2_mfg_board_profile.board,
           rc);
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
  };
  h2_esp_h2loader_run_with_ble_config(runtime, &loader);
}

void app_main(void) {
  int rc = h2_esp_target_task_policy_install();
  if (rc == 0)
    rc = h2_esp_board_start_entry_task("mfg/entry", image_entry, NULL);
  if (rc != 0) printf("H2_MFG_ENTRY_FAIL code=%d\n", rc);
}
