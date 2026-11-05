#ifndef H2_TEST_SHADOW_ESP_LOG_H
#define H2_TEST_SHADOW_ESP_LOG_H
#include <stdint.h>

uint32_t esp_log_timestamp(void);

/* Consume the tag so a file-static TAG stays "used" under -Wunused. */
#define ESP_LOGE(tag, ...) ((void)(tag))
#define ESP_LOGW(tag, ...) ((void)(tag))
#define ESP_LOGI(tag, ...) ((void)(tag))
#define ESP_LOGD(tag, ...) ((void)(tag))
#define ESP_LOGV(tag, ...) ((void)(tag))
#endif
