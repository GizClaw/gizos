#include "h2_libco_internal.h"

#include <limits.h>
#include <string.h>

struct h2_pal_mutex {
    h2_libco_t *core;
    const h2_pal_mem_api_t *allocator;
    h2_libco_task_t *owner;
    h2_libco_task_t *handoff;
    size_t waiters;
    uint32_t depth;
    bool recursive;
    uint8_t wait_key;
};

struct h2_pal_semaphore {
    h2_libco_t *core;
    const h2_pal_mem_api_t *allocator;
    size_t waiters;
    uint32_t grant_count;
    uint32_t count;
    uint32_t max_count;
    uint8_t wait_key;
};

struct h2_pal_cond {
    h2_libco_t *core;
    const h2_pal_mem_api_t *allocator;
    size_t waiters;
    uint64_t broadcast_generation;
    uint8_t wait_key;
};

static bool h2_libco_sync_context(const h2_libco_t *core) {
    return h2_libco_internal_root_context(core) ||
           h2_libco_internal_task_context(core);
}

static bool h2_libco_sync_allocator_valid(const h2_pal_mem_api_t *allocator) {
    return allocator != NULL && allocator->vtable != NULL &&
           allocator->vtable->alloc != NULL &&
           allocator->vtable->free != NULL;
}

static uint64_t h2_libco_sync_deadline(
    h2_libco_t *core, uint32_t timeout_ms) {
    uint64_t now_ms = core->config.now_ms(core->config.user);
    return UINT64_MAX - now_ms < timeout_ms ? UINT64_MAX
                                            : now_ms + timeout_ms;
}

