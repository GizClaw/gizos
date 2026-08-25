#include "h2_bk_h2loader.h"
#include "h2_bk7258_board.h"
#include "h2_crash_before_confirm.h"
#include "h2_bk_layout_task_policy.h"

#include "bk_private/bk_init.h"
#include "os/os.h"

#include <stdio.h>

static void h2_bk_serial_log_string(int port, const char *string) {
    (void)port;
    os_printf("%s", string);
}

#define emergency_uart_write_string h2_bk_serial_log_string

static void crash_now(void *user) {
    (void)user;
    h2_bk_h2loader_abort_for_crash_test();
}

static void app_entry(void *user) {
    h2_runtime_config_t runtime_config;
    h2_runtime_t *runtime = NULL;
    (void)user;
    emergency_uart_write_string(0, "H2_BK_AP_BOOT image=crash-before-confirm\r\n");
    os_printf("H2_BK_AP_BOOT image=crash-before-confirm\r\n");
    rtos_delay_milliseconds(500);
    int rc = h2_bk7258_board_runtime_config(&runtime_config);
    if (rc == H2_PAL_OK) {
        rc = h2_runtime_init(&runtime_config, &runtime);
    }
    if (rc != H2_PAL_OK) {
        (void)h2_bk_h2loader_reboot_to_loader();
        for (;;) {
            rtos_delay_milliseconds(1000);
        }
    }
    if (rc == H2_PAL_OK) {
        rc = h2_bk_h2loader_start_app_iostreamikcp(
            runtime,
            "crash-before-confirm");
    }
    char line[80];
    snprintf(line, sizeof(line), "H2_BK_CRASH_TEST_APP_CLI rc=%d\r\n", rc);
    emergency_uart_write_string(0, line);
    os_printf("%s", line);
    if (rc != H2_PAL_OK) {
        (void)h2_bk_h2loader_reboot_to_loader();
        for (;;) {
            rtos_delay_milliseconds(1000);
        }
    }
    rc = h2_bk_h2loader_start_app_ble(
        runtime,
        "crash-before-confirm");
    snprintf(line, sizeof(line), "H2_BK_CRASH_TEST_APP_BLE rc=%d\r\n", rc);
    emergency_uart_write_string(0, line);
    os_printf("%s", line);
    if (rc != H2_PAL_OK) {
        (void)h2_bk_h2loader_reboot_to_loader();
        for (;;) {
            rtos_delay_milliseconds(1000);
        }
    }
    const h2_crash_before_confirm_config_t config = {
        .crash = crash_now,
    };
    (void)h2_crash_before_confirm_run(runtime, &config);
    for (;;) {
        rtos_delay_milliseconds(1000);
    }
}

int main(void) {
    if (h2_bk_layout_task_policy_install() != H2_PAL_OK) {
        return -1;
    }
    emergency_uart_write_string(0, "H2_BK_AP_MAIN image=crash-before-confirm stage=before_bk_init\r\n");
    bk_init();
    emergency_uart_write_string(0, "H2_BK_AP_MAIN image=crash-before-confirm stage=after_bk_init\r\n");
    h2_pal_result_t rc = h2_bk7258_board_start_entry_task(
        "bk/crash-before-confirm", app_entry, NULL);
    if (rc != H2_PAL_OK) {
        char line[96];
        (void)snprintf(
            line,
            sizeof(line),
            "H2_BK_BOARD_ENTRY_FAIL image=crash-before-confirm rc=%d\r\n",
            rc);
        emergency_uart_write_string(0, line);
        os_printf("%s", line);
    }
    return 0;
}
