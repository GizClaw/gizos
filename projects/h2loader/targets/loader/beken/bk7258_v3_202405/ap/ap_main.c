#include "h2_bk7258_board.h"
#include "h2_device_service.h"
#include "h2loader_board_extension.h"
#include "h2/pal/core/h2_pal_errors.h"
#include "h2_bk_target_task_policy.h"

#include "bk_private/bk_init.h"
#include "components/system.h"
#include "os/os.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static h2_device_loader_t s_loader;
static h2_runtime_t* s_runtime;
static void h2_bk_serial_log_string(int port, const char* string) {
  (void)port;
  printf("%s", string);
}
#define emergency_uart_write_string h2_bk_serial_log_string
static void wait_forever(void) {
  for (;;) rtos_delay_milliseconds(1000);
}

__attribute__((weak)) int h2loader_board_extend_runtime(
    h2_runtime_config_t* config) {
  (void)config;
  return H2_PAL_OK;
}

__attribute__((weak)) int h2loader_board_extend_app(
    h2_runtime_t* runtime, h2loader_app_config_t* config) {
  (void)runtime;
  (void)config;
  return H2_PAL_OK;
}

static void h2loader_ap_entry(void* user) {
  h2_runtime_config_t runtime_config;

  (void)user;

  emergency_uart_write_string(0, "H2_BK_AP_ENTRY_EMERG image=h2loader\r\n");
  rtos_delay_milliseconds(500);
  emergency_uart_write_string(0, "H2_BK_AP_BOOT image=h2loader\r\n");

  emergency_uart_write_string(
      0, "H2_BK_H2LOADER_STEP stage=runtime_config begin\r\n");
  int rc = h2_bk7258_board_recovery_runtime_config(&runtime_config);
  if (rc == H2_PAL_OK) rc = h2loader_board_extend_runtime(&runtime_config);
  emergency_uart_write_string(
      0, "H2_BK_H2LOADER_STEP stage=runtime_config end\r\n");
  if (rc == H2_PAL_OK) {
    emergency_uart_write_string(
        0, "H2_BK_H2LOADER_STEP stage=runtime_init begin\r\n");
    rc = h2_runtime_init(&runtime_config, &s_runtime);
    emergency_uart_write_string(
        0, "H2_BK_H2LOADER_STEP stage=runtime_init end\r\n");
  }
  if (rc != H2_PAL_OK) {
    printf("H2_BK_H2LOADER_STARTUP_ERROR stage=runtime_init rc=%d\r\n", rc);
    wait_forever();
  }

  const h2_device_loader_config_t loader_config = {
      .board = s_runtime->board,
      .app_entry_path = "app/bk/app_ab_crc.rbl",
      .loader_partition_id = 1u,
      .app_partition_id = 2u,
      .coredump_partition_id = 0x7a0000u,
      .hardware_capabilities =
          H2_LOADER_CAPABILITY_UART | H2_LOADER_CAPABILITY_BLE,
      .boot_policy_flags = H2_DEVICE_BOOT_PREPARE_SELECTION,
      .identity_policy = H2_DEVICE_IDENTITY_PADDED_WINDOW,
      .serial_post_write_poll_ms = 1u,
      .ble_advertising_mode = H2_LOADER_BLE_ADVERTISING_LEGACY,
      .extend_app = h2loader_board_extend_app,
  };
  rc = h2_device_loader_run(&s_loader, s_runtime, &loader_config, NULL, 1);
  printf("H2_BK_H2LOADER_STARTUP_ERROR stage=loader_exit rc=%d\r\n", rc);
  wait_forever();
}

int main(void) {
  /* Install before bk_init() and before the entry task below, which takes
   * its bk/h2loader row from this policy. */
  h2_pal_result_t rc = h2_bk_target_task_policy_install();
  if (rc != H2_PAL_OK) {
    return -1;
  }
  emergency_uart_write_string(0,
                              "H2_BK_AP_MAIN_EMERG stage=before_bk_init\r\n");
  os_printf("H2_BK_AP_MAIN stage=before_bk_init\r\n");
  bk_init();
  emergency_uart_write_string(0, "H2_BK_AP_MAIN_EMERG stage=after_bk_init\r\n");
  os_printf("H2_BK_AP_MAIN stage=after_bk_init\r\n");
  rc = h2_bk7258_board_start_entry_task("bk/h2loader", h2loader_ap_entry, NULL);
  if (rc != H2_PAL_OK) {
    char line[96];
    (void)snprintf(line, sizeof(line),
                   "H2_BK_BOARD_ENTRY_FAIL image=h2loader rc=%d\r\n", rc);
    emergency_uart_write_string(0, line);
    os_printf("%s", line);
  }
  return 0;
}
