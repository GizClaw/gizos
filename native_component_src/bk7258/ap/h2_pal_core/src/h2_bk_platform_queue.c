#include "h2_bk_platform_core.h"

#include <os/mem.h>
#include <os/os.h>

#include <stdint.h>
#include <string.h>

#include "FreeRTOS.h"
#include "queue.h"
#include "semphr.h"
#include <driver/aon_rtc.h>
#include "h2_bk_resource_stats_internal.h"

#define H2_BK_QUEUE_CLOSE_POLL_MS 10u

struct h2_pal_queue {
    beken_queue_t handle;
    StaticQueue_t control;
    const h2_pal_mem_api_t *allocator;
    size_t item_size;
    uint8_t *storage;
    void *reset_scratch;
    int closed;
    SemaphoreHandle_t lock, changed;
    StaticSemaphore_t lock_storage, changed_storage;
};

static void *queue_alloc(const h2_pal_mem_api_t *allocator, size_t len) {
    return allocator != NULL ? h2_pal_mem_alloc(allocator, len) : os_malloc(len);
}

static void queue_free(const h2_pal_mem_api_t *allocator, void *ptr) {
    if (allocator != NULL) {
        h2_pal_mem_free(allocator, ptr);
    } else if (ptr != NULL) {
        os_free(ptr);
    }
}

static int bk_queue_create(void *user, const h2_pal_queue_config_t *config, h2_pal_queue_t **out_queue) {
    (void)user;
    if (config == NULL || out_queue == NULL || config->item_size == 0u || config->item_count == 0u) {
        return H2_PAL_QUEUE_ERR_INVALID_ARG;
    }
    *out_queue = NULL;

    if (config->item_count > SIZE_MAX / config->item_size ||
        config->item_count > UINT32_MAX ||
        config->item_size > UINT32_MAX) {
        return H2_PAL_QUEUE_ERR_INVALID_ARG;
    }
    h2_pal_queue_t *queue =
        (h2_pal_queue_t *)queue_alloc(config->allocator, sizeof(*queue));
    if (queue == NULL) {
        return H2_PAL_QUEUE_ERR_NO_MEMORY;
    }
    os_memset(queue, 0, sizeof(*queue));
    queue->allocator = config->allocator;
    queue->item_size = config->item_size;
    queue->storage = (uint8_t *)queue_alloc(
        config->allocator, config->item_size * config->item_count);
    if (queue->storage == NULL) {
        queue_free(config->allocator, queue);
        return H2_PAL_QUEUE_ERR_NO_MEMORY;
    }
    queue->reset_scratch = queue_alloc(config->allocator, config->item_size);
    if (queue->reset_scratch == NULL) {
        queue_free(config->allocator, queue->storage);
        queue_free(config->allocator, queue);
        return H2_PAL_QUEUE_ERR_NO_MEMORY;
    }
    queue->handle = (beken_queue_t)xQueueCreateStatic(
        (UBaseType_t)config->item_count,
        (UBaseType_t)config->item_size,
        queue->storage,
        &queue->control);
    if (queue->handle == NULL) {
        queue_free(config->allocator, queue->reset_scratch);
        queue_free(config->allocator, queue->storage);
        queue_free(config->allocator, queue);
        return H2_PAL_QUEUE_ERR_NO_MEMORY;
    }
    queue->lock = xSemaphoreCreateMutexStatic(&queue->lock_storage);
    queue->changed = xSemaphoreCreateBinaryStatic(&queue->changed_storage);
    if (queue->lock == NULL || queue->changed == NULL) {
        if (queue->lock != NULL) vSemaphoreDelete(queue->lock);
        if (queue->changed != NULL) vSemaphoreDelete(queue->changed);
        (void)rtos_deinit_queue(&queue->handle);
        queue_free(config->allocator, queue->reset_scratch);
        queue_free(config->allocator, queue->storage);
        queue_free(config->allocator, queue);
        return H2_PAL_ERR_NO_MEMORY;
    }
    h2_bk_resource_acquire(H2_BK_RESOURCE_QUEUE, 0u);
    *out_queue = queue;
    return H2_PAL_QUEUE_OK;
}

