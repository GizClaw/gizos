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
#if CPU_CORE_NUM > 1
    /* Match the Display/Touch/Audio targets using this same board layout.
     * JieLi task stack sizes are words, not ESP's byte-sized stack options. */
    {"#C0btctrler", 19, 768, 384},
    {"#C0btstack", 18, 1536, 384},
#else
    {"btctrler", 19, 768, 384},
    {"btstack", 18, 1536, 384},
#endif
    {"h2loader", 10, 4096, 128},
    {"h2loader_rx", 12, 1024, 128},
    {H2LOADER_APP_COMMAND_TASK_NAME_VALUE, 10, 12288, 128},
    {H2LOADER_BLE_COMMAND_TASK_NAME_VALUE, 10, 12288, 128},
    {H2_BLEIKCP_SERVER_TASK_NAME_VALUE, 10, 1536, 128},
    {H2_BLEIKCP_WORKER_TASK_NAME_VALUE, 10, 1536, 128},
    {H2_LOADER_BLE_LINK_TASK_NAME_VALUE, 10, 1536, 64},
    /* The shared h2loader board layout enables JieLi's audio subsystem. Its
     * initcall creates audio_server before the selected Loader/App entrypoint
     * runs, so every firmware using this layout must provide these policies.
     * A Loader boot followed by a bank switch otherwise leaves server_open()
     * with no worker and the App blocks forever in AUDIO_DEC_OPEN. */
    {"audio_server", 16, 1024, 256},
    {"audio_decoder", 30, 1024, 64},
    {"audio_encoder", 12, 384, 64},
    {"audio_mix", 28, 512, 0},
    {0, 0, 0, 0, 0},
};
