#ifndef H2_ESP_RESOURCE_STATS_INTERNAL_H
#define H2_ESP_RESOURCE_STATS_INTERNAL_H
#include <stddef.h>
typedef enum h2_esp_resource_kind {
    H2_ESP_RESOURCE_TASK, H2_ESP_RESOURCE_QUEUE, H2_ESP_RESOURCE_MUTEX,
    H2_ESP_RESOURCE_SEMAPHORE, H2_ESP_RESOURCE_CONDITION, H2_ESP_RESOURCE_TIMER,
    H2_ESP_RESOURCE_COUNT
} h2_esp_resource_kind_t;
void h2_esp_resource_acquire(h2_esp_resource_kind_t kind, size_t stack_bytes);
void h2_esp_resource_release(h2_esp_resource_kind_t kind, size_t stack_bytes);
void h2_esp_memory_acquire(size_t bytes);
void h2_esp_memory_release(size_t bytes);
#endif
