/* Mosaico App image: Runtime lifetime, H2Loader App commands over the Type-C
 * console, and the game task. The game confirms the trial boot once its first
 * frame is visible with Touch open. */
#include "esp_app_desc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "h2_device_service.h"
#include "h2_esp_board.h"
#include "h2_esp_target_task_policy.h"
#include "h2_fruit_slice.h"
#include "h2_mosaico_usb_console.h"

#include <stdio.h>

static h2_device_app_commands_t h2_management;
static h2_runtime_t* runtime;

static void hold(void) {
  for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
}

static void fail(const char* stage, int rc) {
  printf("H2_FRUIT_SLICE_SETUP_FAIL stage=%s rc=%d\n", stage, rc);
  fflush(stdout);
  hold();
}

static h2_pal_result_t confirm(void* user) {
  (void)user;
  const int rc = h2_device_app_confirm(&h2_management, runtime);
  printf("H2_FRUIT_SLICE_CONFIRM rc=%d\n", rc);
  fflush(stdout);
  return rc;
}

static void game(void* user) {
  (void)user;
  const h2_fruit_slice_config_t config = {.brightness_percent = 90u,
                                          .ready = confirm};
  const int rc = h2_fruit_slice_run(runtime, &config);
  /* The run only ends on failure; keep App commands up for recovery. */
  fail("game", rc);
}

void app_main(void) {
  int rc = h2_esp_target_task_policy_install();
  if (rc) fail("task-policy", rc);
  rc = h2_mosaico_usb_console_init();
  if (rc) fail("usb-console", rc);
  h2_runtime_config_t config = {0};
  rc = h2_esp_board_runtime_config(&config);
  if (rc) fail("board", rc);
  const h2_device_app_commands_config_t commands = {
      .active_name = "fruit-slice",
      .hardware_capabilities = H2_LOADER_CAPABILITY_UART,
      .h2loader_partition_id = 1u,
      .coredump_partition_id = 3u,
      .identity_policy = H2_DEVICE_IDENTITY_NATIVE,
      .digest_yield_ms = 1u};
  rc = h2_device_app_commands_prepare_serial(&h2_management, &config, &commands,
                                             NULL);
  if (rc) fail("commands", rc);
  rc = h2_runtime_init(&config, &runtime);
  if (rc) fail("runtime", rc);
  printf("H2_FRUIT_SLICE_BOOT version=%s board=esp_mosaico\n",
         esp_app_get_description()->version);
  fflush(stdout);
  const h2_pal_task_options_t options = {.name = "fruit-slice/game",
                                         .min_stack_size = 32768u};
  h2_pal_task_t* task = NULL;
  rc = h2_pal_task_start(runtime->task, &options, game, NULL, &task);
  if (rc) fail("game-task", rc);
}
