#include "h2_bk_h2loader_cp_transport.h"
#include "h2_bk_target_task_policy.h"

#include "bk_private/bk_init.h"

#include "components/system.h"
#include "driver/pwr_clk.h"
#include "modules/pm.h"
#include "os/os.h"

static beken_semaphore_t s_bk_init_done;
static int s_bk_init_result;

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
    if (rtos_get_semaphore(&s_bk_init_done, BEKEN_WAIT_FOREVER) != kNoErr) {
        emergency_uart_write_string(
            0, "H2_BK_CP_TRANSPORT_READY status=fail reason=bk_init_sync\r\n");
        return;
    }
    (void)rtos_deinit_semaphore(&s_bk_init_done);
    if (s_bk_init_result != 0) {
        emergency_uart_write_string(
            0, "H2_BK_CP_TRANSPORT_READY status=fail reason=bk_init\r\n");
        return;
    }
    emergency_uart_write_string(0, "H2_BK_CP_BOOT image=display\r\n");
    if (h2_bk_h2loader_cp_transport_start() != 0) {
        emergency_uart_write_string(0, "H2_BK_CP_TRANSPORT_READY status=fail\r\n");
        return;
    }
    emergency_uart_write_string(0, "H2_BK_CP_TRANSPORT_READY status=ready\r\n");
}

static void h2_bk_cp_start_task(void *arg) {
    app_cp_entry(arg);
    rtos_delete_thread(NULL);
}

int main(void) {
    if (h2_bk_target_task_policy_install() != H2_PAL_OK) {
        return -1;
    }
    if (rtos_init_semaphore(&s_bk_init_done, 1) != kNoErr) {
        return -1;
    }
    if (rtos_create_thread(NULL, BEKEN_APPLICATION_PRIORITY, "h2-cp-start",
                           h2_bk_cp_start_task, 4096, NULL) != 0) {
        (void)rtos_deinit_semaphore(&s_bk_init_done);
        emergency_uart_write_string(
            0, "H2_BK_CP_TRANSPORT_READY status=task-fail\r\n");
        return -1;
    }
    s_bk_init_result = bk_init();
    if (rtos_set_semaphore(&s_bk_init_done) != kNoErr) {
        return -1;
    }
    return s_bk_init_result;
}
