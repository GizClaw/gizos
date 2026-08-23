#include "h2_windows_internal.h"

#include <string.h>

struct h2_pal_queue {
    h2_windows_platform_t *platform;
    const h2_pal_mem_api_t *allocator;
    CRITICAL_SECTION lock;
    CONDITION_VARIABLE not_empty;
    CONDITION_VARIABLE not_full;
    uint8_t *items;
    size_t item_size;
    size_t item_count;
    size_t head;
    size_t count;
    LONG waiters;
    int closed;
};

static void *queue_alloc(const h2_pal_mem_api_t *allocator, size_t size) {
    return allocator == NULL ? h2_windows_heap_alloc(size)
                             : h2_pal_mem_alloc(allocator, size);
}

static void queue_free(const h2_pal_mem_api_t *allocator, void *memory) {
    if (allocator == NULL) {
        h2_windows_heap_free(memory);
    } else {
        h2_pal_mem_free(allocator, memory);
    }
}

static int queue_wait(h2_pal_queue_t *queue,
                      CONDITION_VARIABLE *condition,
                      uint32_t timeout_ms) {
    if (timeout_ms == H2_PAL_QUEUE_NO_WAIT) {
        return H2_PAL_ERR_TIMEOUT;
    }
    (void)InterlockedIncrement(&queue->waiters);
    BOOL waited = SleepConditionVariableCS(
        condition, &queue->lock,
        timeout_ms == H2_PAL_QUEUE_WAIT_FOREVER ? INFINITE : timeout_ms);
    (void)InterlockedDecrement(&queue->waiters);
    if (waited) {
        return H2_PAL_OK;
    }
    DWORD error = GetLastError();
    return error == ERROR_TIMEOUT ? H2_PAL_ERR_TIMEOUT
                                  : h2_windows_error_from_win32(error);
}

static uint32_t queue_remaining(h2_pal_queue_t *queue, uint64_t deadline) {
    uint64_t now = 0u;
    if (h2_windows_monotonic_ms(queue->platform, &now) != H2_PAL_OK ||
        now >= deadline) {
        return 0u;
    }
    uint64_t remaining = deadline - now;
    return remaining > UINT32_MAX ? UINT32_MAX : (uint32_t)remaining;
}

