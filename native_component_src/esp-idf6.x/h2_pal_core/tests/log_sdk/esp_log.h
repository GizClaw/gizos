#ifndef H2_TEST_ESP_LOG_H
#define H2_TEST_ESP_LOG_H

/* Controlled host facade, not an ESP-IDF implementation or SDK qualification. */
#define CONFIG_LOG_MAXIMUM_LEVEL 2
#define CONFIG_LOG_MASTER_LEVEL H2_TEST_LOG_MASTER
#define ESP_LOG_VERSION 1
typedef enum {
    ESP_LOG_NONE = 0,
    ESP_LOG_ERROR = 1,
    ESP_LOG_WARN = 2,
    ESP_LOG_INFO = 3,
    ESP_LOG_DEBUG = 4,
} esp_log_level_t;

void esp_log_level_set(const char *tag, esp_log_level_t level);
esp_log_level_t esp_log_level_get(const char *tag);
void esp_log_set_level_master(esp_log_level_t level);
esp_log_level_t esp_log_get_level_master(void);
void h2_test_log_emit(esp_log_level_t level, const char *tag,
                      const char *format, ...);

#define ESP_LOG_LEVEL(level, tag, format, ...) \
    h2_test_log_emit(level, tag, format, __VA_ARGS__)
#if CONFIG_LOG_MASTER_LEVEL
#define H2_TEST_LOG_MASTER_ALLOWS(level) (esp_log_get_level_master() >= (level))
#else
#define H2_TEST_LOG_MASTER_ALLOWS(level) 1
#endif
#define ESP_LOG_LEVEL_LOCAL(level, tag, format, ...) do { \
    if (CONFIG_LOG_MAXIMUM_LEVEL >= (level) && H2_TEST_LOG_MASTER_ALLOWS(level)) { \
        ESP_LOG_LEVEL(level, tag, format, __VA_ARGS__); \
    } \
} while (0)
#define ESP_LOGD(tag, format, ...) \
    ESP_LOG_LEVEL_LOCAL(ESP_LOG_DEBUG, tag, format, __VA_ARGS__)
#define ESP_LOGI(tag, format, ...) \
    ESP_LOG_LEVEL_LOCAL(ESP_LOG_INFO, tag, format, __VA_ARGS__)
#define ESP_LOGW(tag, format, ...) \
    ESP_LOG_LEVEL_LOCAL(ESP_LOG_WARN, tag, format, __VA_ARGS__)
#define ESP_LOGE(tag, format, ...) \
    ESP_LOG_LEVEL_LOCAL(ESP_LOG_ERROR, tag, format, __VA_ARGS__)

#endif
