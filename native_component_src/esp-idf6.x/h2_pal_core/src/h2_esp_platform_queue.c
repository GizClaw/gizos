#include "h2_esp_platform_core.h"
#include "h2_esp_resource_stats_internal.h"

#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_timer.h"

#include <stdint.h>
#include <stdlib.h>

#if (configSUPPORT_STATIC_ALLOCATION != 1)
#error "h2_pal_core queues require FreeRTOS static allocation support"
#endif

#define H2_ESP_QUEUE_CLOSE_POLL_MS 10u

struct h2_pal_queue {
    QueueHandle_t handle;
    const h2_pal_mem_api_t *allocator;
    size_t item_size;
    size_t item_count;
    void *drop_scratch;
    void *storage;
    StaticQueue_t *control;
    SemaphoreHandle_t lock;
    SemaphoreHandle_t changed;
    int closed;
};

static void *queue_alloc(const h2_pal_mem_api_t *allocator, size_t len) {
    return allocator != NULL ? h2_pal_mem_alloc(allocator, len) : calloc(1u, len);
}

static void queue_free(const h2_pal_mem_api_t *allocator, void *ptr) {
    if (allocator != NULL) {
        h2_pal_mem_free(allocator, ptr);
    } else {
        free(ptr);
    }
}

static int esp_queue_create(void *user, const h2_pal_queue_config_t *config, h2_pal_queue_t **out_queue) {
    (void)user;
    if (config == NULL || out_queue == NULL || config->item_size == 0u || config->item_count == 0u) {
        return H2_PAL_QUEUE_ERR_INVALID_ARG;
    }
    *out_queue = NULL;

    h2_pal_queue_t *queue = (h2_pal_queue_t *)queue_alloc(config->allocator, sizeof(*queue));
    if (queue == NULL) {
        return H2_PAL_QUEUE_ERR_NO_MEMORY;
    }
    queue->handle = NULL;
    queue->allocator = config->allocator;
    queue->item_size = config->item_size;
    queue->item_count = config->item_count;
    queue->closed = 0;
    queue->lock = NULL;
    queue->changed = NULL;
    queue->storage = NULL;
    queue->control = NULL;
    queue->drop_scratch = queue_alloc(config->allocator, config->item_size);
    if (queue->drop_scratch == NULL) {
        queue_free(config->allocator, queue);
        return H2_PAL_QUEUE_ERR_NO_MEMORY;
    }
#if (configSUPPORT_STATIC_ALLOCATION == 1)
    if (config->allocator != NULL) {
        if (config->item_count > SIZE_MAX / config->item_size ||
            (size_t)(UBaseType_t)config->item_count != config->item_count ||
            (size_t)(UBaseType_t)config->item_size != config->item_size) {
            queue_free(config->allocator, queue->drop_scratch);
            queue_free(config->allocator, queue);
            return H2_PAL_QUEUE_ERR_INVALID_ARG;
        }
        const size_t storage_size =
            config->item_count * config->item_size;
        queue->storage = queue_alloc(config->allocator, storage_size);
        queue->control = (StaticQueue_t *)heap_caps_calloc(
            1u,
            sizeof(*queue->control),
            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (queue->storage != NULL && queue->control != NULL) {
            queue->handle = xQueueCreateStatic(
                (UBaseType_t)config->item_count,
                (UBaseType_t)config->item_size,
                (uint8_t *)queue->storage,
                queue->control);
        }
    }
#endif
    if (queue->handle == NULL && config->allocator == NULL) {
        queue->handle = xQueueCreate((UBaseType_t)config->item_count, (UBaseType_t)config->item_size);
    }
    if (queue->handle == NULL) {
        heap_caps_free(queue->control);
        queue_free(config->allocator, queue->storage);
        queue_free(config->allocator, queue->drop_scratch);
        queue_free(config->allocator, queue);
        return H2_PAL_QUEUE_ERR_NO_MEMORY;
    }
    queue->lock = xSemaphoreCreateMutex();
    queue->changed = xSemaphoreCreateBinary();
    if (queue->lock == NULL || queue->changed == NULL) {
        if (queue->lock != NULL) vSemaphoreDelete(queue->lock);
        if (queue->changed != NULL) vSemaphoreDelete(queue->changed);
        vQueueDelete(queue->handle);
        heap_caps_free(queue->control);
        queue_free(config->allocator, queue->storage);
        queue_free(config->allocator, queue->drop_scratch);
        queue_free(config->allocator, queue);
        return H2_PAL_ERR_NO_MEMORY;
    }
    h2_esp_resource_acquire(H2_ESP_RESOURCE_QUEUE, 0u);
    *out_queue = queue;
    return H2_PAL_QUEUE_OK;
}

static void esp_queue_destroy(void *user, h2_pal_queue_t *queue) {
    (void)user;
    if (queue == NULL) {
        return;
    }
    if (queue->handle != NULL) {
        vQueueDelete(queue->handle);
    }
    vSemaphoreDelete(queue->lock);
    vSemaphoreDelete(queue->changed);
    const h2_pal_mem_api_t *allocator = queue->allocator;
    heap_caps_free(queue->control);
    queue_free(allocator, queue->storage);
    queue_free(allocator, queue->drop_scratch);
    queue_free(allocator, queue);
    h2_esp_resource_release(H2_ESP_RESOURCE_QUEUE, 0u);
}

/* Queue operations use nonblocking kernel access under the admission lock.
 * Waiting outside it allows close to reject all pending senders without
 * resetting/discarding buffered data. A change signal gives fast progress;
 * bounded slices also wake every waiter when signals coalesce. */
static int queue_wait_changed(h2_pal_queue_t *queue, uint64_t started, uint32_t timeout_ms) {
    uint32_t slice = H2_ESP_QUEUE_CLOSE_POLL_MS;
    if (timeout_ms != H2_PAL_QUEUE_WAIT_FOREVER) {
        uint64_t elapsed = (uint64_t)esp_timer_get_time() / 1000u - started;
        if (elapsed >= timeout_ms) return H2_PAL_ERR_TIMEOUT;
        uint32_t remaining = timeout_ms - (uint32_t)elapsed;
        if (slice > remaining) slice = remaining;
    }
    TickType_t ticks = pdMS_TO_TICKS(slice);
    if (ticks == 0) ticks = 1;
    (void)xSemaphoreTake(queue->changed, ticks);
    return H2_PAL_OK;
}

static int esp_queue_send(void *user, h2_pal_queue_t *queue, const void *item, uint32_t timeout_ms) {
    (void)user;
    if (queue == NULL || item == NULL) return H2_PAL_ERR_INVALID_ARG;
    uint64_t started = (uint64_t)esp_timer_get_time() / 1000u;
    for (;;) {
        (void)xSemaphoreTake(queue->lock, portMAX_DELAY);
        int result = queue->closed ? H2_PAL_ERR_CLOSED
            : xQueueSend(queue->handle, item, 0) == pdTRUE ? H2_PAL_OK : H2_PAL_ERR_TIMEOUT;
        (void)xSemaphoreGive(queue->lock);
        if (result != H2_PAL_ERR_TIMEOUT) {
            if (result == H2_PAL_OK) (void)xSemaphoreGive(queue->changed);
            return result;
        }
        result = queue_wait_changed(queue, started, timeout_ms);
        if (result != H2_PAL_OK) return result;
    }
}

static int esp_queue_send_latest(void *user, h2_pal_queue_t *queue, const void *item) {
    (void)user;
    if (queue == NULL || item == NULL) return H2_PAL_ERR_INVALID_ARG;
    (void)xSemaphoreTake(queue->lock, portMAX_DELAY);
    int result = H2_PAL_OK;
    if (queue->closed) result = H2_PAL_ERR_CLOSED;
    else if (xQueueSend(queue->handle, item, 0) != pdTRUE) {
        (void)xQueueReceive(queue->handle, queue->drop_scratch, 0);
        if (xQueueSend(queue->handle, item, 0) != pdTRUE) result = H2_PAL_ERR_IO;
    }
    (void)xSemaphoreGive(queue->lock);
    if (result == H2_PAL_OK) (void)xSemaphoreGive(queue->changed);
    return result;
}

static int esp_queue_recv(void *user, h2_pal_queue_t *queue, void *out_item, uint32_t timeout_ms) {
    (void)user;
    if (queue == NULL || out_item == NULL) return H2_PAL_ERR_INVALID_ARG;
    uint64_t started = (uint64_t)esp_timer_get_time() / 1000u;
    for (;;) {
        (void)xSemaphoreTake(queue->lock, portMAX_DELAY);
        int result = xQueueReceive(queue->handle, out_item, 0) == pdTRUE ? H2_PAL_OK
            : queue->closed ? H2_PAL_ERR_CLOSED : H2_PAL_ERR_TIMEOUT;
        (void)xSemaphoreGive(queue->lock);
        if (result != H2_PAL_ERR_TIMEOUT) {
            if (result == H2_PAL_OK) (void)xSemaphoreGive(queue->changed);
            return result;
        }
        result = queue_wait_changed(queue, started, timeout_ms);
        if (result != H2_PAL_OK) return result;
    }
}

static int esp_queue_reset(void *user, h2_pal_queue_t *queue) {
    (void)user;
    if (queue == NULL) return H2_PAL_ERR_INVALID_ARG;
    (void)xSemaphoreTake(queue->lock, portMAX_DELAY);
    int result = xQueueReset(queue->handle) == pdTRUE ? H2_PAL_OK : H2_PAL_ERR_IO;
    (void)xSemaphoreGive(queue->lock);
    (void)xSemaphoreGive(queue->changed);
    return result;
}

static int esp_queue_close(void *user, h2_pal_queue_t *queue) {
    (void)user;
    if (queue == NULL) return H2_PAL_ERR_INVALID_ARG;
    (void)xSemaphoreTake(queue->lock, portMAX_DELAY);
    queue->closed = 1;
    (void)xSemaphoreGive(queue->lock);
    (void)xSemaphoreGive(queue->changed);
    return H2_PAL_OK;
}

const h2_pal_queue_api_t *h2_esp_platform_queue_api(void) {
    static const h2_pal_queue_vtable_t vtable = {
        .create = esp_queue_create,
        .destroy = esp_queue_destroy,
        .send = esp_queue_send,
        .send_latest = esp_queue_send_latest,
        .recv = esp_queue_recv,
        .reset = esp_queue_reset,
        .close = esp_queue_close,
    };
    static const h2_pal_queue_api_t api = {
        .user = NULL,
        .vtable = &vtable,
    };
    return &api;
}
