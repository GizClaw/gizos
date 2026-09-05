#ifndef H2_ESP_TARGET_TASK_POLICY_H
#define H2_ESP_TARGET_TASK_POLICY_H

#include "h2/pal/core/h2_pal_errors.h"

#define H2_LUA_COSMIC_DRIFT_ENTRY_TASK_NAME_VALUE "amoled/lua-cosmic-drift"

#ifdef __cplusplus
extern "C" {
#endif

h2_pal_result_t h2_esp_target_task_policy_install(void);

#ifdef __cplusplus
}
#endif

#endif
