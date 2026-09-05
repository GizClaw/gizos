#include "h2_jieli_wl82_platform_core.h"
#include "h2_jieli_wl82_sdk_port.h"

#include <string.h>

/* The ring and its predicates have one owner: lock. Conditions only notify
 * waiters to recheck those predicates; no slot is reserved outside the lock.
 * In particular reset cannot invalidate a receiver's semaphore reservation. */
struct h2_pal_queue {
    const h2_pal_sync_api_t *sync;
    h2_pal_mutex_t *lock;
    h2_pal_cond_t *readable;
    h2_pal_cond_t *writable;
    uint8_t *storage;
    size_t item_size;
    size_t capacity;
    size_t head;
    size_t count;
    int closed;
};

static void queue_release(h2_pal_queue_t *queue)
{
    if (queue->readable != NULL) (void)h2_pal_cond_destroy(queue->sync, queue->readable);
    if (queue->writable != NULL) (void)h2_pal_cond_destroy(queue->sync, queue->writable);
    if (queue->lock != NULL) (void)h2_pal_mutex_destroy(queue->sync, queue->lock);
    if (queue->storage != NULL) h2_jieli_sdk_free(queue->storage);
    h2_jieli_sdk_free(queue);
}

static int queue_create(void *user, const h2_pal_queue_config_t *config, h2_pal_queue_t **out_queue)
{
    (void)user;
    if (config == NULL || out_queue == NULL || config->item_size == 0u || config->item_count == 0u) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_queue = NULL;
    if (config->item_size > SIZE_MAX / config->item_count) return H2_PAL_ERR_INVALID_ARG;
    h2_pal_queue_t *queue = h2_jieli_sdk_malloc(sizeof(*queue));
    if (queue == NULL) return H2_PAL_ERR_NO_MEMORY;
    memset(queue, 0, sizeof(*queue));
    queue->sync = h2_jieli_wl82_platform_sync_api();
    queue->item_size = config->item_size;
    queue->capacity = config->item_count;
    queue->storage = h2_jieli_sdk_malloc(config->item_size * config->item_count);
    const h2_pal_mutex_config_t mutex = {.name = config->name};
    const h2_pal_cond_config_t cond = {.name = config->name};
    int rc = H2_PAL_ERR_NO_MEMORY;
    if (queue->storage != NULL) {
        rc = h2_pal_mutex_create(queue->sync, &mutex, &queue->lock);
        if (rc == H2_PAL_OK) rc = h2_pal_cond_create(queue->sync, &cond, &queue->readable);
        if (rc == H2_PAL_OK) rc = h2_pal_cond_create(queue->sync, &cond, &queue->writable);
    }
    if (rc != H2_PAL_OK) {
        queue_release(queue);
        return rc;
    }
    *out_queue = queue;
    return H2_PAL_OK;
}

/* Caller must close and join every queue user before destroy. */
static void queue_destroy(void *user, h2_pal_queue_t *queue)
{
    (void)user;
    if (queue != NULL) queue_release(queue);
}

static uint8_t *slot(h2_pal_queue_t *queue, size_t index)
{
    return queue->storage + ((index % queue->capacity) * queue->item_size);
}

/* Called with lock held. One deadline covers all wake/recheck iterations. */
static int queue_wait(h2_pal_queue_t *queue, h2_pal_cond_t *cond,
                      uint32_t started, uint32_t timeout_ms, int *locked)
{
    uint32_t remaining = timeout_ms;
    if (timeout_ms != H2_PAL_QUEUE_WAIT_FOREVER) {
        uint32_t elapsed = h2_jieli_sdk_time_ms() - started;
        if (elapsed >= timeout_ms) return H2_PAL_ERR_TIMEOUT;
        remaining -= elapsed;
    }
    return h2_jieli_wl82_cond_wait_owned(cond, queue->lock, remaining, locked);
}

