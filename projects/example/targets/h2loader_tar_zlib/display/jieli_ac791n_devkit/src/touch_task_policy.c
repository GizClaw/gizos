#include "app_config.h"
#include "h2_bleikcp_task_names.h"
#include "h2_loader_task_names.h"
#include "h2loader_app_task_names.h"

#include "system/task.h"

const struct task_info task_info_table[] = {
    {"app_core", 15, 8192, 1024},
    {"sys_event", 29, 512, 0},
    {"systimer", 14, 256, 0},
    {"sys_timer", 9, 512, 128},
    {"dw_update", 21, 512, 32},
    {"h2loader/uartcmd", 10, 4096, 128},
    {"h2loader/appcmd", 10, 12288, 128},
    {H2LOADER_BLE_COMMAND_TASK_NAME_VALUE, 10, 12288, 128},
    {H2_BLEIKCP_SERVER_TASK_NAME_VALUE, 10, 1536, 128},
    {H2_BLEIKCP_WORKER_TASK_NAME_VALUE, 10, 1536, 128},
    {H2_LOADER_BLE_LINK_TASK_NAME_VALUE, 10, 1536, 64},
    {"h2app_rx", 12, 4096, 128},
    {"$runtime/input", 10, 4096, 128},
    {"touch-smoke", 10, 16384, 128},
    {"$lvgl/swdraw", 10, 8192, 128},
#if CPU_CORE_NUM > 1
    {"#C0btctrler", 19, 768, 384},
    {"#C0btstack", 18, 1536, 384},
#else
    {"btctrler", 19, 768, 384},
    {"btstack", 18, 1536, 384},
#endif
    {0, 0, 0, 0, 0},
};
