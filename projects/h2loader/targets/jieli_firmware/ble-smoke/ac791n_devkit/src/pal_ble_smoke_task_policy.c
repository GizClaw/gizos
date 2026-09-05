#include "app_config.h"

#include "system/task.h"

const struct task_info task_info_table[] = {
    {"app_core", 15, 4096, 256},
    {"sys_event", 29, 512, 0},
    {"systimer", 14, 256, 0},
    {"sys_timer", 9, 512, 64},
    {"dw_update", 21, 512, 32},
    {"h2pal/heartbeat", 10, 512, 0},
    {"h2loader/appcmd", 10, 12288, 128},
    {"h2app_rx", 12, 4096, 128},
#if CPU_CORE_NUM > 1
    {"#C0btctrler", 19, 768, 384},
    {"#C0btstack", 18, 1536, 384},
#else
    {"btctrler", 19, 768, 384},
    {"btstack", 18, 1536, 384},
#endif
    {"audio_server", 16, 1024, 256},
    {"audio_decoder", 30, 1024, 64},
    {"audio_encoder", 12, 384, 64},
    {"audio_mix", 28, 512, 0},
    {0, 0, 0, 0, 0},
};
