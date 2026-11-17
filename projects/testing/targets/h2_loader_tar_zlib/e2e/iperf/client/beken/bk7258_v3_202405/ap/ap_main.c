#include "bk_private/bk_init.h"
#include "h2_bk7258_board.h"
#include "h2_device_service.h"
#include "h2_bk_platform_core.h"
#include "h2_bk_target_task_policy.h"
#include "h2_iperf_client_app.h"
#include "h2_iperf_client_output.h"
#include <common/sys_config.h>
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
static h2_iperf_client_output_t output;

static void hold(void) {
  for (;;) rtos_delay_milliseconds(1000u);
}
static void fail(const char* stage, int rc) {
  char line[160];
  snprintf(line, sizeof(line),
           "H2_IPERF_CLIENT_FAIL board=bk7258 stage=%s rc=%d", stage, rc);
  if (output.active)
    (void)h2_iperf_client_output_record(&output, line);
  else {
    puts(line);
    fflush(stdout);
  }
  hold();
}
static int start_commands(void) {
  return h2_device_app_commands_start_serial_named(
      &h2_management, runtime, "iperf-client",
      H2_LOADER_CAPABILITY_UART | H2_LOADER_CAPABILITY_WIFI);
}
static void run(void* user) {
  (void)user;
  int rc = h2_pal_time_sleep_ms(runtime->time, 5000u);
  if (rc != H2_PAL_OK) fail("grace", rc);
  rc = h2_device_app_commands_stop_serial(&h2_management);
  if (rc != H2_PAL_OK) fail("command-stop", rc);
  const h2_pal_uart_io_stream_api_t* uart = h2_bk_platform_uart_io_stream_api();
  const h2_pal_uart_io_stream_config_t config = {
      .baud_rate = CONFIG_UART_PRINT_BAUD_RATE,
      .data_bits = 8u,
      .stop_bits = 1u,
      .parity = H2_PAL_UART_PARITY_NONE,
      .flow_control = H2_PAL_UART_FLOW_CONTROL_NONE,
      .rx_buffer_size = 8192u,
      .tx_buffer_size = 2048u};
  rc = h2_pal_uart_io_stream_configure(uart, &config);
  if (rc == H2_PAL_OK) {
    output.active = true;
    (void)h2_iperf_client_output_record(
        &output,
        "H2_IPERF_CLIENT_PHASE board=bk7258 phase=public-network-begin");
    h2_iperf_client_output_checkpoint(&output, "matrix-start");
    rc = h2_iperf_client_app_bench(runtime, "bk7258",
                                   h2_iperf_client_output_checkpoint, &output);
    h2_iperf_client_output_checkpoint(&output, "matrix-end");
    int flushed = h2_pal_io_stream_flush(runtime->io_stream);
    if (rc == H2_PAL_OK) rc = flushed;
  }
  output.active = false;
  int released;
  /* Release retains partial progress on failure. Keep retrying that owner;
   * management may attach only after release completes, and the first cleanup
   * error still prevents this execution from qualifying. */
  while ((released = h2_pal_io_stream_release(runtime->io_stream)) !=
         H2_PAL_OK) {
    if (rc == H2_PAL_OK) rc = released;
    printf("H2_IPERF_CLIENT_RECOVERY stage=uart_release rc=%d\n", released);
    const int slept = h2_pal_time_sleep_ms(runtime->time, 100u);
    if (slept != H2_PAL_OK) rtos_delay_milliseconds(100u);
  }
  int restored;
  while ((restored = start_commands()) != H2_PAL_OK) {
    if (rc == H2_PAL_OK) rc = restored;
    printf("H2_IPERF_CLIENT_RECOVERY stage=management_start rc=%d\n", restored);
    /* A failed start can retain a partial stream/session. Quiesce that owner
     * before retrying startup, rather than racing a retained release. */
    while (h2_device_app_commands_stop_serial(&h2_management) != H2_PAL_OK)
      rtos_delay_milliseconds(100u);
    rtos_delay_milliseconds(100u);
  }
  output.active = true;
  if (rc == H2_PAL_OK && output.error != H2_PAL_OK) rc = output.error;
  if (rc == H2_PAL_OK && restored != H2_PAL_OK) rc = restored;
  if (rc != H2_PAL_OK) fail("bench-or-console", rc);
  rc = h2_iperf_client_output_record(
      &output,
      "H2_IPERF_CLIENT_PHASE board=bk7258 phase=confirm-begin "
      "control_restored=1");
  if (rc != H2_PAL_OK) fail("confirm-log", rc);
  rc = h2_device_app_confirm(&h2_management, runtime);
  char line[160];
  snprintf(line, sizeof(line), "H2_IPERF_CLIENT_CONFIRMED board=bk7258 rc=%d",
           rc);
  int logged = h2_iperf_client_output_record(&output, line);
  if (rc == H2_PAL_OK) rc = logged;
  if (rc != H2_PAL_OK) fail("confirm", rc);
  hold();
}
static void entry(void* user) {
  (void)user;
  h2_runtime_config_t config = {0};
  int rc = h2_bk7258_board_runtime_config(&config);
  if (rc != H2_PAL_OK) fail("board", rc);
  config.mem = h2_bk_platform_psram_allocator();
  h2_iperf_client_output_init(&output, &config, "bk7258");
  config.log = &output.log;
  rc = h2_runtime_init(&config, &runtime);
  if (rc != H2_PAL_OK) {
    (void)h2_bk7258_board_runtime_deinit();
    fail("runtime", rc);
  }
  rc = start_commands();
  if (rc != H2_PAL_OK) fail("commands", rc);
  h2_pal_firmware_info_t image = {0};
  rc = h2_pal_firmware_info_get_current(runtime->firmware_info, &image);
  if (rc != H2_PAL_OK) fail("version", rc);
  printf("H2_IPERF_CLIENT_BOOT board=bk7258 version=%s\n", image.version);
  fflush(stdout);
  const h2_pal_task_options_t options = {.name = h2_iperf_client_app_task_name,
                                         .min_stack_size = 32768u};
  h2_pal_task_t* task = NULL;
  rc = h2_pal_task_start(runtime->task, &options, run, NULL, &task);
  if (rc != H2_PAL_OK) fail("runner", rc);
  hold();
}
int main(void) {
  int rc = h2_bk_target_task_policy_install();
  if (rc != H2_PAL_OK) return -1;
  bk_init();
  rc = h2_bk7258_board_start_entry_task("bk/iperf-client", entry, NULL);
  if (rc != H2_PAL_OK) fail("entry", rc);
  return 0;
}
