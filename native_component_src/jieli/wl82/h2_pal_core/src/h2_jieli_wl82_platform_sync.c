#include "h2_jieli_wl82_platform_core.h"
#include "h2_jieli_wl82_sdk_port.h"
#include "h2_jieli_wl82_atomic.h"

struct h2_pal_mutex {
    h2_jieli_sdk_mutex_t *native;
    const void *owner;
    uint32_t depth;
    int recursive;
};

struct h2_pal_semaphore {
    h2_jieli_sdk_sem_t *native;
    /* os_sem has no ceiling, so the configured bound is tracked here; the
     * count is updated atomically because give/take may run from
     * different SDK tasks. */
    uint32_t max_count;
    volatile uint32_t count;
};

struct h2_pal_cond {
    h2_jieli_sdk_mutex_t *lock;
    h2_jieli_sdk_sem_t *wake;
    uint32_t waiters;
    uint32_t pending_wakes;
};

static h2_pal_result_t map_wait(int rc)
{
    if (rc == 0) {
        return H2_PAL_OK;
    }
    return rc > 0 ? H2_PAL_ERR_TIMEOUT : H2_PAL_ERR_IO;
}

static h2_pal_result_t sync_create_mutex(
    void *user,
    const h2_pal_mutex_config_t *config,
    h2_pal_mutex_t **out_mutex)
{
    h2_pal_mutex_t *mutex;
    (void)user;
    if (config == NULL || out_mutex == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_mutex = NULL;
    mutex = (h2_pal_mutex_t *)h2_jieli_sdk_malloc(sizeof(*mutex));
    if (mutex == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    mutex->native = h2_jieli_sdk_mutex_create();
    if (mutex->native == NULL) {
        h2_jieli_sdk_free(mutex);
        return H2_PAL_ERR_NO_MEMORY;
    }
    mutex->owner = NULL;
    mutex->depth = 0u;
    mutex->recursive =
        (config->flags & H2_PAL_MUTEX_FLAG_RECURSIVE) != 0u;
    *out_mutex = mutex;
    return H2_PAL_OK;
}

static h2_pal_result_t sync_destroy_mutex(void *user, h2_pal_mutex_t *mutex)
{
    (void)user;
    if (mutex == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    h2_jieli_sdk_mutex_destroy(mutex->native);
    h2_jieli_sdk_free(mutex);
    return H2_PAL_OK;
}

static h2_pal_result_t sync_lock_mutex(void *user, h2_pal_mutex_t *mutex)
{
    (void)user;
    if (mutex == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    const void *current = h2_jieli_sdk_task_current();
    if (mutex->recursive != 0 && current != NULL && mutex->owner == current) {
        mutex->depth++;
        return H2_PAL_OK;
    }
    h2_pal_result_t result = map_wait(
        h2_jieli_sdk_mutex_lock(mutex->native, H2_JIELI_SDK_WAIT_FOREVER));
    if (result == H2_PAL_OK && mutex->recursive != 0) {
        mutex->owner = current;
        mutex->depth = 1u;
    }
    return result;
}

static h2_pal_result_t sync_try_lock_mutex(void *user, h2_pal_mutex_t *mutex)
{
    int rc;
    (void)user;
    if (mutex == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    const void *current = h2_jieli_sdk_task_current();
    if (mutex->recursive != 0 && current != NULL && mutex->owner == current) {
        mutex->depth++;
        return H2_PAL_OK;
    }
    rc = h2_jieli_sdk_mutex_lock(mutex->native, 0u);
    if (rc == 0) {
        if (mutex->recursive != 0) {
            mutex->owner = current;
            mutex->depth = 1u;
        }
        return H2_PAL_OK;
    }
    return rc > 0 ? H2_PAL_ERR_WOULD_BLOCK : H2_PAL_ERR_IO;
}

static h2_pal_result_t sync_unlock_mutex(void *user, h2_pal_mutex_t *mutex)
{
    (void)user;
    if (mutex == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (mutex->recursive != 0) {
        const void *current = h2_jieli_sdk_task_current();
        if (mutex->depth == 0u || mutex->owner != current) {
            return H2_PAL_ERR_INVALID_STATE;
        }
        if (--mutex->depth != 0u) {
            return H2_PAL_OK;
        }
        mutex->owner = NULL;
    }
    return h2_jieli_sdk_mutex_unlock(mutex->native) == 0 ? H2_PAL_OK : H2_PAL_ERR_IO;
}

static h2_pal_result_t sync_create_semaphore(
    void *user,
    const h2_pal_semaphore_config_t *config,
    h2_pal_semaphore_t **out_semaphore)
{
    h2_pal_semaphore_t *semaphore;
    (void)user;
    if (config == NULL || out_semaphore == NULL || config->max_count == 0u ||
        config->initial_count > config->max_count) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_semaphore = NULL;
    semaphore = (h2_pal_semaphore_t *)h2_jieli_sdk_malloc(sizeof(*semaphore));
    if (semaphore == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    semaphore->native = h2_jieli_sdk_sem_create(config->initial_count);
    if (semaphore->native == NULL) {
        h2_jieli_sdk_free(semaphore);
        return H2_PAL_ERR_NO_MEMORY;
    }
    semaphore->max_count = config->max_count;
    semaphore->count = config->initial_count;
    *out_semaphore = semaphore;
    return H2_PAL_OK;
}

static h2_pal_result_t sync_destroy_semaphore(void *user, h2_pal_semaphore_t *semaphore)
{
    (void)user;
    if (semaphore == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    h2_jieli_sdk_sem_destroy(semaphore->native);
    h2_jieli_sdk_free(semaphore);
    return H2_PAL_OK;
}

static h2_pal_result_t sync_take_semaphore(
    void *user,
    h2_pal_semaphore_t *semaphore,
    uint32_t timeout_ms)
{
    h2_pal_result_t rc;
    (void)user;
    if (semaphore == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    rc = map_wait(h2_jieli_sdk_sem_take(semaphore->native, timeout_ms));
    if (rc == H2_PAL_OK) {
        (void)h2_jieli_atomic_fetch_sub_u32(&semaphore->count, 1u);
    }
    return rc;
}

static h2_pal_result_t sync_give_semaphore(void *user, h2_pal_semaphore_t *semaphore)
{
    uint32_t count;
    (void)user;
    if (semaphore == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    /* Reserve a slot below the configured ceiling before posting so a give
     * racing with another give cannot overshoot it. */
    count = h2_jieli_atomic_load_u32(&semaphore->count);
    for (;;) {
        if (count >= semaphore->max_count) {
            return H2_PAL_ERR_FULL;
        }
        if (h2_jieli_atomic_cas_u32(&semaphore->count, &count, count + 1u)) {
            break;
        }
    }
    if (h2_jieli_sdk_sem_give(semaphore->native) != 0) {
        (void)h2_jieli_atomic_fetch_sub_u32(&semaphore->count, 1u);
        return H2_PAL_ERR_IO;
    }
    return H2_PAL_OK;
}

static h2_pal_result_t sync_create_cond(
    void *user,
    const h2_pal_cond_config_t *config,
    h2_pal_cond_t **out_cond)
{
    h2_pal_cond_t *cond;
    (void)user;
    if (config == NULL || out_cond == NULL) return H2_PAL_ERR_INVALID_ARG;
    *out_cond = NULL;
    cond = (h2_pal_cond_t *)h2_jieli_sdk_malloc(sizeof(*cond));
    if (cond == NULL) return H2_PAL_ERR_NO_MEMORY;
    cond->lock = h2_jieli_sdk_mutex_create();
    cond->wake = h2_jieli_sdk_sem_create(0u);
    cond->waiters = 0u;
    cond->pending_wakes = 0u;
    if (cond->lock == NULL || cond->wake == NULL) {
        h2_jieli_sdk_mutex_destroy(cond->lock);
        h2_jieli_sdk_sem_destroy(cond->wake);
        h2_jieli_sdk_free(cond);
        return H2_PAL_ERR_NO_MEMORY;
    }
    *out_cond = cond;
    return H2_PAL_OK;
}

static h2_pal_result_t sync_destroy_cond(void *user, h2_pal_cond_t *cond)
{
    (void)user;
    if (cond == NULL) return H2_PAL_ERR_INVALID_ARG;
    h2_jieli_sdk_mutex_destroy(cond->lock);
    h2_jieli_sdk_sem_destroy(cond->wake);
    h2_jieli_sdk_free(cond);
    return H2_PAL_OK;
}

static h2_pal_result_t sync_wait_cond(
    void *user,
    h2_pal_cond_t *cond,
    h2_pal_mutex_t *mutex,
    uint32_t timeout_ms)
{
    h2_pal_result_t result;
    (void)user;
    if (cond == NULL || mutex == NULL) return H2_PAL_ERR_INVALID_ARG;
    if (h2_jieli_sdk_mutex_lock(cond->lock, H2_JIELI_SDK_WAIT_FOREVER) != 0) {
        return H2_PAL_ERR_IO;
    }
    cond->waiters++;
    (void)h2_jieli_sdk_mutex_unlock(cond->lock);
    if (h2_jieli_sdk_mutex_unlock(mutex->native) != 0) return H2_PAL_ERR_IO;
    result = map_wait(h2_jieli_sdk_sem_take(cond->wake, timeout_ms));
    if (h2_jieli_sdk_mutex_lock(mutex->native, H2_JIELI_SDK_WAIT_FOREVER) != 0) {
        return H2_PAL_ERR_IO;
    }
    if (h2_jieli_sdk_mutex_lock(cond->lock, H2_JIELI_SDK_WAIT_FOREVER) != 0) {
        return H2_PAL_ERR_IO;
    }
    if (cond->waiters > 0u) cond->waiters--;
    if (result == H2_PAL_OK && cond->pending_wakes > 0u) {
        cond->pending_wakes--;
    }
    /* A signal can race a timeout before this waiter reacquires the lock.
     * Retire excess semaphore tokens so pending_wakes never exceeds waiters;
     * otherwise broadcast's unsigned subtraction can wrap to UINT32_MAX. */
    while (cond->pending_wakes > cond->waiters) {
        (void)h2_jieli_sdk_sem_take(cond->wake, 0u);
        cond->pending_wakes--;
    }
    (void)h2_jieli_sdk_mutex_unlock(cond->lock);
    return result;
}

static h2_pal_result_t sync_signal_cond(void *user, h2_pal_cond_t *cond)
{
    h2_pal_result_t result = H2_PAL_OK;
    (void)user;
    if (cond == NULL) return H2_PAL_ERR_INVALID_ARG;
    if (h2_jieli_sdk_mutex_lock(cond->lock, H2_JIELI_SDK_WAIT_FOREVER) != 0) {
        return H2_PAL_ERR_IO;
    }
    if (cond->waiters > cond->pending_wakes) {
        cond->pending_wakes++;
        if (h2_jieli_sdk_sem_give(cond->wake) != 0) result = H2_PAL_ERR_IO;
    }
    (void)h2_jieli_sdk_mutex_unlock(cond->lock);
    return result;
}

static h2_pal_result_t sync_broadcast_cond(void *user, h2_pal_cond_t *cond)
{
    h2_pal_result_t result = H2_PAL_OK;
    (void)user;
    if (cond == NULL) return H2_PAL_ERR_INVALID_ARG;
    if (h2_jieli_sdk_mutex_lock(cond->lock, H2_JIELI_SDK_WAIT_FOREVER) != 0) {
        return H2_PAL_ERR_IO;
    }
    uint32_t wakes = cond->waiters - cond->pending_wakes;
    cond->pending_wakes += wakes;
    while (wakes-- > 0u) {
        if (h2_jieli_sdk_sem_give(cond->wake) != 0) result = H2_PAL_ERR_IO;
    }
    (void)h2_jieli_sdk_mutex_unlock(cond->lock);
    return result;
}

static const h2_pal_sync_vtable_t s_sync_vtable = {
    .create_mutex = sync_create_mutex,
    .destroy_mutex = sync_destroy_mutex,
    .lock_mutex = sync_lock_mutex,
    .try_lock_mutex = sync_try_lock_mutex,
    .unlock_mutex = sync_unlock_mutex,
    .create_semaphore = sync_create_semaphore,
    .destroy_semaphore = sync_destroy_semaphore,
    .take_semaphore = sync_take_semaphore,
    .give_semaphore = sync_give_semaphore,
    .create_cond = sync_create_cond,
    .destroy_cond = sync_destroy_cond,
    .wait_cond = sync_wait_cond,
    .signal_cond = sync_signal_cond,
    .broadcast_cond = sync_broadcast_cond,
};

static const h2_pal_sync_api_t s_sync_api = {
    .user = NULL,
    .vtable = &s_sync_vtable,
};

const h2_pal_sync_api_t *h2_jieli_wl82_platform_sync_api(void)
{
    return &s_sync_api;
}
