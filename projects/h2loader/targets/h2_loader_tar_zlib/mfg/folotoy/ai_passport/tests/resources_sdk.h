#ifndef H2_PASSPORT_RESOURCES_SDK_H
#define H2_PASSPORT_RESOURCES_SDK_H
#include "h2loader_app.h"

#define MALLOC_CAP_INTERNAL 1u
#define MALLOC_CAP_8BIT 2u
static inline size_t heap_caps_get_free_size(unsigned caps) {
  (void)caps;
  return 100000;
}
static inline size_t heap_caps_get_largest_free_block(unsigned caps) {
  (void)caps;
  return 90000;
}
static inline unsigned uxTaskGetStackHighWaterMark(void* task) {
  (void)task;
  return 8192;
}
int h2_esp_board_audio_set_aec_enabled(int enabled);
int h2_esp_board_audio_release(void);
int h2_esp_platform_wifi_stop(void);
int h2loader_bleikcp_command_service_with_config(
    const h2loader_ble_command_service_config_t* config,
    h2loader_app_command_service_api_t* out);
#endif
