#include "h2_bk_h2loader_cp_transport.h"
#include "h2_bk_target_task_policy.h"

#include "bk_private/bk_init.h"

#include "components/system.h"
#include "driver/pwr_clk.h"
#include "modules/pm.h"
#include "os/os.h"

extern void rtos_set_user_app_entry(beken_thread_function_t entry);

static void h2_bk_serial_log_string(int port, const char *string) {
    (void)port;
    os_printf("%s", string);
}

#define emergency_uart_write_string h2_bk_serial_log_string

void _init(void) {}
void _fini(void) {}

static void app_cp_entry(void *arg) {
    (void)arg;
    rtos_delay_milliseconds(500);
    bk_pm_module_vote_boot_cp1_ctrl(PM_BOOT_CP1_MODULE_NAME_APP,
                                    PM_POWER_MODULE_STATE_ON);
    emergency_uart_write_string(0, "H2_BK_CP_BOOT image=ble-observer\r\n");
    if (h2_bk_h2loader_cp_transport_start() != 0) {
        emergency_uart_write_string(0, "H2_BK_CP_TRANSPORT_READY status=fail\r\n");
        return;
    }
    emergency_uart_write_string(0, "H2_BK_CP_TRANSPORT_READY status=ready\r\n");
}

int main(void) {
    if (h2_bk_target_task_policy_install() != H2_PAL_OK) {
        return -1;
    }
    rtos_set_user_app_entry(app_cp_entry);
    return bk_init();
}
