#ifndef H2_BK_DMA2D_TEST_PM_H
#define H2_BK_DMA2D_TEST_PM_H
#define PM_POWER_SUB_MODULE_NAME_VIDP_DMA2D 7
#define PM_POWER_MODULE_STATE_OFF 0
int bk_pm_module_vote_power_ctrl(int module, int state);
#endif
