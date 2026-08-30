#include "h2_esp_platform_safe_call.h"

#include "sdkconfig.h"

#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define H2_ESP_SAFE_CALL_MIN_STACK_DEPTH 1024u
#define H2_ESP_SAFE_CALL_WORKER_STACK_DEPTH 16384u
#define H2_ESP_SAFE_CALL_CONTEXT_CAPACITY 1024u
#define H2_ESP_SAFE_IO_SCRATCH_CAPACITY (16u * 1024u)
#define H2_ESP_SAFE_CALL_WORKER_PRIORITY \
    ((configMAX_PRIORITIES > 9u) ? 9u : (configMAX_PRIORITIES - 1u))
#define H2_ESP_SAFE_CALL_WORKER_CORE 0

#if defined(CONFIG_SPIRAM_XIP_FROM_PSRAM) && CONFIG_SPIRAM_XIP_FROM_PSRAM
#define H2_ESP_SAFE_CALL_PSRAM_XIP 1
#else
#define H2_ESP_SAFE_CALL_PSRAM_XIP 0
#endif

typedef union h2_esp_safe_call_context_storage {
    max_align_t alignment;
    uint8_t bytes[H2_ESP_SAFE_CALL_CONTEXT_CAPACITY];
} h2_esp_safe_call_context_storage_t;

typedef union h2_esp_safe_io_scratch_storage {
    max_align_t alignment;
    uint8_t bytes[H2_ESP_SAFE_IO_SCRATCH_CAPACITY];
} h2_esp_safe_io_scratch_storage_t;

typedef struct h2_esp_safe_call_worker {
    h2_esp_platform_safe_call_cb_t callback;
    void *caller_context;
    size_t context_size;
    h2_esp_safe_call_context_storage_t context;
    StaticSemaphore_t request_storage;
    SemaphoreHandle_t request;
    StaticSemaphore_t done_storage;
    SemaphoreHandle_t done;
    StaticTask_t task_storage;
    TaskHandle_t task;
    StackType_t *stack;
} h2_esp_safe_call_worker_t;

_Static_assert(sizeof(StackType_t) == 1u,
               "ESP-IDF task stack depth and storage must use bytes");

static h2_esp_safe_call_worker_t s_safe_call_worker;
static StaticSemaphore_t s_safe_call_mutex_storage;
static SemaphoreHandle_t s_safe_call_mutex;
static portMUX_TYPE s_safe_call_mutex_init_lock = portMUX_INITIALIZER_UNLOCKED;
static h2_esp_safe_io_scratch_storage_t s_safe_io_scratch;
static StaticSemaphore_t s_safe_io_mutex_storage;
static SemaphoreHandle_t s_safe_io_mutex;
static portMUX_TYPE s_safe_io_mutex_init_lock = portMUX_INITIALIZER_UNLOCKED;

static int safe_call_current_stack_is_internal(void) {
    const StackType_t *stack_start = xTaskGetStackStart(NULL);
    return stack_start != NULL && esp_ptr_internal(stack_start);
}

static SemaphoreHandle_t safe_call_mutex(void) {
    portENTER_CRITICAL(&s_safe_call_mutex_init_lock);
    if (s_safe_call_mutex == NULL) {
        s_safe_call_mutex =
            xSemaphoreCreateMutexStatic(&s_safe_call_mutex_storage);
    }
    portEXIT_CRITICAL(&s_safe_call_mutex_init_lock);
    return s_safe_call_mutex;
}

static SemaphoreHandle_t safe_io_mutex(void) {
    portENTER_CRITICAL(&s_safe_io_mutex_init_lock);
    if (s_safe_io_mutex == NULL) {
        s_safe_io_mutex =
            xSemaphoreCreateMutexStatic(&s_safe_io_mutex_storage);
    }
    portEXIT_CRITICAL(&s_safe_io_mutex_init_lock);
    return s_safe_io_mutex;
}

static void safe_call_worker_task(void *raw_worker) {
    h2_esp_safe_call_worker_t *worker =
        (h2_esp_safe_call_worker_t *)raw_worker;

    for (;;) {
        if (xSemaphoreTake(worker->request, portMAX_DELAY) != pdTRUE) {
            abort();
        }
        worker->callback(worker->context.bytes);
        memcpy(
            worker->caller_context,
            worker->context.bytes,
            worker->context_size);
        (void)xSemaphoreGive(worker->done);
    }
}

