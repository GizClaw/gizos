#include "h2_bk7258_board.h"
#include "h2_bk_h2loader.h"
#include "h2_smoke_wifi_csi.h"
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

static void emit_marker(const char *fmt, ...) {
  char line[128];
  va_list ap;

  va_start(ap, fmt);
  vsnprintf(line, sizeof(line), fmt, ap);
  va_end(ap);
  emergency_uart_write_string(0, line);
  emergency_uart_write_string(0, "\r\n");
  os_printf("%s\r\n", line);
}

static void smoke_ready(void *user, int rc) {
  h2_runtime_t *runtime = user;
  if (rc != H2_DISPLAY_OK) {
    emit_marker("H2_BK_SMOKE_WIFI_CSI_FAIL stage=initial_render rc=%d", rc);
    return;
  }
  emit_marker("H2_BK_SMOKE_WIFI_CSI_STAGE stage=dashboard_ready");
  rc = h2_bk_h2loader_confirm_current_app(runtime->pref);
  if (rc != H2_PAL_OK) {
    emit_marker("H2_BK_SMOKE_WIFI_CSI_FAIL stage=confirm rc=%d", rc);
  } else {
    emit_marker("H2_BK_SMOKE_WIFI_CSI_READY rc=0");
  }
}

static void app_entry(void *user) {
  h2_runtime_config_t runtime_config;
  h2_runtime_t *runtime = NULL;
  int rc;

  (void)user;

  emergency_uart_write_string(0, "H2_BK_AP_BOOT image=wifi-csi\r\n");
  os_printf("H2_BK_AP_BOOT image=wifi-csi\r\n");
  rtos_delay_milliseconds(500);
  emit_marker("H2_BK_SMOKE_WIFI_CSI_STAGE stage=host_cli_owner_cp");
  rc = h2_bk7258_board_runtime_config(&runtime_config);
  if (rc != H2_PAL_OK) {
    emit_marker("H2_BK_SMOKE_WIFI_CSI_FAIL stage=runtime_config rc=%d", rc);
    (void)h2_bk_h2loader_reboot_to_loader();
    for (;;) {
      rtos_delay_milliseconds(1000);
    }
  }
  rc = h2_runtime_init(&runtime_config, &runtime);
  if (rc != H2_PAL_OK) {
    emit_marker("H2_BK_SMOKE_WIFI_CSI_FAIL stage=runtime_init rc=%d", rc);
    (void)h2_bk_h2loader_reboot_to_loader();
    for (;;) {
      rtos_delay_milliseconds(1000);
    }
  }
  if (rc == H2_PAL_OK) {
    rc = h2_bk_h2loader_start_app_iostreamikcp(runtime, "wifi-csi");
  }
  if (rc != H2_PAL_OK) {
    emit_marker("H2_BK_SMOKE_WIFI_CSI_FAIL stage=app_cli rc=%d", rc);
    (void)h2_bk_h2loader_reboot_to_loader();
    for (;;) {
      rtos_delay_milliseconds(1000);
    }
  }

  emit_marker("H2_BK_SMOKE_WIFI_CSI_STAGE stage=run_begin");
  h2_smoke_wifi_csi_config_t smoke_config = {
      .user = runtime,
      .ready = smoke_ready,
  };
  rc = h2_smoke_wifi_csi_run(runtime, &smoke_config);
  emit_marker("H2_BK_SMOKE_WIFI_CSI_EXIT rc=%d", rc);
  for (;;) {
    rtos_delay_milliseconds(1000);
  }
}

int main(void) {
    if (h2_bk_layout_task_policy_install() != H2_PAL_OK) {
        return -1;
    }
  emergency_uart_write_string(
      0, "H2_BK_AP_MAIN image=wifi-csi stage=before_bk_init\r\n");
  bk_init();
  emergency_uart_write_string(
      0, "H2_BK_AP_MAIN image=wifi-csi stage=after_bk_init\r\n");
  h2_pal_result_t rc =
      h2_bk7258_board_start_entry_task("bk/wifi-csi", app_entry, NULL);
  if (rc != H2_PAL_OK) {
    emit_marker("H2_BK_BOARD_ENTRY_FAIL image=wifi-csi rc=%d", rc);
  }
  return 0;
}
