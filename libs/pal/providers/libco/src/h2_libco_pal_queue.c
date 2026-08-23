#include "h2_libco_internal.h"

#include <limits.h>
#include <string.h>

struct h2_pal_queue {
    h2_libco_t *core;
    const h2_pal_mem_api_t *allocator;
    uint8_t *items;
    size_t item_size;
    size_t item_count;
    size_t head;
    size_t count;
    size_t waiting_senders;
    size_t waiting_receivers;
    uint64_t close_generation;
    bool closed;
    uint8_t not_empty_key;
    uint8_t not_full_key;
};

static uint32_t h2_libco_queue_remaining(
    h2_libco_t *core, uint64_t deadline_ms) {
    uint64_t now_ms = core->config.now_ms(core->config.user);
    uint64_t remaining;
    if (now_ms >= deadline_ms) {
        return 0u;
    }
    remaining = deadline_ms - now_ms;
    return remaining >= UINT32_MAX ? UINT32_MAX - 1u
                                   : (uint32_t)remaining;
}

static int h2_libco_queue_create(
    void *user,
    const h2_pal_queue_config_t *config,
    h2_pal_queue_t **out_queue) {
    h2_libco_t *core = user;
    h2_pal_queue_t *queue;
    size_t bytes;
    if (core == NULL || config == NULL || out_queue == NULL ||
        *out_queue != NULL || config->item_size == 0u ||
        config->item_count == 0u || config->allocator == NULL ||
        config->allocator->vtable == NULL ||
        config->allocator->vtable->alloc == NULL ||
        config->allocator->vtable->free == NULL ||
        config->item_count > SIZE_MAX / config->item_size ||
        (!h2_libco_internal_root_context(core) &&
         !h2_libco_internal_task_context(core))) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_queue = NULL;
    bytes = config->item_count * config->item_size;
    queue = h2_pal_mem_alloc(config->allocator, sizeof(*queue));
    if (queue == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    memset(queue, 0, sizeof(*queue));
    queue->items = h2_pal_mem_alloc(config->allocator, bytes);
    if (queue->items == NULL) {
        h2_pal_mem_free(config->allocator, queue);
        return H2_PAL_ERR_NO_MEMORY;
    }
    queue->core = core;
    queue->allocator = config->allocator;
    queue->item_size = config->item_size;
    queue->item_count = config->item_count;
    ++core->live_pal_objects;
    *out_queue = queue;
    return H2_PAL_OK;
}

static void h2_libco_queue_destroy(void *user, h2_pal_queue_t *queue) {
    h2_libco_t *core = user;
    const h2_pal_mem_api_t *allocator;
    if (core == NULL || queue == NULL || queue->core != core ||
        queue->waiting_senders != 0u || queue->waiting_receivers != 0u ||
        (!h2_libco_internal_root_context(core) &&
         !h2_libco_internal_task_context(core))) {
        return;
    }
    allocator = queue->allocator;
    h2_pal_mem_free(allocator, queue->items);
    --core->live_pal_objects;
    h2_pal_mem_free(allocator, queue);
}

static h2_pal_result_t h2_libco_queue_wait(
    h2_pal_queue_t *queue,
    uintptr_t key,
    uint32_t timeout_ms,
    uint64_t deadline_ms,
    size_t *waiter_count,
    uint64_t close_generation) {
    uint32_t remaining = timeout_ms;
    h2_libco_result_t result;
    if (timeout_ms != H2_PAL_QUEUE_WAIT_FOREVER) {
        remaining = h2_libco_queue_remaining(queue->core, deadline_ms);
        if (remaining == 0u) {
            return H2_PAL_ERR_TIMEOUT;
        }
    }
    ++*waiter_count;
    result = h2_libco_wait(queue->core, key, remaining);
    --*waiter_count;
    if (queue->close_generation != close_generation) {
        return H2_PAL_ERR_CLOSED;
    }
    return h2_libco_internal_to_pal(result);
}

static int h2_libco_queue_send(
    void *user,
    h2_pal_queue_t *queue,
    const void *item,
    uint32_t timeout_ms) {
    h2_libco_t *core = user;
    uint64_t generation;
    uint64_t deadline_ms = 0u;
    if (core == NULL || queue == NULL || queue->core != core || item == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (queue->closed) {
        return H2_PAL_ERR_CLOSED;
    }
    if (timeout_ms != H2_PAL_QUEUE_NO_WAIT &&
        !h2_libco_internal_task_context(core)) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    generation = queue->close_generation;
    if (timeout_ms != H2_PAL_QUEUE_NO_WAIT &&
        timeout_ms != H2_PAL_QUEUE_WAIT_FOREVER) {
        uint64_t now_ms = core->config.now_ms(core->config.user);
        deadline_ms = UINT64_MAX - now_ms < timeout_ms
                          ? UINT64_MAX
                          : now_ms + timeout_ms;
    }
    while (queue->count == queue->item_count) {
        h2_pal_result_t result;
        if (timeout_ms == H2_PAL_QUEUE_NO_WAIT) {
            return H2_PAL_ERR_TIMEOUT;
        }
        result = h2_libco_queue_wait(
            queue, (uintptr_t)&queue->not_full_key, timeout_ms, deadline_ms,
            &queue->waiting_senders, generation);
        if (result != H2_PAL_OK) {
            return result;
        }
        if (queue->closed) {
            return H2_PAL_ERR_CLOSED;
        }
    }
    size_t tail = (queue->head + queue->count) % queue->item_count;
    memcpy(queue->items + tail * queue->item_size, item, queue->item_size);
    ++queue->count;
    (void)h2_libco_wake(core, (uintptr_t)&queue->not_empty_key, 1u, NULL);
    return H2_PAL_OK;
}

static int h2_libco_queue_send_latest(
    void *user, h2_pal_queue_t *queue, const void *item) {
    h2_libco_t *core = user;
    size_t tail;
    if (core == NULL || queue == NULL || queue->core != core || item == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (queue->closed) {
        return H2_PAL_ERR_CLOSED;
    }
    if (queue->count == queue->item_count) {
        queue->head = (queue->head + 1u) % queue->item_count;
        --queue->count;
    }
    tail = (queue->head + queue->count) % queue->item_count;
    memcpy(queue->items + tail * queue->item_size, item, queue->item_size);
    ++queue->count;
    (void)h2_libco_wake(core, (uintptr_t)&queue->not_empty_key, 1u, NULL);
    return H2_PAL_OK;
}

static int h2_libco_queue_recv(
    void *user,
    h2_pal_queue_t *queue,
    void *out_item,
    uint32_t timeout_ms) {
    h2_libco_t *core = user;
    uint64_t generation;
    uint64_t deadline_ms = 0u;
    if (core == NULL || queue == NULL || queue->core != core ||
        out_item == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (queue->closed) {
        return H2_PAL_ERR_CLOSED;
    }
    if (timeout_ms != H2_PAL_QUEUE_NO_WAIT &&
        !h2_libco_internal_task_context(core)) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    generation = queue->close_generation;
    if (timeout_ms != H2_PAL_QUEUE_NO_WAIT &&
        timeout_ms != H2_PAL_QUEUE_WAIT_FOREVER) {
        uint64_t now_ms = core->config.now_ms(core->config.user);
        deadline_ms = UINT64_MAX - now_ms < timeout_ms
                          ? UINT64_MAX
                          : now_ms + timeout_ms;
    }
    while (queue->count == 0u) {
        h2_pal_result_t result;
        if (timeout_ms == H2_PAL_QUEUE_NO_WAIT) {
            return H2_PAL_ERR_TIMEOUT;
        }
        result = h2_libco_queue_wait(
            queue, (uintptr_t)&queue->not_empty_key, timeout_ms, deadline_ms,
            &queue->waiting_receivers, generation);
        if (result != H2_PAL_OK) {
            return result;
        }
        if (queue->closed) {
            return H2_PAL_ERR_CLOSED;
        }
    }
    memcpy(out_item, queue->items + queue->head * queue->item_size,
           queue->item_size);
    queue->head = (queue->head + 1u) % queue->item_count;
    --queue->count;
    (void)h2_libco_wake(core, (uintptr_t)&queue->not_full_key, 1u, NULL);
    return H2_PAL_OK;
}

static int h2_libco_queue_reset(void *user, h2_pal_queue_t *queue) {
    h2_libco_t *core = user;
    if (core == NULL || queue == NULL || queue->core != core) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    queue->head = 0u;
    queue->count = 0u;
    queue->closed = false;
    (void)h2_libco_wake(core, (uintptr_t)&queue->not_full_key,
                        H2_LIBCO_WAKE_ALL, NULL);
    return H2_PAL_OK;
}

static int h2_libco_queue_close(void *user, h2_pal_queue_t *queue) {
    h2_libco_t *core = user;
    if (core == NULL || queue == NULL || queue->core != core) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (!queue->closed) {
        queue->closed = true;
        queue->head = 0u;
        queue->count = 0u;
        ++queue->close_generation;
        (void)h2_libco_wake(core, (uintptr_t)&queue->not_empty_key,
                            H2_LIBCO_WAKE_ALL, NULL);
        (void)h2_libco_wake(core, (uintptr_t)&queue->not_full_key,
                            H2_LIBCO_WAKE_ALL, NULL);
    }
    return H2_PAL_OK;
}

static const h2_pal_queue_vtable_t s_queue_vtable = {
    .create = h2_libco_queue_create,
    .destroy = h2_libco_queue_destroy,
    .send = h2_libco_queue_send,
    .send_latest = h2_libco_queue_send_latest,
    .recv = h2_libco_queue_recv,
    .reset = h2_libco_queue_reset,
    .close = h2_libco_queue_close,
};

const h2_pal_queue_vtable_t *h2_libco_internal_queue_vtable(void) {
    return &s_queue_vtable;
}
