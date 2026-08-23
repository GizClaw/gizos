#include "h2_esp_platform_core.h"

#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

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
    volatile int closed;
};

static uint32_t queue_wait_slice(uint32_t timeout_ms, uint32_t waited_ms) {
    if (timeout_ms == H2_PAL_QUEUE_WAIT_FOREVER) {
        return H2_ESP_QUEUE_CLOSE_POLL_MS;
    }
    if (timeout_ms <= waited_ms) {
        return 0u;
    }
    uint32_t remaining = timeout_ms - waited_ms;
    return remaining < H2_ESP_QUEUE_CLOSE_POLL_MS ? remaining : H2_ESP_QUEUE_CLOSE_POLL_MS;
}

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
    const h2_pal_mem_api_t *allocator = queue->allocator;
    heap_caps_free(queue->control);
    queue_free(allocator, queue->storage);
    queue_free(allocator, queue->drop_scratch);
    queue_free(allocator, queue);
}

static int esp_queue_send(void *user, h2_pal_queue_t *queue, const void *item, uint32_t timeout_ms) {
    (void)user;
    if (queue == NULL || queue->handle == NULL || item == NULL) {
        return H2_PAL_QUEUE_ERR_INVALID_ARG;
    }
    if (queue->closed) {
        return H2_PAL_QUEUE_ERR_CLOSED;
    }
    uint32_t waited_ms = 0u;
    for (;;) {
        uint32_t slice_ms = queue_wait_slice(timeout_ms, waited_ms);
        if (xQueueSend(queue->handle, item, pdMS_TO_TICKS(slice_ms)) == pdTRUE) {
            return H2_PAL_QUEUE_OK;
        }
        if (queue->closed) {
            return H2_PAL_QUEUE_ERR_CLOSED;
        }
        if (timeout_ms == H2_PAL_QUEUE_NO_WAIT || (timeout_ms != H2_PAL_QUEUE_WAIT_FOREVER && slice_ms == 0u)) {
            return H2_PAL_QUEUE_ERR_TIMEOUT;
        }
        waited_ms += slice_ms;
    }
}

static int esp_queue_send_latest(void *user, h2_pal_queue_t *queue, const void *item) {
    (void)user;
    if (queue == NULL || queue->handle == NULL || item == NULL || queue->drop_scratch == NULL) {
        return H2_PAL_QUEUE_ERR_INVALID_ARG;
    }
    if (queue->closed) {
        return H2_PAL_QUEUE_ERR_CLOSED;
    }
    if (queue->item_count == 1u) {
        return xQueueOverwrite(queue->handle, item) == pdTRUE
            ? H2_PAL_QUEUE_OK
            : H2_PAL_QUEUE_ERR_IO;
    }
    if (xQueueSend(queue->handle, item, 0) == pdTRUE) {
        return H2_PAL_QUEUE_OK;
    }
    (void)xQueueReceive(queue->handle, queue->drop_scratch, 0);
    return xQueueSend(queue->handle, item, 0) == pdTRUE ? H2_PAL_QUEUE_OK : H2_PAL_QUEUE_ERR_IO;
}

static int esp_queue_recv(void *user, h2_pal_queue_t *queue, void *out_item, uint32_t timeout_ms) {
    (void)user;
    if (queue == NULL || queue->handle == NULL || out_item == NULL) {
        return H2_PAL_QUEUE_ERR_INVALID_ARG;
    }
    uint32_t waited_ms = 0u;
    for (;;) {
        uint32_t slice_ms = queue_wait_slice(timeout_ms, waited_ms);
        if (xQueueReceive(queue->handle, out_item, pdMS_TO_TICKS(slice_ms)) == pdTRUE) {
            return H2_PAL_QUEUE_OK;
        }
        if (queue->closed) {
            return H2_PAL_QUEUE_ERR_CLOSED;
        }
        if (timeout_ms == H2_PAL_QUEUE_NO_WAIT || (timeout_ms != H2_PAL_QUEUE_WAIT_FOREVER && slice_ms == 0u)) {
            return H2_PAL_QUEUE_ERR_TIMEOUT;
        }
        waited_ms += slice_ms;
    }
}

static int esp_queue_reset(void *user, h2_pal_queue_t *queue) {
    (void)user;
    if (queue == NULL || queue->handle == NULL) {
        return H2_PAL_QUEUE_ERR_INVALID_ARG;
    }
    return xQueueReset(queue->handle) == pdTRUE ? H2_PAL_QUEUE_OK : H2_PAL_QUEUE_ERR_IO;
}

static int esp_queue_close(void *user, h2_pal_queue_t *queue) {
    (void)user;
    if (queue == NULL) {
        return H2_PAL_QUEUE_ERR_INVALID_ARG;
    }
    queue->closed = 1;
    if (queue->handle != NULL) {
        (void)xQueueReset(queue->handle);
    }
    return H2_PAL_QUEUE_OK;
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