static void bk_queue_destroy(void *user, h2_pal_queue_t *queue) {
    (void)user;
    if (queue == NULL) {
        return;
    }
    if (queue->handle != NULL) {
        (void)rtos_deinit_queue(&queue->handle);
    }
    vSemaphoreDelete(queue->lock);
    vSemaphoreDelete(queue->changed);
    const h2_pal_mem_api_t *allocator = queue->allocator;
    queue_free(allocator, queue->reset_scratch);
    queue_free(allocator, queue->storage);
    queue_free(allocator, queue);
    h2_bk_resource_release(H2_BK_RESOURCE_QUEUE, 0u);
}

/* Queue operations use nonblocking kernel access under the admission lock.
 * Waiting outside it allows close to reject all pending senders without
 * resetting/discarding buffered data. A change signal gives fast progress;
 * bounded slices also wake every waiter when signals coalesce. */
static int queue_wait_changed(h2_pal_queue_t *queue, uint64_t started, uint32_t timeout_ms) {
    uint32_t slice = H2_BK_QUEUE_CLOSE_POLL_MS;
    if (timeout_ms != H2_PAL_QUEUE_WAIT_FOREVER) {
        uint64_t elapsed = (uint64_t)bk_aon_rtc_get_us() / 1000u - started;
        if (elapsed >= timeout_ms) return H2_PAL_ERR_TIMEOUT;
        uint32_t remaining = timeout_ms - (uint32_t)elapsed;
        if (slice > remaining) slice = remaining;
    }
    TickType_t ticks = (TickType_t)(((uint64_t)slice * configTICK_RATE_HZ + 999u) / 1000u);
    if (ticks == 0) ticks = 1;
    (void)xSemaphoreTake(queue->changed, ticks);
    return H2_PAL_OK;
}

static int bk_queue_send(void *user, h2_pal_queue_t *queue, const void *item, uint32_t timeout_ms) {
    (void)user;
    if (queue == NULL || item == NULL) return H2_PAL_ERR_INVALID_ARG;
    uint64_t started = (uint64_t)bk_aon_rtc_get_us() / 1000u;
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

static int bk_queue_send_latest(void *user, h2_pal_queue_t *queue, const void *item) {
    (void)user;
    if (queue == NULL || item == NULL) return H2_PAL_ERR_INVALID_ARG;
    (void)xSemaphoreTake(queue->lock, portMAX_DELAY);
    int result = H2_PAL_OK;
    if (queue->closed) result = H2_PAL_ERR_CLOSED;
    else if (xQueueSend(queue->handle, item, 0) != pdTRUE) {
        (void)xQueueReceive(queue->handle, queue->reset_scratch, 0);
        if (xQueueSend(queue->handle, item, 0) != pdTRUE) result = H2_PAL_ERR_IO;
    }
    (void)xSemaphoreGive(queue->lock);
    if (result == H2_PAL_OK) (void)xSemaphoreGive(queue->changed);
    return result;
}

static int bk_queue_recv(void *user, h2_pal_queue_t *queue, void *out_item, uint32_t timeout_ms) {
    (void)user;
    if (queue == NULL || out_item == NULL) return H2_PAL_ERR_INVALID_ARG;
    uint64_t started = (uint64_t)bk_aon_rtc_get_us() / 1000u;
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

static int bk_queue_reset(void *user, h2_pal_queue_t *queue) {
    (void)user;
    if (queue == NULL) return H2_PAL_ERR_INVALID_ARG;
    (void)xSemaphoreTake(queue->lock, portMAX_DELAY);
    int result = xQueueReset(queue->handle) == pdTRUE ? H2_PAL_OK : H2_PAL_ERR_IO;
    (void)xSemaphoreGive(queue->lock);
    (void)xSemaphoreGive(queue->changed);
    return result;
}

static int bk_queue_close(void *user, h2_pal_queue_t *queue) {
    (void)user;
    if (queue == NULL) return H2_PAL_ERR_INVALID_ARG;
    (void)xSemaphoreTake(queue->lock, portMAX_DELAY);
    queue->closed = 1;
    (void)xSemaphoreGive(queue->lock);
    (void)xSemaphoreGive(queue->changed);
    return H2_PAL_OK;
}

const h2_pal_queue_api_t *h2_bk_platform_queue_api(void) {
    static const h2_pal_queue_vtable_t vtable = {
        .create = bk_queue_create,
        .destroy = bk_queue_destroy,
        .send = bk_queue_send,
        .send_latest = bk_queue_send_latest,
        .recv = bk_queue_recv,
        .reset = bk_queue_reset,
        .close = bk_queue_close,
    };
    static const h2_pal_queue_api_t api = {
        .user = NULL,
        .vtable = &vtable,
    };
    return &api;
}
