#include "h2_bk7258_board.h"
#include "h2_bk_h2loader.h"
#include "h2_smoke_ble_observer.h"
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

static void app_entry(void *user) {
    h2_runtime_config_t runtime_config;
    h2_runtime_t *runtime = NULL;
    int rc;

    (void)user;

    emergency_uart_write_string(0, "H2_BK_AP_BOOT image=ble-observer\r\n");
    os_printf("H2_BK_AP_BOOT image=ble-observer\r\n");
    rtos_delay_milliseconds(500);
    emit_marker("H2_BK_SMOKE_BLE_OBSERVER_STAGE stage=host_cli_owner_cp");
    rc = h2_bk7258_board_runtime_config(&runtime_config);
    if (rc != H2_PAL_OK) {
        emit_marker("H2_BK_SMOKE_BLE_OBSERVER_FAIL stage=runtime_config rc=%d", rc);
        (void)h2_bk_h2loader_reboot_to_loader();
        for (;;) {
            rtos_delay_milliseconds(1000);
        }
    }
    rc = h2_runtime_init(&runtime_config, &runtime);
    if (rc != H2_PAL_OK) {
        emit_marker("H2_BK_SMOKE_BLE_OBSERVER_FAIL stage=runtime_init rc=%d", rc);
        (void)h2_bk_h2loader_reboot_to_loader();
        for (;;) {
            rtos_delay_milliseconds(1000);
        }
    }
    if (rc == H2_PAL_OK) {
        rc = h2_bk_h2loader_start_app_iostreamikcp(
            runtime,
            "ble-observer");
    }
    if (rc != H2_PAL_OK) {
        emit_marker("H2_BK_SMOKE_BLE_OBSERVER_FAIL stage=app_cli rc=%d", rc);
        (void)h2_bk_h2loader_reboot_to_loader();
        for (;;) {
            rtos_delay_milliseconds(1000);
        }
    }
    rc = h2_bk_h2loader_start_app_ble(runtime, "ble-observer");
    if (rc != H2_PAL_OK) {
        emit_marker("H2_BK_SMOKE_BLE_OBSERVER_FAIL stage=app_ble rc=%d", rc);
        (void)h2_bk_h2loader_reboot_to_loader();
        for (;;) {
            rtos_delay_milliseconds(1000);
        }
    }

    emit_marker("H2_BK_SMOKE_BLE_OBSERVER_STAGE stage=run_begin");
    rc = h2_smoke_ble_observer_run(runtime);
    if (rc != H2_PAL_OK) {
        emit_marker("H2_BK_SMOKE_BLE_OBSERVER_FAIL stage=run rc=%d", rc);
    } else {
        emit_marker("H2_BK_SMOKE_BLE_OBSERVER_STAGE stage=run_done");
        rtos_delay_milliseconds(500);
        emit_marker("H2_BK_SMOKE_BLE_OBSERVER_STAGE stage=confirm_begin");
        rc = h2_bk_h2loader_confirm_current_app(runtime->pref);
        if (rc != H2_PAL_OK) {
            emit_marker("H2_BK_SMOKE_BLE_OBSERVER_FAIL stage=confirm rc=%d", rc);
        } else {
            emit_marker("H2_BK_SMOKE_BLE_OBSERVER_READY rc=0");
        }
    }
    for (;;) {
        rtos_delay_milliseconds(1000);
    }
}

int main(void) {
    if (h2_bk_layout_task_policy_install() != H2_PAL_OK) {
        return -1;
    }
    emergency_uart_write_string(0, "H2_BK_AP_MAIN image=ble-observer stage=before_bk_init\r\n");
    bk_init();
    emergency_uart_write_string(0, "H2_BK_AP_MAIN image=ble-observer stage=after_bk_init\r\n");
    h2_pal_result_t rc = h2_bk7258_board_start_entry_task(
        "bk/ble-observer", app_entry, NULL);
    if (rc != H2_PAL_OK) {
        emit_marker("H2_BK_BOARD_ENTRY_FAIL image=ble-observer rc=%d", rc);
    }
    return 0;
}
