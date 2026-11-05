#ifndef IMAGE_TEST_ESP_SLEEP_H
#define IMAGE_TEST_ESP_SLEEP_H
#include "esp_err.h"
#include <stdint.h>
#define ESP_SLEEP_WAKEUP_TIMER 3u
uint32_t esp_sleep_get_wakeup_causes(void);
esp_err_t esp_sleep_enable_timer_wakeup(uint64_t us);
void esp_deep_sleep_start(void);
#endif
