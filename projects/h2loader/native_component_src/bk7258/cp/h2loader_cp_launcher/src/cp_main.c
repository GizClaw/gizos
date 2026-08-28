#include "h2_bk_h2loader_cp_transport.h"
#include "h2_bk_target_task_policy.h"

#include "bk_private/bk_init.h"

#include "components/system.h"
#include "driver/pwr_clk.h"
#include "modules/pm.h"
#include "os/os.h"

#define H2_BK_CP_BOOTSTRAP_STACK_SIZE 1024u

static void h2_bk_serial_log_string(int port, const char *string) {
    (void)port;
    os_printf("%s", string);
}

#define emergency_uart_write_string h2_bk_serial_log_string

void _init(void) {}
void _fini(void) {}

static void h2loader_cp_entry(void *arg) {
    (void)arg;
    rtos_delay_milliseconds(500);
    emergency_uart_write_string(0, "H2_BK_CP_BOOT image=h2loader-managed\r\n");
    while (h2_bk_h2loader_cp_transport_start() != 0) {
        rtos_delay_milliseconds(100);
    }
    emergency_uart_write_string(
        0, "H2_BK_CP_TRANSPORT_READY status=ready\r\n");
    bk_pm_module_vote_boot_cp1_ctrl(PM_BOOT_CP1_MODULE_NAME_APP,
                                    PM_POWER_MODULE_STATE_ON);
    rtos_delete_thread(NULL);
}

int main(void) {
    if (h2_bk_target_task_policy_install() != H2_PAL_OK) {
        return -1;
    }
    if (rtos_create_thread(NULL, BEKEN_APPLICATION_PRIORITY,
                           "h2-cp-bootstrap", h2loader_cp_entry,
                           H2_BK_CP_BOOTSTRAP_STACK_SIZE, NULL) != kNoErr) {
        return -1;
    }
    return bk_init();
}
