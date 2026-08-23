#ifndef H2_ESP_BOARD_TASK_TEST_SDK_H
#define H2_ESP_BOARD_TASK_TEST_SDK_H

#include <stddef.h>
#include <stdint.h>

#define CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM 1
#define MALLOC_CAP_SPIRAM 0x01u
#define MALLOC_CAP_8BIT 0x02u
#define pdPASS 1
#define tskIDLE_PRIORITY 0u
#define tskNO_AFFINITY (-1)

typedef int BaseType_t;
typedef void (*TaskFunction_t)(void *user);

void *heap_caps_malloc(size_t size, uint32_t capabilities);
void heap_caps_free(void *ptr);
BaseType_t xTaskCreatePinnedToCoreWithCaps(
    TaskFunction_t entry,
    const char *name,
    uint32_t stack_size,
    void *user,
    uint32_t priority,
    void *out_task,
    int core_id,
    uint32_t capabilities);
void vTaskDeleteWithCaps(void *task);

#endif
