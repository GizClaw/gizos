#ifndef H2_PAL_AUDIO_BOARD_TEST_SDK_H
#define H2_PAL_AUDIO_BOARD_TEST_SDK_H
#include "projects/e2e/apps/pal-audio/app/include/h2_pal_audio_e2e.h"
#include "h2_device_service.h"
#include <stdint.h>
#define pdMS_TO_TICKS(ms) (ms)
void vTaskDelay(uint32_t ms);
void rtos_delay_milliseconds(uint32_t ms);
void esp_restart(void);
typedef struct test_app_description {
  const char* version;
} test_app_description_t;
const test_app_description_t* esp_app_get_description(void);
int h2_esp_board_runtime_config(h2_runtime_config_t* config);
int h2_esp_board_start_entry_task(const char* name, void (*entry)(void*),
                                  void* user);
int h2_esp_target_task_policy_install(void);
void bk_init(void);
uint32_t bk_misc_get_reset_reason(void);
int h2_bk7258_board_runtime_config(h2_runtime_config_t* config);
int h2_bk7258_board_start_entry_task(const char* name, void (*entry)(void*),
                                     void* user);
int h2_bk_target_task_policy_install(void);
#endif