static int windows_queue_create(void *user,
                                const h2_pal_queue_config_t *config,
                                h2_pal_queue_t **out_queue) {
    h2_windows_platform_t *platform = user;
    if (config == NULL || out_queue == NULL || config->item_size == 0u ||
        config->item_count == 0u ||
        config->item_count > SIZE_MAX / config->item_size) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_queue = NULL;
    h2_pal_queue_t *queue = queue_alloc(config->allocator, sizeof(*queue));
    if (queue == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    memset(queue, 0, sizeof(*queue));
    queue->items = queue_alloc(
        config->allocator, config->item_count * config->item_size);
    if (queue->items == NULL) {
        queue_free(config->allocator, queue);
        return H2_PAL_ERR_NO_MEMORY;
    }
    queue->platform = platform;
    queue->allocator = config->allocator;
    queue->item_size = config->item_size;
    queue->item_count = config->item_count;
    InitializeCriticalSection(&queue->lock);
    InitializeConditionVariable(&queue->not_empty);
    InitializeConditionVariable(&queue->not_full);
    h2_windows_object_acquire(platform);
    *out_queue = queue;
    return H2_PAL_OK;
}

static void windows_queue_destroy(void *user, h2_pal_queue_t *queue) {
    h2_windows_platform_t *platform = user;
    if (queue == NULL || queue->platform != platform) {
        return;
    }
    EnterCriticalSection(&queue->lock);
    queue->closed = 1;
    WakeAllConditionVariable(&queue->not_empty);
    WakeAllConditionVariable(&queue->not_full);
    while (InterlockedCompareExchange(&queue->waiters, 0, 0) != 0) {
        LeaveCriticalSection(&queue->lock);
        Sleep(0u);
        EnterCriticalSection(&queue->lock);
    }
    LeaveCriticalSection(&queue->lock);
    DeleteCriticalSection(&queue->lock);
    const h2_pal_mem_api_t *allocator = queue->allocator;
    queue_free(allocator, queue->items);
    queue_free(allocator, queue);
    h2_windows_object_release(platform);
}

static int windows_queue_send(void *user, h2_pal_queue_t *queue,
                              const void *item, uint32_t timeout_ms) {
    if (queue == NULL || queue->platform != user || item == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    EnterCriticalSection(&queue->lock);
    int result = H2_PAL_OK;
    uint64_t now = 0u;
    uint64_t deadline = UINT64_MAX;
    if (timeout_ms != H2_PAL_QUEUE_NO_WAIT &&
        timeout_ms != H2_PAL_QUEUE_WAIT_FOREVER) {
        result = h2_windows_monotonic_ms(queue->platform, &now);
        deadline = UINT64_MAX - now < timeout_ms ? UINT64_MAX
                                                 : now + timeout_ms;
    }
    while (!queue->closed && queue->count == queue->item_count &&
           result == H2_PAL_OK) {
        uint32_t remaining = timeout_ms;
        if (timeout_ms != H2_PAL_QUEUE_NO_WAIT &&
            timeout_ms != H2_PAL_QUEUE_WAIT_FOREVER) {
            remaining = queue_remaining(queue, deadline);
            if (remaining == 0u) {
                result = H2_PAL_ERR_TIMEOUT;
                break;
            }
        }
        result = queue_wait(queue, &queue->not_full, remaining);
    }
    if (result == H2_PAL_OK && queue->closed) {
        result = H2_PAL_ERR_CLOSED;
    }
    if (result == H2_PAL_OK) {
        size_t tail = (queue->head + queue->count) % queue->item_count;
        memcpy(queue->items + tail * queue->item_size, item, queue->item_size);
        ++queue->count;
        WakeConditionVariable(&queue->not_empty);
    }
    LeaveCriticalSection(&queue->lock);
    return result;
}

static int windows_queue_send_latest(void *user, h2_pal_queue_t *queue,
                                     const void *item) {
    if (queue == NULL || queue->platform != user || item == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    EnterCriticalSection(&queue->lock);
    if (queue->closed) {
        LeaveCriticalSection(&queue->lock);
        return H2_PAL_ERR_CLOSED;
    }
    if (queue->count == queue->item_count) {
        queue->head = (queue->head + 1u) % queue->item_count;
        --queue->count;
    }
    size_t tail = (queue->head + queue->count) % queue->item_count;
    memcpy(queue->items + tail * queue->item_size, item, queue->item_size);
    ++queue->count;
    WakeConditionVariable(&queue->not_empty);
    LeaveCriticalSection(&queue->lock);
    return H2_PAL_OK;
}

static int windows_queue_recv(void *user, h2_pal_queue_t *queue,
                              void *out_item, uint32_t timeout_ms) {
    if (queue == NULL || queue->platform != user || out_item == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    EnterCriticalSection(&queue->lock);
    int result = H2_PAL_OK;
    uint64_t now = 0u;
    uint64_t deadline = UINT64_MAX;
    if (timeout_ms != H2_PAL_QUEUE_NO_WAIT &&
        timeout_ms != H2_PAL_QUEUE_WAIT_FOREVER) {
        result = h2_windows_monotonic_ms(queue->platform, &now);
        deadline = UINT64_MAX - now < timeout_ms ? UINT64_MAX
                                                 : now + timeout_ms;
    }
    while (!queue->closed && queue->count == 0u && result == H2_PAL_OK) {
        uint32_t remaining = timeout_ms;
        if (timeout_ms != H2_PAL_QUEUE_NO_WAIT &&
            timeout_ms != H2_PAL_QUEUE_WAIT_FOREVER) {
            remaining = queue_remaining(queue, deadline);
            if (remaining == 0u) {
                result = H2_PAL_ERR_TIMEOUT;
                break;
            }
        }
        result = queue_wait(queue, &queue->not_empty, remaining);
    }
    if (result == H2_PAL_OK && queue->count == 0u && queue->closed) {
        result = H2_PAL_ERR_CLOSED;
    }
    if (result == H2_PAL_OK) {
        memcpy(out_item, queue->items + queue->head * queue->item_size,
               queue->item_size);
        queue->head = (queue->head + 1u) % queue->item_count;
        --queue->count;
        WakeConditionVariable(&queue->not_full);
    }
    LeaveCriticalSection(&queue->lock);
    return result;
}

static int windows_queue_reset(void *user, h2_pal_queue_t *queue) {
    if (queue == NULL || queue->platform != user) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    EnterCriticalSection(&queue->lock);
    queue->head = 0u;
    queue->count = 0u;
    WakeAllConditionVariable(&queue->not_full);
    LeaveCriticalSection(&queue->lock);
    return H2_PAL_OK;
}

static int windows_queue_close(void *user, h2_pal_queue_t *queue) {
    if (queue == NULL || queue->platform != user) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    EnterCriticalSection(&queue->lock);
    queue->closed = 1;
    WakeAllConditionVariable(&queue->not_empty);
    WakeAllConditionVariable(&queue->not_full);
    LeaveCriticalSection(&queue->lock);
    return H2_PAL_OK;
}

const h2_pal_queue_vtable_t h2_windows_queue_vtable = {
    .create = windows_queue_create,
    .destroy = windows_queue_destroy,
    .send = windows_queue_send,
    .send_latest = windows_queue_send_latest,
    .recv = windows_queue_recv,
    .reset = windows_queue_reset,
    .close = windows_queue_close,
};
