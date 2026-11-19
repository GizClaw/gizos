#include "display_bench.h"
#include "esp_app_desc.h"
#include "h2_device_service.h"
#include "h2_esp_board.h"
#include "h2_esp_target_task_policy.h"

#include <stdio.h>

static h2_device_app_commands_t management;
static h2_runtime_t* runtime;

static void print_line(const char* line) {
  puts(line);
  fflush(stdout);
}

static int start(void) {
  int rc = h2_esp_target_task_policy_install();
  if (rc != H2_PAL_OK) return rc;
  h2_runtime_config_t config = {0};
  rc = h2_esp_board_runtime_config(&config);
  if (rc != H2_PAL_OK) return rc;
  const h2_device_app_commands_config_t commands = {
      .active_name = "display-bench",
      .hardware_capabilities = H2_LOADER_CAPABILITY_UART,
      .h2loader_partition_id = 1u,
      .coredump_partition_id = 3u,
      .identity_policy = H2_DEVICE_IDENTITY_NATIVE,
      .digest_yield_ms = 1u,
  };
  rc = h2_device_app_commands_prepare_serial(&management, &config, &commands,
                                             NULL);
  if (rc != H2_PAL_OK) return rc;
  rc = h2_runtime_init(&config, &runtime);
  if (rc != H2_PAL_OK) return rc;
  return h2_device_app_confirm(&management, runtime);
}

void app_main(void) {
  const int rc = start();
  printf("H2_DISPLAY_BENCH_CONFIRM rc=%d\n", rc);
  if (rc != H2_PAL_OK) return;
  const h2_display_bench_profile_t profile = {
      .board = "waveshare_esp32p4_wifi6_touch_lcd_4_3",
      .version = esp_app_get_description()->version,
      .bus = "MIPI-DSI",
      .panel_config =
          "panel=ST7701 mode=video lanes=2 lane_mbps=500 dpi_mhz=30 "
          "htotal=576 vtotal=870 wire_format=RGB565",
      .mode = H2_DISPLAY_BENCH_COPY,
      .display = runtime->display,
      .memory = h2_esp_board_psram_allocator(),
      .time = runtime->time,
      .line = print_line,
      .clock_tick_ns = 1000u,
  };
  (void)h2_display_bench_run(&profile);
}