static uint32_t h2_libco_sync_remaining(
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

static void h2_libco_mutex_wake_next(h2_pal_mutex_t *mutex) {
    h2_libco_task_t *task = NULL;
    (void)h2_libco_internal_wake_one(
        mutex->core, (uintptr_t)&mutex->wait_key, &task);
    mutex->handoff = task;
}

static h2_pal_result_t h2_libco_mutex_acquire(
    h2_pal_mutex_t *mutex, bool defer_cancel) {
    h2_libco_t *core = mutex->core;
    h2_libco_task_t *current = h2_libco_internal_current_task(core);
    if (current == NULL) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    if (mutex->owner == current) {
        if (!mutex->recursive) {
            return H2_PAL_ERR_BUSY;
        }
        if (mutex->depth == UINT32_MAX) {
            return H2_PAL_ERR_FULL;
        }
        ++mutex->depth;
        return H2_PAL_OK;
    }
    for (;;) {
        h2_libco_result_t result;
        if (mutex->owner == NULL &&
            (mutex->handoff == NULL || mutex->handoff == current)) {
            mutex->owner = current;
            mutex->handoff = NULL;
            mutex->depth = 1u;
            return H2_PAL_OK;
        }
        ++mutex->waiters;
        if (defer_cancel) {
            result = h2_libco_internal_wait_deferred_cancel(
                core, (uintptr_t)&mutex->wait_key);
        } else {
            result = h2_libco_wait(
                core, (uintptr_t)&mutex->wait_key, H2_LIBCO_WAIT_FOREVER);
        }
        --mutex->waiters;
        if (result != H2_LIBCO_OK && result != H2_LIBCO_WOKEN) {
            if (mutex->handoff == current) {
                mutex->handoff = NULL;
                h2_libco_mutex_wake_next(mutex);
            }
            return h2_libco_internal_to_pal(result);
        }
    }
}

static h2_pal_result_t h2_libco_mutex_release(h2_pal_mutex_t *mutex) {
    h2_libco_task_t *current =
        h2_libco_internal_current_task(mutex->core);
    if (current == NULL || mutex->owner != current) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    if (mutex->depth > 1u) {
        --mutex->depth;
        return H2_PAL_OK;
    }
    mutex->owner = NULL;
    mutex->depth = 0u;
    h2_libco_mutex_wake_next(mutex);
    return H2_PAL_OK;
}

static h2_pal_result_t h2_libco_sync_create_mutex(
    void *user,
    const h2_pal_mutex_config_t *config,
    h2_pal_mutex_t **out_mutex) {
    h2_libco_t *core = user;
    h2_pal_mutex_t *mutex;
    if (core == NULL || config == NULL || out_mutex == NULL ||
        *out_mutex != NULL || !h2_libco_sync_context(core) ||
        !h2_libco_sync_allocator_valid(config->allocator) ||
        (config->flags & ~H2_PAL_MUTEX_FLAG_RECURSIVE) != 0u) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_mutex = NULL;
    mutex = h2_pal_mem_alloc(config->allocator, sizeof(*mutex));
    if (mutex == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    memset(mutex, 0, sizeof(*mutex));
    mutex->core = core;
    mutex->allocator = config->allocator;
    mutex->recursive =
        (config->flags & H2_PAL_MUTEX_FLAG_RECURSIVE) != 0u;
    ++core->live_pal_objects;
    *out_mutex = mutex;
    return H2_PAL_OK;
}

static h2_pal_result_t h2_libco_sync_destroy_mutex(
    void *user, h2_pal_mutex_t *mutex) {
    h2_libco_t *core = user;
    const h2_pal_mem_api_t *allocator;
    if (core == NULL || mutex == NULL || mutex->core != core) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (mutex->owner != NULL || mutex->handoff != NULL ||
        mutex->waiters != 0u) {
        return H2_PAL_ERR_BUSY;
    }
    allocator = mutex->allocator;
    --core->live_pal_objects;
    h2_pal_mem_free(allocator, mutex);
    return H2_PAL_OK;
}

static h2_pal_result_t h2_libco_sync_lock_mutex(
    void *user, h2_pal_mutex_t *mutex) {
    h2_libco_t *core = user;
    if (core == NULL || mutex == NULL || mutex->core != core) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    return h2_libco_mutex_acquire(mutex, false);
}

static h2_pal_result_t h2_libco_sync_try_lock_mutex(
    void *user, h2_pal_mutex_t *mutex) {
    h2_libco_t *core = user;
    h2_libco_task_t *current;
    if (core == NULL || mutex == NULL || mutex->core != core) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    current = h2_libco_internal_current_task(core);
    if (current == NULL) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    if (mutex->owner == current) {
        if (!mutex->recursive) {
            return H2_PAL_ERR_BUSY;
        }
        if (mutex->depth == UINT32_MAX) {
            return H2_PAL_ERR_FULL;
        }
        ++mutex->depth;
        return H2_PAL_OK;
    }
    if (mutex->owner != NULL ||
        (mutex->handoff != NULL && mutex->handoff != current)) {
        return H2_PAL_ERR_BUSY;
    }
    mutex->owner = current;
    mutex->handoff = NULL;
    mutex->depth = 1u;
    return H2_PAL_OK;
}

static h2_pal_result_t h2_libco_sync_unlock_mutex(
    void *user, h2_pal_mutex_t *mutex) {
    h2_libco_t *core = user;
    if (core == NULL || mutex == NULL || mutex->core != core) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    return h2_libco_mutex_release(mutex);
}

static h2_pal_result_t h2_libco_sync_create_semaphore(
    void *user,
    const h2_pal_semaphore_config_t *config,
    h2_pal_semaphore_t **out_semaphore) {
    h2_libco_t *core = user;
    h2_pal_semaphore_t *semaphore;
    if (core == NULL || config == NULL || out_semaphore == NULL ||
        *out_semaphore != NULL || config->max_count == 0u ||
        config->initial_count > config->max_count ||
        !h2_libco_sync_context(core) ||
        !h2_libco_sync_allocator_valid(config->allocator)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_semaphore = NULL;
    semaphore = h2_pal_mem_alloc(config->allocator, sizeof(*semaphore));
    if (semaphore == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    memset(semaphore, 0, sizeof(*semaphore));
    semaphore->core = core;
    semaphore->allocator = config->allocator;
    semaphore->count = config->initial_count;
    semaphore->max_count = config->max_count;
    ++core->live_pal_objects;
    *out_semaphore = semaphore;
    return H2_PAL_OK;
}

static h2_pal_result_t h2_libco_sync_destroy_semaphore(
    void *user, h2_pal_semaphore_t *semaphore) {
    h2_libco_t *core = user;
    const h2_pal_mem_api_t *allocator;
    if (core == NULL || semaphore == NULL || semaphore->core != core) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (semaphore->waiters != 0u || semaphore->grant_count != 0u) {
        return H2_PAL_ERR_BUSY;
    }
    allocator = semaphore->allocator;
    --core->live_pal_objects;
    h2_pal_mem_free(allocator, semaphore);
    return H2_PAL_OK;
}

static void h2_libco_semaphore_reassign_grant(
    h2_pal_semaphore_t *semaphore) {
    h2_libco_task_t *task = NULL;
    (void)h2_libco_internal_wake_one(
        semaphore->core, (uintptr_t)&semaphore->wait_key, &task);
    if (task != NULL) {
        task->granted_key = (uintptr_t)&semaphore->wait_key;
    } else {
        --semaphore->grant_count;
        ++semaphore->count;
    }
}

static h2_pal_result_t h2_libco_sync_take_semaphore(
    void *user,
    h2_pal_semaphore_t *semaphore,
    uint32_t timeout_ms) {
    h2_libco_t *core = user;
    h2_libco_task_t *current;
    uint64_t deadline_ms = 0u;
    if (core == NULL || semaphore == NULL || semaphore->core != core) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    current = h2_libco_internal_current_task(core);
    if (current == NULL) {
        if (!h2_libco_internal_root_context(core)) {
            return H2_PAL_ERR_INVALID_STATE;
        }
        if (semaphore->count != 0u) {
            --semaphore->count;
            return H2_PAL_OK;
        }
        return timeout_ms == H2_PAL_SYNC_NO_WAIT
                   ? H2_PAL_ERR_TIMEOUT
                   : H2_PAL_ERR_INVALID_STATE;
    }
    if (timeout_ms != H2_PAL_SYNC_NO_WAIT &&
        timeout_ms != H2_PAL_SYNC_WAIT_FOREVER) {
        deadline_ms = h2_libco_sync_deadline(core, timeout_ms);
    }
    for (;;) {
        h2_libco_result_t result;
        uint32_t remaining = timeout_ms;
        if (current->granted_key == (uintptr_t)&semaphore->wait_key) {
            current->granted_key = 0u;
            --semaphore->grant_count;
            return H2_PAL_OK;
        }
        if (semaphore->count != 0u) {
            --semaphore->count;
            return H2_PAL_OK;
        }
        if (timeout_ms == H2_PAL_SYNC_NO_WAIT) {
            return H2_PAL_ERR_TIMEOUT;
        }
        if (timeout_ms != H2_PAL_SYNC_WAIT_FOREVER) {
            remaining = h2_libco_sync_remaining(core, deadline_ms);
            if (remaining == 0u) {
                return H2_PAL_ERR_TIMEOUT;
            }
        }
        ++semaphore->waiters;
        result = h2_libco_wait(
            core, (uintptr_t)&semaphore->wait_key, remaining);
        --semaphore->waiters;
        if (result != H2_LIBCO_OK && result != H2_LIBCO_WOKEN) {
            if (current->granted_key ==
                (uintptr_t)&semaphore->wait_key) {
                current->granted_key = 0u;
                h2_libco_semaphore_reassign_grant(semaphore);
            }
            return h2_libco_internal_to_pal(result);
        }
    }
}

static h2_pal_result_t h2_libco_sync_give_semaphore(
    void *user, h2_pal_semaphore_t *semaphore) {
    h2_libco_t *core = user;
    h2_libco_task_t *task = NULL;
    if (core == NULL || semaphore == NULL || semaphore->core != core ||
        !h2_libco_sync_context(core)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (semaphore->count + semaphore->grant_count ==
        semaphore->max_count) {
        return H2_PAL_ERR_FULL;
    }
    (void)h2_libco_internal_wake_one(
        core, (uintptr_t)&semaphore->wait_key, &task);
    if (task != NULL) {
        task->granted_key = (uintptr_t)&semaphore->wait_key;
        ++semaphore->grant_count;
    } else {
        ++semaphore->count;
    }
    return H2_PAL_OK;
}

static h2_pal_result_t h2_libco_sync_create_cond(
    void *user,
    const h2_pal_cond_config_t *config,
    h2_pal_cond_t **out_cond) {
    h2_libco_t *core = user;
    h2_pal_cond_t *cond;
    if (core == NULL || config == NULL || out_cond == NULL ||
        *out_cond != NULL || !h2_libco_sync_context(core) ||
        !h2_libco_sync_allocator_valid(config->allocator)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_cond = NULL;
    cond = h2_pal_mem_alloc(config->allocator, sizeof(*cond));
    if (cond == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    memset(cond, 0, sizeof(*cond));
    cond->core = core;
    cond->allocator = config->allocator;
    ++core->live_pal_objects;
    *out_cond = cond;
    return H2_PAL_OK;
}

static h2_pal_result_t h2_libco_sync_destroy_cond(
    void *user, h2_pal_cond_t *cond) {
    h2_libco_t *core = user;
    const h2_pal_mem_api_t *allocator;
    if (core == NULL || cond == NULL || cond->core != core) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (cond->waiters != 0u) {
        return H2_PAL_ERR_BUSY;
    }
    allocator = cond->allocator;
    --core->live_pal_objects;
    h2_pal_mem_free(allocator, cond);
    return H2_PAL_OK;
}

static h2_pal_result_t h2_libco_sync_wait_cond(
    void *user,
    h2_pal_cond_t *cond,
    h2_pal_mutex_t *mutex,
    uint32_t timeout_ms) {
    h2_libco_t *core = user;
    h2_libco_task_t *current;
    h2_pal_result_t pending = H2_PAL_OK;
    uint64_t broadcast_generation;
    uint64_t deadline_ms = 0u;
    if (core == NULL || cond == NULL || mutex == NULL ||
        cond->core != core || mutex->core != core) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    current = h2_libco_internal_current_task(core);
    if (current == NULL || mutex->owner != current || mutex->recursive ||
        mutex->depth != 1u) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    broadcast_generation = cond->broadcast_generation;
    if (timeout_ms != H2_PAL_SYNC_NO_WAIT &&
        timeout_ms != H2_PAL_SYNC_WAIT_FOREVER) {
        deadline_ms = h2_libco_sync_deadline(core, timeout_ms);
    }
    pending = h2_libco_mutex_release(mutex);
    if (pending != H2_PAL_OK) {
        return pending;
    }
    ++cond->waiters;
    while (cond->broadcast_generation == broadcast_generation &&
           current->granted_key != (uintptr_t)&cond->wait_key) {
        h2_libco_result_t result;
        uint32_t remaining = timeout_ms;
        if (timeout_ms == H2_PAL_SYNC_NO_WAIT) {
            pending = H2_PAL_ERR_TIMEOUT;
            break;
        }
        if (timeout_ms != H2_PAL_SYNC_WAIT_FOREVER) {
            remaining = h2_libco_sync_remaining(core, deadline_ms);
            if (remaining == 0u) {
                pending = H2_PAL_ERR_TIMEOUT;
                break;
            }
        }
        result = h2_libco_wait(core, (uintptr_t)&cond->wait_key, remaining);
        if (result != H2_LIBCO_OK && result != H2_LIBCO_WOKEN) {
            pending = h2_libco_internal_to_pal(result);
            break;
        }
    }
    if (current->granted_key == (uintptr_t)&cond->wait_key) {
        current->granted_key = 0u;
    }
    --cond->waiters;
    h2_pal_result_t reacquire = h2_libco_mutex_acquire(
        mutex, pending == H2_PAL_EXIT);
    return reacquire == H2_PAL_OK ? pending : reacquire;
}

static h2_pal_result_t h2_libco_sync_signal_cond(
    void *user, h2_pal_cond_t *cond) {
    h2_libco_t *core = user;
    if (core == NULL || cond == NULL || cond->core != core ||
        !h2_libco_sync_context(core)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    h2_libco_task_t *task = NULL;
    (void)h2_libco_internal_wake_one(
        core, (uintptr_t)&cond->wait_key, &task);
    if (task != NULL) {
        task->granted_key = (uintptr_t)&cond->wait_key;
    }
    return H2_PAL_OK;
}

static h2_pal_result_t h2_libco_sync_broadcast_cond(
    void *user, h2_pal_cond_t *cond) {
    h2_libco_t *core = user;
    if (core == NULL || cond == NULL || cond->core != core ||
        !h2_libco_sync_context(core)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    ++cond->broadcast_generation;
    (void)h2_libco_wake(core, (uintptr_t)&cond->wait_key,
                        H2_LIBCO_WAKE_ALL, NULL);
    return H2_PAL_OK;
}

static const h2_pal_sync_vtable_t s_sync_vtable = {
    .create_mutex = h2_libco_sync_create_mutex,
    .destroy_mutex = h2_libco_sync_destroy_mutex,
    .lock_mutex = h2_libco_sync_lock_mutex,
    .try_lock_mutex = h2_libco_sync_try_lock_mutex,
    .unlock_mutex = h2_libco_sync_unlock_mutex,
    .create_semaphore = h2_libco_sync_create_semaphore,
    .destroy_semaphore = h2_libco_sync_destroy_semaphore,
    .take_semaphore = h2_libco_sync_take_semaphore,
    .give_semaphore = h2_libco_sync_give_semaphore,
    .create_cond = h2_libco_sync_create_cond,
    .destroy_cond = h2_libco_sync_destroy_cond,
    .wait_cond = h2_libco_sync_wait_cond,
    .signal_cond = h2_libco_sync_signal_cond,
    .broadcast_cond = h2_libco_sync_broadcast_cond,
};

const h2_pal_sync_vtable_t *h2_libco_internal_sync_vtable(void) {
    return &s_sync_vtable;
}