static int queue_send(void *user, h2_pal_queue_t *queue, const void *item, uint32_t timeout_ms)
{
    (void)user;
    if (queue == NULL || item == NULL) return H2_PAL_ERR_INVALID_ARG;
    uint32_t started = h2_jieli_sdk_time_ms();
    int rc = h2_pal_mutex_lock(queue->sync, queue->lock);
    if (rc != H2_PAL_OK) return rc;
    int locked = 1;
    while (!queue->closed && queue->count == queue->capacity) {
        if (timeout_ms == 0u) {
            rc = H2_PAL_ERR_FULL;
            goto done;
        }
        rc = queue_wait(queue, queue->writable, started, timeout_ms, &locked);
        if (rc != H2_PAL_OK) goto done;
    }
    if (queue->closed) {
        rc = H2_PAL_ERR_CLOSED;
        goto done;
    }
    memcpy(slot(queue, queue->head + queue->count), item, queue->item_size);
    ++queue->count;
    (void)h2_pal_cond_signal(queue->sync, queue->readable);
done:
    if (locked) (void)h2_pal_mutex_unlock(queue->sync, queue->lock);
    return rc;
}

static int queue_send_latest(void *user, h2_pal_queue_t *queue, const void *item)
{
    (void)user;
    if (queue == NULL || item == NULL) return H2_PAL_ERR_INVALID_ARG;
    int rc = h2_pal_mutex_lock(queue->sync, queue->lock);
    if (rc != H2_PAL_OK) return rc;
    if (queue->closed) {
        rc = H2_PAL_ERR_CLOSED;
    } else if (queue->count == queue->capacity) {
        memcpy(slot(queue, queue->head + queue->count - 1u), item, queue->item_size);
    } else {
        memcpy(slot(queue, queue->head + queue->count), item, queue->item_size);
        ++queue->count;
        (void)h2_pal_cond_signal(queue->sync, queue->readable);
    }
    (void)h2_pal_mutex_unlock(queue->sync, queue->lock);
    return rc;
}

static int queue_recv(void *user, h2_pal_queue_t *queue, void *out_item, uint32_t timeout_ms)
{
    (void)user;
    if (queue == NULL || out_item == NULL) return H2_PAL_ERR_INVALID_ARG;
    uint32_t started = h2_jieli_sdk_time_ms();
    int rc = h2_pal_mutex_lock(queue->sync, queue->lock);
    if (rc != H2_PAL_OK) return rc;
    int locked = 1;
    while (queue->count == 0u) {
        if (queue->closed) {
            rc = H2_PAL_ERR_CLOSED;
            goto done;
        }
        rc = queue_wait(queue, queue->readable, started, timeout_ms, &locked);
        if (rc != H2_PAL_OK) goto done;
    }
    memcpy(out_item, slot(queue, queue->head), queue->item_size);
    queue->head = (queue->head + 1u) % queue->capacity;
    --queue->count;
    (void)h2_pal_cond_signal(queue->sync, queue->writable);
done:
    if (locked) (void)h2_pal_mutex_unlock(queue->sync, queue->lock);
    return rc;
}

static int queue_reset(void *user, h2_pal_queue_t *queue)
{
    (void)user;
    if (queue == NULL) return H2_PAL_ERR_INVALID_ARG;
    int rc = h2_pal_mutex_lock(queue->sync, queue->lock);
    if (rc != H2_PAL_OK) return rc;
    queue->head = 0u;
    queue->count = 0u;
    rc = h2_pal_cond_broadcast(queue->sync, queue->writable);
    (void)h2_pal_mutex_unlock(queue->sync, queue->lock);
    return rc;
}

static int queue_close(void *user, h2_pal_queue_t *queue)
{
    (void)user;
    if (queue == NULL) return H2_PAL_ERR_INVALID_ARG;
    int rc = h2_pal_mutex_lock(queue->sync, queue->lock);
    if (rc != H2_PAL_OK) return rc;
    queue->closed = 1;
    rc = h2_pal_cond_broadcast(queue->sync, queue->readable);
    int write_rc = h2_pal_cond_broadcast(queue->sync, queue->writable);
    (void)h2_pal_mutex_unlock(queue->sync, queue->lock);
    return rc == H2_PAL_OK ? write_rc : rc;
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
