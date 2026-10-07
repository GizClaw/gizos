#include "bk_private/bk_init.h"
#include "h2_atomic_static.h"
#include "h2_bk7258_board.h"
#include "h2_bk_h2loader.h"
#include "h2_bk_platform_core.h"
#include "h2_bk_target_task_policy.h"
#include "h2_iperf_client_app.h"
#include <common/sys_config.h>
#include <driver/uart.h>
#include <os/os.h>
#include <stdio.h>
#include <string.h>

static h2_runtime_t *runtime;
static const h2_pal_log_api_t *board_log;
static const h2_pal_uart_io_stream_api_t *console;
H2_ATOMIC_DEFINE_STATIC(bool, console_writable, false);
static int console_error;

static int record(const char *message) {
  if (!h2_atomic_bool_load(&console_writable, H2_ATOMIC_ACQUIRE))
    return H2_PAL_ERR_INVALID_STATE;
  char line[1024];
  size_t length = strlen(message);
  if (length == 0 || length > sizeof(line) - 2 || strchr(message, '\r') ||
      strchr(message, '\n'))
    return H2_PAL_ERR_INVALID_ARG;
  memcpy(line, message, length);
  line[length++] = '\r';
  line[length++] = '\n';
  size_t written = 0;
  int rc = h2_pal_uart_io_stream_write(console, line, length, &written, 5000u);
  if (rc == H2_PAL_OK && written != length)
    rc = H2_PAL_ERR_IO;
  if (rc != H2_PAL_OK && console_error == H2_PAL_OK)
    console_error = rc;
  return rc;
}
static int ledger_log(void *user, h2_pal_log_level_t level, const char *scope,
                      const char *message) {
  (void)user;
  if (h2_atomic_bool_load(&console_writable, H2_ATOMIC_ACQUIRE) && scope &&
      strncmp(scope, "iperf", 5u) == 0)
    return record(message);
  return h2_pal_log_write(board_log, level, scope, message);
}
static const h2_pal_log_vtable_t ledger_vtable = {.write = ledger_log};
static const h2_pal_log_api_t ledger_api = {.vtable = &ledger_vtable};
static void hold(void) {
  for (;;)
    rtos_delay_milliseconds(1000u);
}
static void fail(const char *stage, int rc) {
  char line[160];
  snprintf(line, sizeof(line),
           "H2_IPERF_CLIENT_FAIL board=bk7258 stage=%s rc=%d", stage, rc);
  if (h2_atomic_bool_load(&console_writable, H2_ATOMIC_ACQUIRE))
    (void)record(line);
  else {
    puts(line);
    fflush(stdout);
  }
  hold();
}
static void memory(void *user, const char *checkpoint) {
  (void)user;
  h2_bk_platform_resource_stats_t resources = {0};
  int rc = h2_bk_platform_get_resource_stats(&resources);
  char line[320];
  snprintf(
      line, sizeof(line),
      "H2_IPERF_CLIENT_MEMORY board=bk7258 checkpoint=%s internal_free=%u "
      "internal_min=%u psram_free=%u resource_rc=%d allocations=%u bytes=%u",
      checkpoint, (unsigned)rtos_get_free_heap_size(),
      (unsigned)rtos_get_minimum_free_heap_size(),
      (unsigned)rtos_get_psram_free_heap_size(), rc,
      (unsigned)resources.allocations, (unsigned)resources.allocation_bytes);
  (void)record(line);
}
static int start_commands(void) {
  return h2_bk_h2loader_start_app_iostreamikcp_with_capabilities(
      runtime, "iperf-client",
      H2_LOADER_CAPABILITY_UART | H2_LOADER_CAPABILITY_WIFI);
}
static void run(void *user) {
  (void)user;
  rtos_delay_milliseconds(5000u);
  int rc = h2_bk_h2loader_stop_app_iostreamikcp();
  if (rc != H2_PAL_OK)
    fail("command-stop", rc);
  console = h2_bk_platform_uart_io_stream_api();
  const h2_pal_uart_io_stream_config_t config = {
      .baud_rate = CONFIG_UART_PRINT_BAUD_RATE,
      .data_bits = 8u,
      .stop_bits = 1u,
      .parity = H2_PAL_UART_PARITY_NONE,
      .flow_control = H2_PAL_UART_FLOW_CONTROL_NONE,
      .rx_buffer_size = 8192u,
      .tx_buffer_size = 2048u};
  rc = h2_pal_uart_io_stream_configure(console, &config);
  if (rc == H2_PAL_OK) {
    h2_atomic_bool_store(&console_writable, true, H2_ATOMIC_RELEASE);
    (void)record(
        "H2_IPERF_CLIENT_PHASE board=bk7258 phase=public-network-begin");
    memory(NULL, "matrix-start");
    rc = h2_iperf_client_app_bench(runtime, "bk7258", memory, NULL);
    memory(NULL, "matrix-end");
    uint32_t started = rtos_get_time();
    while (!bk_uart_is_tx_over(CONFIG_UART_PRINT_PORT)) {
      if ((uint32_t)(rtos_get_time() - started) >= 5000u) {
        if (rc == H2_PAL_OK)
          rc = H2_PAL_ERR_TIMEOUT;
        break;
      }
      rtos_delay_milliseconds(1u);
    }
  }
  h2_atomic_bool_store(&console_writable, false, H2_ATOMIC_RELEASE);
  h2_bk_platform_uart_io_stream_deinit();
  int restored = start_commands();
  if (restored == H2_PAL_OK)
    h2_atomic_bool_store(&console_writable, true, H2_ATOMIC_RELEASE);
  if (rc == H2_PAL_OK && console_error != H2_PAL_OK)
    rc = console_error;
  if (rc == H2_PAL_OK && restored != H2_PAL_OK)
    rc = restored;
  if (rc != H2_PAL_OK)
    fail("bench-or-console", rc);
  (void)record("H2_IPERF_CLIENT_PHASE board=bk7258 phase=confirm-begin "
               "control_restored=1");
  rc = h2_bk_h2loader_confirm_current_app(runtime);
  char line[160];
  snprintf(line, sizeof(line), "H2_IPERF_CLIENT_CONFIRMED board=bk7258 rc=%d",
           rc);
  if (record(line) != H2_PAL_OK || rc != H2_PAL_OK)
    fail("confirm", rc);
  hold();
}
static void entry(void *user) {
  (void)user;
  h2_runtime_config_t config = {0};
  int rc = h2_bk7258_board_runtime_config(&config);
  if (rc != H2_PAL_OK)
    fail("board", rc);
  /* Bulk iperf receive buffers must leave internal SRAM to Wi-Fi/control RX. */
  config.mem = h2_bk_platform_psram_allocator();
  board_log = config.log;
  config.log = &ledger_api;
  rc = h2_runtime_init(&config, &runtime);
  if (rc != H2_PAL_OK)
    fail("runtime", rc);
  rc = start_commands();
  if (rc != H2_PAL_OK)
    fail("commands", rc);
  h2_pal_firmware_info_t image = {0};
  rc = h2_pal_firmware_info_get_current(runtime->firmware_info, &image);
  if (rc != H2_PAL_OK)
    fail("version", rc);
  printf("H2_IPERF_CLIENT_BOOT board=bk7258 version=%s\n", image.version);
  fflush(stdout);
  const h2_pal_task_options_t options = {.name = h2_iperf_client_app_task_name,
                                         .min_stack_size = 32768u};
  h2_pal_task_t *task = NULL;
  rc = h2_pal_task_start(runtime->task, &options, run, NULL, &task);
  if (rc != H2_PAL_OK)
    fail("runner", rc);
  hold();
}
int main(void) {
  int rc = h2_bk_target_task_policy_install();
  if (rc != H2_PAL_OK)
    return -1;
  bk_init();
  rc = h2_bk7258_board_start_entry_task("bk/iperf-client", entry, NULL);
  if (rc != H2_PAL_OK)
    fail("entry", rc);
  return 0;
}
