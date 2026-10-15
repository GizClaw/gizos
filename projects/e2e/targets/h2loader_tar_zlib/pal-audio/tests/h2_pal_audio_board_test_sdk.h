#ifndef H2_PAL_AUDIO_BOARD_TEST_SDK_H
#define H2_PAL_AUDIO_BOARD_TEST_SDK_H
#include "projects/e2e/apps/pal-audio/app/include/h2_pal_audio_e2e.h"
#include "h2_loader_app_client.h"
#include "h2_runtime.h"
#include <stdint.h>
#define pdMS_TO_TICKS(ms) (ms)
void vTaskDelay(uint32_t ms);
void rtos_delay_milliseconds(uint32_t ms);
void esp_restart(void);
typedef struct test_app_description { const char *version; } test_app_description_t;
const test_app_description_t *esp_app_get_description(void);
int h2_esp_board_runtime_config(h2_runtime_config_t *config);
int h2_esp_board_start_entry_task(const char *name, void (*entry)(void *), void *user);
int h2_esp_target_task_policy_install(void);
int h2_esp_h2loader_app_commands_prepare_serial(const h2_runtime_config_t *config,
                                                const char *name, uint32_t p1,
                                                uint32_t coredump);
int h2_esp_h2loader_app_confirm(h2_runtime_t *runtime);
int h2_esp_h2loader_app_commands_get_config(h2_loader_app_client_config_t *config);
void bk_init(void);
uint32_t bk_misc_get_reset_reason(void);
int h2_bk7258_board_runtime_config(h2_runtime_config_t *config);
int h2_bk7258_board_start_entry_task(const char *name, void (*entry)(void *), void *user);
int h2_bk_target_task_policy_install(void);
int h2_bk_h2loader_start_app_iostreamikcp_with_capabilities(
    h2_runtime_t *runtime, const char *name, uint32_t capabilities);
int h2_bk_h2loader_confirm_current_app(h2_runtime_t *runtime);
int h2_bk_h2loader_app_commands_get_config(h2_loader_app_client_config_t *config);
const h2_pal_power_api_t *h2_bk_h2loader_power_api(void);
#endif
