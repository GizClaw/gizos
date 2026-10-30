#include "h2_esp_board.h"
#include "h2_esp_h2loader_ble.h"
#include "h2_esp_h2loader_runtime.h"
#include "h2_esp_target_task_policy.h"
#include "h2_mfg_peer.h"
#include <stdio.h>

static h2_runtime_t* runtime;

static void run(void* unused) {
  (void)unused;
  int rc = h2_mfg_peer_run_with_address(runtime, h2_mfg_peer_dut_uid(),
                                        h2_mfg_peer_dut_ble_address(), 1200000);
  printf("H2_MFG_PEER_END uid=%s code=%d\n", h2_mfg_peer_dut_uid(), rc);
}

void app_main(void) {
  h2_runtime_config_t config;
  int rc = h2_esp_target_task_policy_install();
  if (rc == 0) rc = h2_esp_board_runtime_config(&config);
  const h2_esp_h2loader_app_commands_config_t commands = {
      .active_name = "mfg-peer",
      .hardware_capabilities = H2_LOADER_CAPABILITY_UART,
      .h2loader_partition_id = 1,
      .coredump_partition_id = 3};
  if (rc == 0)
    rc = h2_esp_h2loader_app_commands_prepare_serial_with_config(&config,
                                                                 &commands);
  if (rc == 0) rc = h2_runtime_init(&config, &runtime);
  if (rc == 0) rc = h2_esp_h2loader_app_confirm(runtime);
  if (rc == 0) {
    const h2_pal_task_options_t options = {.name = "mfg/peer",
                                           .min_stack_size = 65536};
    h2_pal_task_t* task = NULL;
    printf("H2_MFG_PEER_BOOT board=%s uid=%s\n", runtime->board,
           h2_mfg_peer_dut_uid());
    rc = h2_pal_task_start(runtime->task, &options, run, NULL, &task);
  }
  if (rc != 0) printf("H2_MFG_PEER_INIT_FAIL code=%d\n", rc);
}
