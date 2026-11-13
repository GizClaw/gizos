/* BK7258 DevKit App image: Runtime lifetime, H2Loader App commands over UART
 * and the game task. The game confirms the trial boot once its first frame is
 * visible with Touch open. */
#include "bk_private/bk_init.h"
#include "h2_bk7258_board.h"
#include "h2_bk_platform_core.h"
#include "h2_bk_target_task_policy.h"
#include "h2_device_service.h"
#include "h2_fruit_slice.h"
#include "h2_pal_unsupported.h"

#include <os/os.h>
#include <stdio.h>

static h2_device_app_commands_t h2_management = {
    .config =
        {
            .active_name = "app",
            .hardware_capabilities =
                H2_LOADER_CAPABILITY_UART | H2_LOADER_CAPABILITY_BLE,
            .h2loader_partition_id = 1u,
            .app_partition_id = 2u,
            .coredump_partition_id = 0x7a0000u,
            .app_entry_path = "app/bk/app_ab_crc.rbl",
            .boot_policy_flags = H2_DEVICE_BOOT_PREPARE_SELECTION |
                                 H2_DEVICE_BOOT_PREPARE_APP_RESTART,
            .identity_policy = H2_DEVICE_IDENTITY_PADDED_WINDOW,
            .serial_post_write_poll_ms = 1u,
            .command_stack_size = 32768u,
            .ble_start_attempts = 1u,
            .ble_advertising_mode = H2_LOADER_BLE_ADVERTISING_LEGACY,
        },
};

static h2_runtime_t* runtime;
static int commands_ready;

static void hold(void) {
  for (;;) rtos_delay_milliseconds(1000);
}

static void fail(const char* stage, int rc) {
  printf("H2_FRUIT_SLICE_SETUP_FAIL stage=%s rc=%d\n", stage, rc);
  fflush(stdout);
  /* Without App commands nothing can recover the device: reboot so the
   * unconfirmed trial returns to Loader. */
  if (!commands_ready) {
    rtos_delay_milliseconds(200u);
    (void)h2_pal_power_reboot(h2_device_app_commands_power(&h2_management), 0u);
  }
  hold();
}

static h2_pal_result_t confirm(void* user) {
  (void)user;
  const int rc = h2_device_app_confirm(&h2_management, runtime);
  printf("H2_FRUIT_SLICE_CONFIRM rc=%d\n", rc);
  fflush(stdout);
  return rc;
}

static void game(void* unused) {
  (void)unused;
  const h2_fruit_slice_config_t config = {.brightness_percent = 100u,
                                          .ready = confirm};
  const int rc = h2_fruit_slice_run(runtime, &config);
  /* The run only ends on failure; keep App commands up for recovery. */
  fail("game", rc);
}

static void entry(void* unused) {
  (void)unused;
  h2_runtime_config_t config;
  int rc = h2_bk7258_board_runtime_config(&config);
  if (rc) fail("board", rc);
  config.system_event = h2_pal_unsupported_system_event_api();
  rc = h2_runtime_init(&config, &runtime);
  if (rc) fail("runtime", rc);
  rc = h2_device_app_commands_start_serial_named(
      &h2_management, runtime, "fruit-slice", H2_LOADER_CAPABILITY_UART);
  if (rc) fail("commands", rc);
  commands_ready = 1;
  printf("H2_FRUIT_SLICE_BOOT board=bk7258_v3_202405\n");
  const h2_pal_task_options_t options = {.name = "fruit-slice/game",
                                         .min_stack_size = 32768u};
  h2_pal_task_t* task = NULL;
  rc = h2_pal_task_start(runtime->task, &options, game, NULL, &task);
  if (rc) fail("game-task", rc);
  hold();
}

int main(void) {
  int rc = h2_bk_target_task_policy_install();
  if (rc) return -1;
  bk_init();
  rc = h2_bk7258_board_start_entry_task("bk/fruit-slice", entry, NULL);
  if (rc) fail("entry", rc);
  return 0;
}
