#include "app_config.h"

#include "system/task.h"

const struct task_info task_info_table[] = {
    {"app_core", 15, 8192, 1024},
    {"sys_event", 29, 512, 0},
    {"systimer", 14, 256, 0},
    {"sys_timer", 9, 512, 128},
    {"dw_update", 21, 512, 32},
    {"h2loader/uartcmd", 10, 4096, 128},
    {"h2loader/appcmd", 10, 12288, 128},
    {"h2app_rx", 12, 4096, 128},
#if CPU_CORE_NUM > 1
    /* Keep the software H.264 decoder on CPU1.  JieLi's #C0 task-policy
     * prefix pins the presentation/audio side to CPU0, so full-frame LCD
     * transfers and DAC consumption do not contend with decode execution. */
    {"#C0h2mp4/runtime", 10, 32768, 128},
    {"#C0$mp4-player/audio", 10, 2048, 128},
    {"$mp4-player/decoder", 10, 8192, 128},
    {"#C0audio_server", 16, 1024, 256},
    {"#C0audio_decoder", 30, 1024, 64},
#else
    {"h2mp4/runtime", 10, 32768, 128},
    {"$mp4-player/audio", 10, 2048, 128},
    {"$mp4-player/decoder", 10, 8192, 128},
    {"audio_server", 16, 1024, 256},
    {"audio_decoder", 30, 1024, 64},
#endif
    {"audio_encoder", 12, 384, 64},
    {"audio_mix", 28, 512, 0},
#if CPU_CORE_NUM > 1
    {"#C0btctrler", 19, 768, 384},
    {"#C0btstack", 18, 1536, 384},
#else
    {"btctrler", 19, 768, 384},
    {"btstack", 18, 1536, 384},
#endif
    {0, 0, 0, 0, 0},
};
