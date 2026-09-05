#include "h2_jieli_wl82_platform_core.h"
#include "h2_jieli_wl82_sdk_port.h"
#include "h2_jieli_wl82_atomic.h"

#include <string.h>

/* Bounded FIFO: a heap ring buffer guarded by an SDK mutex, with two counting
 * semaphores tracking filled and free slots so blocking send/recv honour
 * millisecond timeouts without spinning. close() wakes every waiter. */
struct h2_pal_queue {
    h2_jieli_sdk_mutex_t *lock;
    h2_jieli_sdk_sem_t *items;
    h2_jieli_sdk_sem_t *space;
    uint8_t *storage;
    size_t item_size;
    size_t capacity;
    size_t head;
    size_t count;
    volatile uint32_t closed;
};

static void queue_release(h2_pal_queue_t *queue)
{
    if (queue->lock != NULL) {
        h2_jieli_sdk_mutex_destroy(queue->lock);
    }
    if (queue->items != NULL) {
        h2_jieli_sdk_sem_destroy(queue->items);
    }
    if (queue->space != NULL) {
        h2_jieli_sdk_sem_destroy(queue->space);
    }
    if (queue->storage != NULL) {
        h2_jieli_sdk_free(queue->storage);
    }
    h2_jieli_sdk_free(queue);
}

