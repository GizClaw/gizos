#include "h2_esp_board.h"
#include "h2_esp_h2loader_ble.h"
#include "h2_esp_h2loader_runtime.h"
#include "h2_pal_e2e.h"
#include "h2_esp_layout_task_policy.h"

#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdio.h>

static void hold(void) {
  for (;;) vTaskDelay(pdMS_TO_TICKS(1000u));
}

static void run_entry(void *user) {
  h2_runtime_config_t runtime_config = {0};
  h2_runtime_t *runtime = NULL;
  h2_pal_e2e_config_t config = {.suite_mask = H2_PAL_E2E_SUITE_PREF};
  h2_pal_e2e_result_t result;
  int rc;
  (void)user;
  rc = h2_esp_board_runtime_config(&runtime_config);
  if (rc == H2_PAL_OK)
    rc = h2_esp_h2loader_app_commands_prepare_serial(&runtime_config,
                                                       "pal-pref", 1u, 3u);
  if (rc == H2_PAL_OK) rc = h2_runtime_init(&runtime_config, &runtime);
  if (rc == H2_PAL_OK)
    rc = h2_esp_h2loader_app_commands_start(runtime, "pal-pref", 1u, 3u);
  if (rc != H2_PAL_OK) {
    printf("H2_PAL_PREF_E2E_FAIL stage=runtime rc=%d\n", rc);
    fflush(stdout);
    hold();
  }
  rc = h2_pal_e2e_run(runtime, &config, &result);
  printf("H2_PAL_PREF_E2E phase=%d action=%d rc=%d selected=%u passed=%u "
         "failed=%u status=%s\n",
         (int)result.pref_phase, (int)result.action, rc,
         (unsigned)result.selected, (unsigned)result.passed,
         (unsigned)result.failed, rc == H2_PAL_OK ? "PASS" : "FAIL");
  fflush(stdout);
  if (rc != H2_PAL_OK) hold();
  if (result.pref_phase == H2_PAL_E2E_PREF_PHASE_SEED) {
    rc = h2_esp_h2loader_app_confirm(runtime);
    if (rc != H2_PAL_OK) {
      printf("H2_PAL_PREF_E2E_FAIL stage=confirm rc=%d\n", rc);
      fflush(stdout);
      hold();
    }
    printf("H2_PAL_PREF_E2E_CONFIRMED image=pal-pref\n");
    fflush(stdout);
  }
  if (result.action == H2_PAL_E2E_ACTION_REBOOT) {
    printf("H2_PAL_PREF_E2E_REBOOT next=%d\n", (int)result.pref_phase + 1);
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(100u));
    esp_restart();
  }
  printf("H2_PAL_PREF_E2E_READY status=PASS phase=%d\n",
         (int)result.pref_phase);
  fflush(stdout);
  hold();
}

void app_main(void) {
    if (h2_esp_layout_task_policy_install() != H2_PAL_OK) {
        return;
    }
  int rc = h2_esp_board_start_entry_task("devkit/pal-pref", run_entry, NULL);
  if (rc != H2_PAL_OK) {
    printf("H2_PAL_PREF_E2E_FAIL stage=entry rc=%d\n", rc);
    fflush(stdout);
  }
}
