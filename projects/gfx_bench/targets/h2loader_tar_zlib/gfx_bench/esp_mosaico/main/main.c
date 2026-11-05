#include "h2_gfx_bench.h"

#include "h2_esp_board.h"
#include "h2_esp_h2loader_ble.h"
#include "h2_esp_h2loader_runtime.h"
#include "h2_esp_target_task_policy.h"
#include "h2_mosaico_loader_usb.h"
#include "h2_mosaico_usb_console.h"
#include "h2_mosaico_usb_transport.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdio.h>

/* Serial recovery borrows the config and strings for the entire boot. */
static h2_runtime_config_t runtime_config;
static h2_runtime_t* runtime;
static h2_pal_task_t* runner_task;
static int serial_prepared;
static const char active_name[] = "gfx-bench";
static const h2_esp_h2loader_app_commands_config_t commands_config = {
    .active_name = active_name,
    .hardware_capabilities = H2_LOADER_CAPABILITY_UART,
    .h2loader_partition_id = 1u,
    .coredump_partition_id = 3u,
};

static void hold(void) {
  fflush(stdout);
  for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
}

static void fail(const char* stage, int rc) {
  printf("H2_GFX_BENCH_FAIL board=esp_mosaico stage=%s rc=%d\n", stage, rc);
  for (;;) {
    if (!serial_prepared) {
      unsigned char ignored[64];
      size_t count = 0;
      (void)h2_mosaico_usb_read(NULL, ignored, sizeof(ignored), &count, 10);
    }
    printf("H2_GFX_BENCH_FAIL board=esp_mosaico stage=%s rc=%d\n", stage, rc);
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

/* The App invokes this only after its first completed display frame. */
static int ready(void* user) {
  int rc = h2_esp_h2loader_app_confirm(user);
  printf("H2_GFX_BENCH_READY board=esp_mosaico confirm=%d\n", rc);
  fflush(stdout);
  return rc;
}

static h2_gfx_bench_config_t app_config = {.ready = ready};

static void run(void* unused) {
  (void)unused;
  printf("H2_GFX_INIT stage=app-enter\n");
  int rc = h2_gfx_bench_run(runtime, &app_config);
  printf("H2_GFX_BENCH_RETURN board=esp_mosaico rc=%d\n", rc);
  /* Backends and recovery are boot-long singletons. Even a failed App cleanup
   * may retain provider borrows, so keep Runtime and the board alive. */
  hold();
}

void app_main(void) {
  int rc = h2_esp_target_task_policy_install();
  if (rc != H2_PAL_OK) fail("task-policy", rc);
  rc = h2_mosaico_usb_console_init();
  if (rc != H2_PAL_OK) fail("usb-console", rc);
  rc = h2_mosaico_loader_usb_init();
  if (rc != H2_PAL_OK) fail("usb-transport", rc);
  /* Local bring-up window: prime the USB stream before recovery owns RX,
   * so board-init failures cannot silence their own diagnostics. Host may send
   * GFX_BOOT to continue immediately; otherwise continue after 20 seconds. */
  for (unsigned i = 0; i < 200; ++i) {
    unsigned char input[64];
    size_t count = 0;
    const int read_rc =
        h2_mosaico_usb_read(NULL, input, sizeof(input), &count, 10);
    if (read_rc == H2_PAL_OK && count) break;
    if (i % 10 == 0) printf("H2_GFX_INIT stage=usb-wait\n");
    vTaskDelay(pdMS_TO_TICKS(90));
  }
  printf("H2_GFX_INIT stage=board-enter\n");
  rc = h2_esp_board_runtime_config(&runtime_config);
  if (rc != H2_PAL_OK) fail("board", rc);
  printf("H2_GFX_INIT stage=board-ready\n");
  runtime_config.system_event = h2_pal_unsupported_system_event_api();
  rc = h2_esp_h2loader_app_commands_prepare_serial_with_config(
      &runtime_config, &commands_config);
  if (rc != H2_PAL_OK) fail("serial-recovery", rc);
  serial_prepared = 1;
  printf("H2_GFX_INIT stage=runtime-enter\n");
  rc = h2_runtime_init(&runtime_config, &runtime);
  if (rc != H2_PAL_OK) fail("runtime", rc);

  printf("H2_GFX_INIT stage=runtime-ready\n");
  /* The App reads raw Touch PAL; leave Runtime input polling stopped. */
  app_config.user = runtime;
  const h2_pal_task_options_t options = {
      .name = "gfx/runner",
      .min_stack_size = 65536u,
  };
  rc = h2_pal_task_start(runtime->task, &options, run, NULL, &runner_task);
  if (rc != H2_PAL_OK) fail("runner", rc);
}
