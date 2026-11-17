#ifndef H2_BK_CP_STARTUP_TEST_SDK_H
#define H2_BK_CP_STARTUP_TEST_SDK_H

#define H2_PAL_OK 0
#define BK_OK 0
#define PM_DEV_ID_DEFAULT 40
#define PM_CPU_FRQ_480M 6
#define PM_BOOT_CP1_MODULE_NAME_APP 1
#define PM_POWER_MODULE_STATE_ON 1

typedef void (*beken_thread_function_t)(void);

int h2_bk_target_task_policy_install(void);
int bk_init(void);
int bk_pm_module_vote_cpu_freq(int module, int frequency);
int bk_pm_module_vote_boot_cp1_ctrl(int module, int state);

#endif
