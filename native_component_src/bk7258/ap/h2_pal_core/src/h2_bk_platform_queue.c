#include "h2_bk_platform_core.h"

#include <os/mem.h>
#include <os/os.h>

#include <stdint.h>
#include <string.h>

#include "FreeRTOS.h"
#include "queue.h"

#define H2_BK_QUEUE_CLOSE_POLL_MS 10u

struct h2_pal_queue {
    beken_queue_t handle;
    StaticQueue_t control;
    const h2_pal_mem_api_t *allocator;
    size_t item_size;
    uint8_t *storage;
    void *reset_scratch;
    volatile int closed;
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

static uint32_t bk_timeout_slice(uint32_t timeout_ms, uint32_t waited_ms) {
    if (timeout_ms == H2_PAL_QUEUE_WAIT_FOREVER) {
        return H2_BK_QUEUE_CLOSE_POLL_MS;
    }
    if (timeout_ms <= waited_ms) {
        return 0u;
    }
    uint32_t remaining = timeout_ms - waited_ms;
    return remaining < H2_BK_QUEUE_CLOSE_POLL_MS ? remaining : H2_BK_QUEUE_CLOSE_POLL_MS;
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
    const h2_pal_mem_api_t *allocator = queue->allocator;
    queue_free(allocator, queue->reset_scratch);
    queue_free(allocator, queue->storage);
    queue_free(allocator, queue);
}

static int bk_queue_send(void *user, h2_pal_queue_t *queue, const void *item, uint32_t timeout_ms) {
    (void)user;
    if (queue == NULL || queue->handle == NULL || item == NULL) {
        return H2_PAL_QUEUE_ERR_INVALID_ARG;
    }
    if (queue->closed) {
        return H2_PAL_QUEUE_ERR_CLOSED;
    }
    uint32_t waited_ms = 0u;
    for (;;) {
        uint32_t slice_ms = bk_timeout_slice(timeout_ms, waited_ms);
        if (rtos_push_to_queue(&queue->handle, (void *)item, slice_ms) == kNoErr) {
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

static int bk_queue_send_latest(void *user, h2_pal_queue_t *queue, const void *item) {
    (void)user;
    if (queue == NULL || queue->handle == NULL || queue->reset_scratch == NULL || item == NULL) {
        return H2_PAL_QUEUE_ERR_INVALID_ARG;
    }
    if (queue->closed) {
        return H2_PAL_QUEUE_ERR_CLOSED;
    }
    if (rtos_push_to_queue(&queue->handle, (void *)item, BEKEN_NO_WAIT) == kNoErr) {
        return H2_PAL_QUEUE_OK;
    }
    (void)rtos_pop_from_queue(&queue->handle, queue->reset_scratch, BEKEN_NO_WAIT);
    return rtos_push_to_queue(&queue->handle, (void *)item, BEKEN_NO_WAIT) == kNoErr
        ? H2_PAL_QUEUE_OK
        : H2_PAL_QUEUE_ERR_IO;
}

static int bk_queue_recv(void *user, h2_pal_queue_t *queue, void *out_item, uint32_t timeout_ms) {
    (void)user;
    if (queue == NULL || queue->handle == NULL || out_item == NULL) {
        return H2_PAL_QUEUE_ERR_INVALID_ARG;
    }
    uint32_t waited_ms = 0u;
    for (;;) {
        uint32_t slice_ms = bk_timeout_slice(timeout_ms, waited_ms);
        if (rtos_pop_from_queue(&queue->handle, out_item, slice_ms) == kNoErr) {
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

static int bk_queue_reset(void *user, h2_pal_queue_t *queue) {
    (void)user;
    if (queue == NULL || queue->handle == NULL || queue->reset_scratch == NULL) {
        return H2_PAL_QUEUE_ERR_INVALID_ARG;
    }
    while (rtos_pop_from_queue(&queue->handle, queue->reset_scratch, BEKEN_NO_WAIT) == kNoErr) {
    }
    return H2_PAL_QUEUE_OK;
}

static int bk_queue_close(void *user, h2_pal_queue_t *queue) {
    (void)user;
    if (queue == NULL) {
        return H2_PAL_QUEUE_ERR_INVALID_ARG;
    }
    queue->closed = 1;
    (void)bk_queue_reset(NULL, queue);
    return H2_PAL_QUEUE_OK;
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
