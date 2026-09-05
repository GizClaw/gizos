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
#if CPU_CORE_NUM > 1
    /* Opus decode stays on CPU1. Keep capture, AEC and DAC/mixer scheduling
     * together on CPU0 so their reference timing remains deterministic. */
    {"audio-system/music", 10, 24576, 128},
    {"#C0audio-system/mic", 10, 8192, 128},
    {"#C0audio_server", 16, 2048, 256},
    {"#C0audio_decoder", 30, 2048, 64},
    {"#C0audio_encoder", 12, 2048, 64},
    {"#C0aec_encoder", 13, 1024, 0},
    {"#C0audio_mix", 28, 2048, 64},
#else
    {"audio-system/music", 10, 24576, 128},
    {"audio-system/mic", 10, 8192, 128},
    {"audio_server", 16, 2048, 256},
    {"audio_decoder", 30, 2048, 64},
    {"audio_encoder", 12, 2048, 64},
    {"aec_encoder", 13, 1024, 0},
    {"audio_mix", 28, 2048, 64},
#endif
#if CPU_CORE_NUM > 1
    {"#C0btctrler", 19, 768, 384},
    {"#C0btstack", 18, 1536, 384},
#else
    {"btctrler", 19, 768, 384},
    {"btstack", 18, 1536, 384},
#endif
    {0, 0, 0, 0, 0},
};
