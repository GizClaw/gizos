#if defined(H2_BK_CP_STARTUP_HOST_TEST)
#include "startup_test_sdk.h"
#define main h2_bk_cp_main
#else
#include "h2_bk_target_task_policy.h"

#include "bk_private/bk_init.h"
#include "components/system.h"
#include "driver/pwr_clk.h"
#include "modules/pm.h"
#include "os/os.h"
#include "sdkconfig.h"

void _init(void) {}
void _fini(void) {}
#endif

#if CONFIG_H2_AP_CPU_FREQ_480M && CONFIG_CPU_DEFAULT_FREQ_60M
#error "The AP 480 MHz floor conflicts with CPU_DEFAULT_FREQ_60M"
#endif
#if CONFIG_H2_AP_CPU_FREQ_480M && CONFIG_CLK_FORCE_MAX_CPU_FREQ_320M
#error "The AP 480 MHz floor conflicts with CLK_FORCE_MAX_CPU_FREQ_320M"
#endif

extern void rtos_set_user_app_entry(beken_thread_function_t entry);

extern void h2_bk_cp_try_fixed_app(void);

static int main_entered;

/* SDK calls this once before main, then after driver_init inside bk_init.
 * Only the latter has Flash/IPC ready, and still precedes radio startup. */
void bk_module_init(void) {
  if (main_entered) h2_bk_cp_try_fixed_app();
}

static void h2_bk_cp_entry(void) {
  if (h2_bk_target_task_policy_install() != H2_PAL_OK) {
    return;
  }
#if CONFIG_H2_AP_CPU_FREQ_480M
  /* CP owns the shared clock vote. Set the Board's active floor before AP
   * starts, so audio shutdown cannot lower it while other work is running.
   * Deep-sleep entry remains governed by the SDK's power-state flow. */
  if (bk_pm_module_vote_cpu_freq(PM_DEV_ID_DEFAULT, PM_CPU_FRQ_480M) != BK_OK) {
    return;
  }
#endif
  /* Preserve the SDK boot vote and system services. Managed UART and logs
   * belong to AP/UART1; CP owns no H2Loader UART transport. */
  bk_pm_module_vote_boot_cp1_ctrl(PM_BOOT_CP1_MODULE_NAME_APP,
                                  PM_POWER_MODULE_STATE_ON);
}

int main(void) {
  main_entered = 1;
  rtos_set_user_app_entry((beken_thread_function_t)h2_bk_cp_entry);
  return bk_init();
}
