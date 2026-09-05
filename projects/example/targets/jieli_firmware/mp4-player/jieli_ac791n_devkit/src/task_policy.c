#include "app_config.h"

#include "system/task.h"

const struct task_info task_info_table[] = {
    {"app_core", 15, 8192, 1024},
    {"sys_event", 29, 512, 0},
    {"systimer", 14, 256, 0},
    {"sys_timer", 9, 512, 128},
    {"dw_update", 21, 512, 32},
    {"h2mp4/heartbeat", 8, 2048, 64},
    /* The SDK table stores stack depth in 32-bit words. */
    {"h2mp4/decode", 10, 8192, 128},
#if CPU_CORE_NUM > 1
    {"#C0btctrler", 19, 768, 384},
    {"#C0btstack", 18, 1536, 384},
#else
    {"btctrler", 19, 768, 384},
    {"btstack", 18, 1536, 384},
#endif
    {0, 0, 0, 0, 0},
};