static int queue_create(void *user, const h2_pal_queue_config_t *config, h2_pal_queue_t **out_queue)
{
    h2_pal_queue_t *queue;
    (void)user;
    if (config == NULL || out_queue == NULL || config->item_size == 0u || config->item_count == 0u) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_queue = NULL;
    if (config->item_size > SIZE_MAX / config->item_count) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    queue = (h2_pal_queue_t *)h2_jieli_sdk_malloc(sizeof(*queue));
    if (queue == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    memset(queue, 0, sizeof(*queue));
    queue->item_size = config->item_size;
    queue->capacity = config->item_count;
    queue->storage = (uint8_t *)h2_jieli_sdk_malloc(config->item_size * config->item_count);
    queue->lock = h2_jieli_sdk_mutex_create();
    queue->items = h2_jieli_sdk_sem_create(0u);
    queue->space = h2_jieli_sdk_sem_create((uint32_t)config->item_count);
    if (queue->storage == NULL || queue->lock == NULL || queue->items == NULL || queue->space == NULL) {
        queue_release(queue);
        return H2_PAL_ERR_NO_MEMORY;
    }
    *out_queue = queue;
    return H2_PAL_OK;
}

static void queue_destroy(void *user, h2_pal_queue_t *queue)
{
    (void)user;
    if (queue != NULL) {
        queue_release(queue);
    }
}

static uint8_t *slot(h2_pal_queue_t *queue, size_t index)
{
    return queue->storage + ((index % queue->capacity) * queue->item_size);
}

static int queue_send(void *user, h2_pal_queue_t *queue, const void *item, uint32_t timeout_ms)
{
    int rc;
    (void)user;
    if (queue == NULL || item == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (h2_jieli_atomic_load_u32(&queue->closed)) {
        return H2_PAL_ERR_CLOSED;
    }
    rc = h2_jieli_sdk_sem_take(queue->space, timeout_ms);
    if (rc > 0) {
        return timeout_ms == 0u ? H2_PAL_ERR_FULL : H2_PAL_ERR_TIMEOUT;
    }
    if (rc < 0) {
        return H2_PAL_ERR_IO;
    }
    if (h2_jieli_sdk_mutex_lock(queue->lock, H2_JIELI_SDK_WAIT_FOREVER) != 0) {
        /* No slot was written: return the reservation taken above. */
        (void)h2_jieli_sdk_sem_give(queue->space);
        return H2_PAL_ERR_IO;
    }
    if (h2_jieli_atomic_load_u32(&queue->closed)) {
        (void)h2_jieli_sdk_mutex_unlock(queue->lock);
        (void)h2_jieli_sdk_sem_give(queue->space);
        return H2_PAL_ERR_CLOSED;
    }
    memcpy(slot(queue, queue->head + queue->count), item, queue->item_size);
    queue->count++;
    (void)h2_jieli_sdk_mutex_unlock(queue->lock);
    (void)h2_jieli_sdk_sem_give(queue->items);
    return H2_PAL_OK;
}

static int queue_send_latest(void *user, h2_pal_queue_t *queue, const void *item)
{
    (void)user;
    if (queue == NULL || item == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (h2_jieli_sdk_mutex_lock(queue->lock, H2_JIELI_SDK_WAIT_FOREVER) != 0) {
        return H2_PAL_ERR_IO;
    }
    if (h2_jieli_atomic_load_u32(&queue->closed)) {
        (void)h2_jieli_sdk_mutex_unlock(queue->lock);
        return H2_PAL_ERR_CLOSED;
    }
    if (queue->count == queue->capacity) {
        /* Coalesce: replace the newest pending item; counts are unchanged. */
        memcpy(slot(queue, queue->head + queue->count - 1u), item, queue->item_size);
        (void)h2_jieli_sdk_mutex_unlock(queue->lock);
        return H2_PAL_OK;
    }
    (void)h2_jieli_sdk_mutex_unlock(queue->lock);
    return queue_send(user, queue, item, 0u);
}

static int queue_recv(void *user, h2_pal_queue_t *queue, void *out_item, uint32_t timeout_ms)
{
    int rc;
    (void)user;
    if (queue == NULL || out_item == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    rc = h2_jieli_sdk_sem_take(queue->items, timeout_ms);
    if (rc > 0) {
        return h2_jieli_atomic_load_u32(&queue->closed)
            ? H2_PAL_ERR_CLOSED : H2_PAL_ERR_TIMEOUT;
    }
    if (rc < 0) {
        return H2_PAL_ERR_IO;
    }
    if (h2_jieli_sdk_mutex_lock(queue->lock, H2_JIELI_SDK_WAIT_FOREVER) != 0) {
        /* The queued item is untouched and must remain available to readers. */
        (void)h2_jieli_sdk_sem_give(queue->items);
        return H2_PAL_ERR_IO;
    }
    if (queue->count == 0u) {
        /* Woken by close() rather than by an item. */
        (void)h2_jieli_sdk_mutex_unlock(queue->lock);
        (void)h2_jieli_sdk_sem_give(queue->items);
        return h2_jieli_atomic_load_u32(&queue->closed)
            ? H2_PAL_ERR_CLOSED : H2_PAL_ERR_IO;
    }
    memcpy(out_item, slot(queue, queue->head), queue->item_size);
    queue->head = (queue->head + 1u) % queue->capacity;
    queue->count--;
    (void)h2_jieli_sdk_mutex_unlock(queue->lock);
    (void)h2_jieli_sdk_sem_give(queue->space);
    return H2_PAL_OK;
}

static int queue_reset(void *user, h2_pal_queue_t *queue)
{
    (void)user;
    if (queue == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (h2_jieli_sdk_mutex_lock(queue->lock, H2_JIELI_SDK_WAIT_FOREVER) != 0) {
        return H2_PAL_ERR_IO;
    }
    while (queue->count > 0u) {
        if (h2_jieli_sdk_sem_take(queue->items, 0u) != 0) {
            break;
        }
        queue->count--;
        (void)h2_jieli_sdk_sem_give(queue->space);
    }
    queue->head = 0u;
    queue->count = 0u;
    (void)h2_jieli_sdk_mutex_unlock(queue->lock);
    return H2_PAL_OK;
}

static int queue_close(void *user, h2_pal_queue_t *queue)
{
    (void)user;
    if (queue == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (h2_jieli_sdk_mutex_lock(queue->lock, H2_JIELI_SDK_WAIT_FOREVER) != 0) {
        return H2_PAL_ERR_IO;
    }
    if (h2_jieli_atomic_load_u32(&queue->closed)) {
        (void)h2_jieli_sdk_mutex_unlock(queue->lock);
        return H2_PAL_OK;
    }
    h2_jieli_atomic_store_u32(&queue->closed, 1u);
    (void)h2_jieli_sdk_mutex_unlock(queue->lock);
    /* Wake one blocked receiver and one blocked sender; each re-wakes the
     * next waiter after observing the closed flag. */
    (void)h2_jieli_sdk_sem_give(queue->items);
    (void)h2_jieli_sdk_sem_give(queue->space);
    return H2_PAL_OK;
}

static const h2_pal_queue_vtable_t s_queue_vtable = {
    .create = queue_create,
    .destroy = queue_destroy,
    .send = queue_send,
    .send_latest = queue_send_latest,
    .recv = queue_recv,
    .reset = queue_reset,
    .close = queue_close,
};

static const h2_pal_queue_api_t s_queue_api = {
    .user = NULL,
    .vtable = &s_queue_vtable,
};

const h2_pal_queue_api_t *h2_jieli_wl82_platform_queue_api(void)
{
    return &s_queue_api;
}
