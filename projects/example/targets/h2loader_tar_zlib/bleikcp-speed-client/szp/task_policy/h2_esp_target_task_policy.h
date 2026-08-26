#ifndef H2_ESP_TARGET_TASK_POLICY_H
#define H2_ESP_TARGET_TASK_POLICY_H

#include "h2_esp_platform_core.h"

#define H2_TARGET_TASK_POLICY_NAME                                             \
  "projects/example/targets/h2loader_tar_zlib/bleikcp-speed-client/szp"
#define H2_ESP_TARGET_TASK_POLICY_ROUTES(X)                                    \
  X("h2loader/appcmd", 8u, H2_ESP_TASK_CORE_0, 4096u, H2_ESP_TASK_STACK_PSRAM) \
  X("h2loader/return", 8u, H2_ESP_TASK_CORE_0, 4096u, H2_ESP_TASK_STACK_PSRAM) \
  X("blelink", 6u, H2_ESP_TASK_CORE_0, 4096u, H2_ESP_TASK_STACK_PSRAM)         \
  X("bleikcp/kcp", 7u, H2_ESP_TASK_CORE_0, 4096u, H2_ESP_TASK_STACK_PSRAM)     \
  X("bleikcp/server", 5u, H2_ESP_TASK_CORE_0, 4096u, H2_ESP_TASK_STACK_PSRAM)  \
  X("h2peer/net", 7u, H2_ESP_TASK_CORE_0, 4096u, H2_ESP_TASK_STACK_PSRAM)      \
  X("h2peer/udp", 7u, H2_ESP_TASK_CORE_0, 4096u, H2_ESP_TASK_STACK_PSRAM)      \
  X("bleikcp-speed/kcp", 7u, H2_ESP_TASK_CORE_0, 4096u,                        \
    H2_ESP_TASK_STACK_PSRAM)                                                   \
  X("bleikcp-speed/server", 5u, H2_ESP_TASK_CORE_0, 4096u,                     \
    H2_ESP_TASK_STACK_PSRAM)

h2_pal_result_t h2_esp_target_task_policy_install(void);

#endif
