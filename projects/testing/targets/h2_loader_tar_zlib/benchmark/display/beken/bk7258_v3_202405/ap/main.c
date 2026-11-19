#include "bk_private/bk_init.h"
#include "driver/aon_rtc.h"
#include "os/os.h"

#include "display_bench.h"
#include "h2_bk7258_board.h"
#include "h2_bk_target_task_policy.h"
#include "h2_device_service.h"
#include "h2_pal_unsupported.h"

#include <stdio.h>

static h2_runtime_t* runtime;
static h2_device_app_commands_t management = {
    .config =
        {
            .active_name = "display-bench",
            .hardware_capabilities = H2_LOADER_CAPABILITY_UART,
            .h2loader_partition_id = 1u,
            .app_partition_id = 2u,
            .coredump_partition_id = 0x7a0000u,
            .app_entry_path = "app/bk/app_ab_crc.rbl",
            .boot_policy_flags = H2_DEVICE_BOOT_PREPARE_SELECTION |
                                 H2_DEVICE_BOOT_PREPARE_APP_RESTART,
            .identity_policy = H2_DEVICE_IDENTITY_PADDED_WINDOW,
            .serial_post_write_poll_ms = 1u,
            .command_stack_size = 32768u,
        },
};

static void print_line(const char* line) {
  puts(line);
  fflush(stdout);
}

static int start(void) {
  h2_runtime_config_t config;
  int rc = h2_bk7258_board_runtime_config(&config);
  if (rc != H2_PAL_OK) return rc;
  config.system_event = h2_pal_unsupported_system_event_api();
  rc = h2_runtime_init(&config, &runtime);
  if (rc != H2_PAL_OK) return rc;
  rc = h2_device_app_commands_start_serial_named(
      &management, runtime, "display-bench", H2_LOADER_CAPABILITY_UART);
  if (rc != H2_PAL_OK) return rc;
  return h2_device_app_confirm(&management, runtime);
}

static void entry(void* user) {
  (void)user;
  const int rc = start();
  printf("H2_DISPLAY_BENCH_CONFIRM rc=%d\n", rc);
  if (rc == H2_PAL_OK) {
    const uint32_t rtc_hz = bk_rtc_get_clock_freq();
    const h2_display_bench_profile_t profile = {
        .board = "bk7258_v3_202405",
        .version = H2_BENCH_VERSION,
        .bus = "parallel-RGB",
        .panel_config =
            "panel=H050IWV mode=continuous pclk_mhz=30 "
            "htotal=896 vtotal=530 controller_out=RGB888",
        .mode = H2_DISPLAY_BENCH_SUBMIT,
        .display = runtime->display,
        .memory = h2_bk7258_board_psram_allocator(),
        .time = runtime->time,
        .line = print_line,
        .clock_tick_ns = rtc_hz != 0u ? 1000000000u / rtc_hz : 0u,
    };
    (void)h2_display_bench_run(&profile);
  } else {
    printf("H2_DISPLAY_BENCH_END stage=start rc=%d\n", rc);
  }
  for (;;) rtos_delay_milliseconds(1000u);
}

int main(void) {
  int rc = h2_bk_target_task_policy_install();
  if (rc != H2_PAL_OK) return -1;
  bk_init();
  rc = h2_bk7258_board_start_entry_task("bk/display-bench", entry, NULL);
  if (rc != H2_PAL_OK) printf("H2_DISPLAY_BENCH_END stage=entry rc=%d\n", rc);
  return rc == H2_PAL_OK ? 0 : -1;
}
