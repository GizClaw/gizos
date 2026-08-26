#ifndef H2_BK_AP_TARGET_TASK_POLICY_H
#define H2_BK_AP_TARGET_TASK_POLICY_H

#include "h2_bk_platform_core.h"

#define H2_TARGET_TASK_POLICY_NAME                                             \
  "projects/example/targets/h2loader_tar_zlib/audio-system/bk7258_v3_202405"
#define H2_BK_TARGET_TASK_POLICY_ROUTES(X)                                     \
  X("h2loader/appcmd", H2_BK_TARGET_TASK_ROUTE_EXACT, NULL, 0u, 5u, 8192u,     \
    H2_BK_TASK_STACK_PSRAM)                                                    \
  X("h2loader/uartcmd", H2_BK_TARGET_TASK_ROUTE_EXACT, NULL, 0u, 5u, 8192u,    \
    H2_BK_TASK_STACK_PSRAM)                                                    \
  X("audio-system/music", H2_BK_TARGET_TASK_ROUTE_EXACT, NULL, 1u, 4u, 4096u,  \
    H2_BK_TASK_STACK_PSRAM)                                                    \
  X("audio-system/mic", H2_BK_TARGET_TASK_ROUTE_EXACT, NULL, 1u, 4u, 4096u,    \
    H2_BK_TASK_STACK_PSRAM)                                                    \
  X("bleikcp-speed/", H2_BK_TARGET_TASK_ROUTE_PREFIX, NULL, 0u, 6u, 4096u,     \
    H2_BK_TASK_STACK_PSRAM)

h2_pal_result_t h2_bk_target_task_policy_install(void);

#endif
