#include "h2_bk_h2loader_cp_transport.h"
#include "h2_bk_target_task_policy.h"

#include "bk_private/bk_init.h"

#include "components/system.h"
#include "driver/pwr_clk.h"
#include "driver/uart.h"
#include "modules/pm.h"
#include "os/os.h"

#include <stdio.h>
#include <string.h>

static void h2_bk_serial_log_string(int port, const char *string) {
    (void)port;
    os_printf("%s", string);
}

#define emergency_uart_write_string h2_bk_serial_log_string

void _init(void) {}
void _fini(void) {}

extern void rtos_set_user_app_entry(beken_thread_function_t entry);

static void h2loader_cp_transport_probe(unsigned int attempt, int rc) {
    char line[80];
    int length = snprintf(
        line,
        sizeof(line),
        "H2_BK_CP_TRANSPORT_RETRY attempt=%u rc=%d\r\n",
        attempt,
        rc);
    if (length <= 0 || (size_t)length >= sizeof(line)) {
        return;
    }
    (void)bk_uart_set_enable_tx(UART_ID_0, true);
    if (bk_uart_write_bytes(UART_ID_0, line, (uint32_t)length) == BK_OK) {
        bk_uart_wait_tx_over(UART_ID_0);
    }
}

static void h2loader_cp_entry(void) {
    unsigned int transport_attempt = 0u;
    int transport_rc;

    rtos_delay_milliseconds(500);
    if (h2_bk_target_task_policy_install() != H2_PAL_OK) {
        return;
    }
    emergency_uart_write_string(0, "H2_BK_CP_BOOT image=h2loader-managed\r\n");
    /* Keep CP1 online before initializing the AP/CP mailbox tunnel.  AP's
     * bk_init() waits for the CP user-app boot vote, while the mailbox peer is
     * not ready until that AP initialization completes.  Voting only after
     * transport startup therefore creates a clean-boot dependency cycle. */
    bk_pm_module_vote_boot_cp1_ctrl(PM_BOOT_CP1_MODULE_NAME_APP,
                                    PM_POWER_MODULE_STATE_ON);
    while ((transport_rc = h2_bk_h2loader_cp_transport_start()) != 0) {
        if ((transport_attempt++ % 10u) == 0u) {
            h2loader_cp_transport_probe(transport_attempt, transport_rc);
        }
        rtos_delay_milliseconds(100);
    }
    emergency_uart_write_string(
        0, "H2_BK_CP_TRANSPORT_READY status=ready\r\n");
}

int main(void) {
    rtos_set_user_app_entry((beken_thread_function_t)h2loader_cp_entry);
    return bk_init();
}
