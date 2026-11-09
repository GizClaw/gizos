#ifndef H2_TEST_ESP_SLEEP_H
#define H2_TEST_ESP_SLEEP_H
#include "esp_err.h"
#include <stdint.h>
#define ESP_SLEEP_WAKEUP_ALL 0
#define ESP_EXT1_WAKEUP_ANY_LOW 0
esp_err_t esp_sleep_disable_wakeup_source(int source);
esp_err_t esp_sleep_enable_ext1_wakeup_io(uint64_t mask, int mode);
esp_err_t esp_sleep_enable_ext0_wakeup(int pin, int level);
/* The real declaration is noreturn; the host fake returns so the caller's
 * failure handling stays reachable in tests. */
void esp_deep_sleep_start(void);
esp_err_t esp_sleep_enable_timer_wakeup(uint64_t time_us);
#endif
