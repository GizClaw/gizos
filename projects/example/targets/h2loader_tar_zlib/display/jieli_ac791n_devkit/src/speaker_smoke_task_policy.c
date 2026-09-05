#include "app_config.h"

#include "system/task.h"

const struct task_info task_info_table[] = {
    {"app_core", 15, 8192, 1024},
    {"sys_event", 29, 512, 0},
    {"systimer", 14, 256, 0},
    {"sys_timer", 9, 512, 128},
    {"dw_update", 21, 512, 32},
    {"audio_server", 16, 1024, 256},
    {"audio_decoder", 30, 1024, 64},
    {"audio_mix", 28, 512, 0},
    {0, 0, 0, 0, 0},
};
