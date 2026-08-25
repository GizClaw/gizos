#include "h2_bk7258_board.h"
#include "h2_bk_h2loader.h"
#include "h2_libco_smoke.h"
#include "h2_bk_layout_task_policy.h"

#include "bk_private/bk_init.h"
#include "os/os.h"

#include <stdarg.h>
#include <stdio.h>

static void h2_bk_serial_log_string(int port, const char *string) {
  (void)port;
  os_printf("%s", string);
}

#define emergency_uart_write_string h2_bk_serial_log_string

static void h2_bk_libco_smoke_emit_marker(const char *fmt, ...) {
  char line[128];
  va_list ap;

  va_start(ap, fmt);
  (void)vsnprintf(line, sizeof(line), fmt, ap);
  va_end(ap);
  emergency_uart_write_string(0, line);
  emergency_uart_write_string(0, "\r\n");
}

static void h2_bk_libco_smoke_log_marker(h2_runtime_t *runtime,
                                         h2_pal_log_level_t level,
                                         const char *fmt, ...) {
  char line[128];
  va_list ap;

  va_start(ap, fmt);
  (void)vsnprintf(line, sizeof(line), fmt, ap);
  va_end(ap);
  if (h2_pal_log_write(runtime->log, level, "libco-smoke", line) != H2_PAL_OK) {
    h2_bk_libco_smoke_emit_marker("%s", line);
  }
}

static void h2_bk_libco_smoke_wait_forever(void) {
  for (;;) {
    rtos_delay_milliseconds(1000);
  }
}

static void
h2_bk_libco_smoke_recover_before_command_transport(const char *stage, int rc) {
  h2_bk_libco_smoke_emit_marker("H2_BK_LIBCO_SMOKE_FAIL stage=%s rc=%d", stage,
                                rc);
  (void)h2_bk_h2loader_reboot_to_loader();
  h2_bk_libco_smoke_wait_forever();
}

static void h2_bk_libco_smoke_app_entry(void *user) {
  h2_runtime_config_t runtime_config;
  h2_runtime_t *runtime = NULL;
  const h2_libco_smoke_config_t smoke_config = {
      .task_stack_size = H2_LIBCO_SMOKE_DEFAULT_STACK_SIZE,
      .switch_iterations = H2_LIBCO_SMOKE_DEFAULT_SWITCH_ITERATIONS,
  };
  int rc;

  (void)user;
  h2_bk_libco_smoke_emit_marker(
      "H2_BK_AP_BOOT image=libco-smoke stack=%u iterations=%u",
      (unsigned int)smoke_config.task_stack_size,
      (unsigned int)smoke_config.switch_iterations);
  rtos_delay_milliseconds(500);

  rc = h2_bk7258_board_runtime_config(&runtime_config);
  if (rc != H2_PAL_OK) {
    h2_bk_libco_smoke_recover_before_command_transport("runtime_config", rc);
  }
  rc = h2_runtime_init(&runtime_config, &runtime);
  if (rc != H2_PAL_OK) {
    h2_bk_libco_smoke_recover_before_command_transport("runtime_init", rc);
  }
  if (rc == H2_PAL_OK) {
    rc = h2_bk_h2loader_start_app_iostreamikcp(runtime, "libco-smoke");
  }
  if (rc != H2_PAL_OK) {
    h2_bk_libco_smoke_recover_before_command_transport("app_cli", rc);
  }
  /* Let the UART command owner finish publishing before smoke log traffic. */
  rtos_delay_milliseconds(500);
  rc = h2_libco_smoke_run(runtime, &smoke_config);
  if (rc != H2_PAL_OK) {
    h2_bk_libco_smoke_log_marker(runtime, H2_PAL_LOG_ERROR,
                                 "H2_BK_LIBCO_SMOKE_FAIL stage=portable rc=%d",
                                 rc);
    h2_bk_libco_smoke_wait_forever();
  }
  rc = h2_bk_h2loader_confirm_current_app(runtime->pref);
  if (rc != H2_PAL_OK) {
    h2_bk_libco_smoke_log_marker(runtime, H2_PAL_LOG_ERROR,
                                 "H2_BK_LIBCO_SMOKE_FAIL stage=confirm rc=%d",
                                 rc);
    h2_bk_libco_smoke_wait_forever();
  }
  h2_bk_libco_smoke_log_marker(runtime, H2_PAL_LOG_INFO,
                               "H2_BK_LIBCO_SMOKE_READY rc=0");
  h2_bk_libco_smoke_wait_forever();
}

int main(void) {
    if (h2_bk_layout_task_policy_install() != H2_PAL_OK) {
        return -1;
    }
  emergency_uart_write_string(
      0, "H2_BK_AP_MAIN image=libco-smoke stage=before_bk_init\r\n");
  bk_init();
  emergency_uart_write_string(
      0, "H2_BK_AP_MAIN image=libco-smoke stage=after_bk_init\r\n");
  h2_pal_result_t rc = h2_bk7258_board_start_entry_task(
      "bk/libco-smoke", h2_bk_libco_smoke_app_entry, NULL);
  if (rc != H2_PAL_OK) {
    h2_bk_libco_smoke_emit_marker(
        "H2_BK_BOARD_ENTRY_FAIL image=libco-smoke rc=%d", rc);
  }
  return 0;
}