static h2_pal_result_t safe_call_worker_init(void) {
    if (s_safe_call_worker.task != NULL) {
        return H2_PAL_OK;
    }
    if (s_safe_call_worker.request == NULL) {
        s_safe_call_worker.request =
            xSemaphoreCreateBinaryStatic(&s_safe_call_worker.request_storage);
    }
    if (s_safe_call_worker.done == NULL) {
        s_safe_call_worker.done =
            xSemaphoreCreateBinaryStatic(&s_safe_call_worker.done_storage);
    }
    if (s_safe_call_worker.request == NULL || s_safe_call_worker.done == NULL) {
        return H2_PAL_ERR_TASK;
    }
    if (s_safe_call_worker.stack == NULL) {
        s_safe_call_worker.stack = heap_caps_malloc(
            H2_ESP_SAFE_CALL_WORKER_STACK_DEPTH,
            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (s_safe_call_worker.stack == NULL) {
        return H2_PAL_ERR_TASK;
    }
    s_safe_call_worker.task = xTaskCreateStaticPinnedToCore(
        safe_call_worker_task,
        "$esp/safe-call",
        H2_ESP_SAFE_CALL_WORKER_STACK_DEPTH,
        &s_safe_call_worker,
        H2_ESP_SAFE_CALL_WORKER_PRIORITY,
        s_safe_call_worker.stack,
        &s_safe_call_worker.task_storage,
        H2_ESP_SAFE_CALL_WORKER_CORE);
    return s_safe_call_worker.task != NULL ? H2_PAL_OK : H2_PAL_ERR_TASK;
}

h2_pal_result_t h2_esp_platform_safe_call(
    h2_esp_platform_safe_call_cb_t callback,
    void *context,
    size_t context_size,
    size_t stack_depth) {
    SemaphoreHandle_t mutex;
    h2_pal_result_t rc;

    if (callback == NULL || context == NULL || context_size == 0u ||
        stack_depth < H2_ESP_SAFE_CALL_MIN_STACK_DEPTH || xPortInIsrContext()) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (context_size > H2_ESP_SAFE_CALL_CONTEXT_CAPACITY ||
        stack_depth > H2_ESP_SAFE_CALL_WORKER_STACK_DEPTH) {
        return H2_PAL_ERR_INVALID_ARG;
    }

    if (safe_call_current_stack_is_internal() && esp_ptr_internal(context) &&
        (H2_ESP_SAFE_CALL_PSRAM_XIP ||
         esp_ptr_in_iram((const void *)callback))) {
        callback(context);
        return H2_PAL_OK;
    }

    mutex = safe_call_mutex();
    if (mutex == NULL || xSemaphoreTake(mutex, portMAX_DELAY) != pdTRUE) {
        return H2_PAL_ERR_TASK;
    }
    rc = safe_call_worker_init();
    if (rc != H2_PAL_OK) {
        (void)xSemaphoreGive(mutex);
        return rc;
    }
    memcpy(s_safe_call_worker.context.bytes, context, context_size);
    s_safe_call_worker.callback = callback;
    s_safe_call_worker.caller_context = context;
    s_safe_call_worker.context_size = context_size;
    if (xSemaphoreGive(s_safe_call_worker.request) != pdTRUE) {
        (void)xSemaphoreGive(mutex);
        return H2_PAL_ERR_TASK;
    }
    if (xSemaphoreTake(s_safe_call_worker.done, portMAX_DELAY) != pdTRUE) {
        (void)xSemaphoreGive(mutex);
        return H2_PAL_ERR_TASK;
    }
    (void)xSemaphoreGive(mutex);
    return H2_PAL_OK;
}

h2_pal_result_t h2_esp_platform_safe_io_acquire(
    uint8_t **out_buffer,
    size_t *out_capacity) {
    SemaphoreHandle_t mutex;

    if (out_buffer == NULL || out_capacity == NULL || xPortInIsrContext()) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_buffer = NULL;
    *out_capacity = 0u;
    mutex = safe_io_mutex();
    if (mutex == NULL || xSemaphoreTake(mutex, portMAX_DELAY) != pdTRUE) {
        return H2_PAL_ERR_TASK;
    }
    *out_buffer = s_safe_io_scratch.bytes;
    *out_capacity = sizeof(s_safe_io_scratch.bytes);
    return H2_PAL_OK;
}

void h2_esp_platform_safe_io_release(void) {
    if (s_safe_io_mutex != NULL) {
        (void)xSemaphoreGive(s_safe_io_mutex);
    }
}
