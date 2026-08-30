#include "h2_bk7258_board.h"
#include "h2_bk_h2loader.h"
#include "h2_bleikcp_speed.h"
#include "h2_bk_target_task_policy.h"

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
    char line[160];
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    emergency_uart_write_string(0, line);
    emergency_uart_write_string(0, "\r\n");
    os_printf("%s\r\n", line);
}

static int pause_management_advertising(void *user) {
    (void)user;
    return h2_bk_h2loader_pause_app_ble_advertising();
}

static int resume_management_advertising(void *user) {
    (void)user;
    return h2_bk_h2loader_resume_app_ble_advertising();
}

static int confirm_ready(void *user) {
    h2_runtime_t *runtime = user;
    int rc = h2_bk_h2loader_confirm_current_app(runtime);
    emit_marker("H2_BK_SPEED_STAGE stage=confirm rc=%d", rc);
    return rc;
}

static void app_entry(void *user) {
    (void)user;
    emit_marker("H2_BK_SPEED_STAGE stage=entry");
    rtos_delay_milliseconds(500);
    h2_runtime_config_t config;
    h2_runtime_t *runtime = NULL;
    int rc = h2_bk7258_board_runtime_config(&config);
    emit_marker("H2_BK_SPEED_STAGE stage=runtime_config rc=%d", rc);
    if (rc == H2_PAL_OK) {
        config.mem = h2_bk7258_board_psram_allocator();
        rc = h2_runtime_init(&config, &runtime);
        emit_marker("H2_BK_SPEED_STAGE stage=runtime_init rc=%d", rc);
    }
    if (rc == H2_PAL_OK) {
        rc = h2_bk_h2loader_start_app_iostreamikcp(
            runtime, "bleikcp-speed-client");
        emit_marker("H2_BK_SPEED_STAGE stage=app_cli rc=%d", rc);
    }
    if (rc == H2_PAL_OK) {
        rc = h2_bk_h2loader_start_app_ble(runtime, "bleikcp-speed-client");
        emit_marker("H2_BK_SPEED_STAGE stage=app_ble rc=%d", rc);
    }
    if (rc == H2_PAL_OK) {
        const h2_bleikcp_speed_config_t speed_config = {
            .role = H2_BLEIKCP_SPEED_ROLE_CLIENT,
            .advertising_type = H2_PAL_BLE_ADV_TYPE_EXTENDED,
            .scan_type = H2_PAL_BLE_SCAN_TYPE_EXTENDED,
            .pause_management_advertising = pause_management_advertising,
            .resume_management_advertising = resume_management_advertising,
            .ready = confirm_ready,
            .ready_user = runtime,
        };
        rc = h2_bleikcp_speed_run(runtime, &speed_config);
    }
    emit_marker("H2_BLEIKCP_SPEED_FAIL board=bk7258_v3_202405 role=client rc=%d", rc);
}

int main(void) {
    if (h2_bk_target_task_policy_install() != H2_PAL_OK) {
        return -1;
    }
    emergency_uart_write_string(0, "H2_BK_AP_MAIN image=bleikcp-speed-client\r\n");
    bk_init();
    emergency_uart_write_string(0, "H2_BK_AP_MAIN image=bleikcp-speed-client stage=after_bk_init\r\n");
    int rc = h2_bk7258_board_start_entry_task(
        "bk/bleikcp-speed-client", app_entry, NULL);
    if (rc != H2_PAL_OK) {
        os_printf("H2_BK_BOARD_ENTRY_FAIL image=bleikcp-speed-client rc=%d\r\n", rc);
    }
    return 0;
}
