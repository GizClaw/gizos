#include "h2_bk_target_task_policy.h"

#include "bk_private/bk_init.h"
#include "components/system.h"
#include "driver/pwr_clk.h"
#include "modules/pm.h"
#include "os/os.h"

void _init(void) {}
void _fini(void) {}

extern void rtos_set_user_app_entry(beken_thread_function_t entry);

extern void h2loader_cp_try_fixed_app(void);

static int main_entered;

/* SDK calls this once before main, then after driver_init inside bk_init.
 * Only the latter has Flash/IPC ready, and still precedes radio startup. */
void bk_module_init(void) {
    if (main_entered) h2loader_cp_try_fixed_app();
}

static void h2loader_cp_entry(void) {
    if (h2_bk_target_task_policy_install() != H2_PAL_OK) {
        return;
    }
    /* Preserve the SDK boot vote and system services. Managed UART and logs
     * belong to AP/UART1; CP owns no H2Loader UART transport. */
    bk_pm_module_vote_boot_cp1_ctrl(PM_BOOT_CP1_MODULE_NAME_APP,
                                    PM_POWER_MODULE_STATE_ON);
}

int main(void) {
    main_entered = 1;
    rtos_set_user_app_entry((beken_thread_function_t)h2loader_cp_entry);
    return bk_init();
}
