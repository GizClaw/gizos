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

typedef struct h2_jieli_cond_waiter {
    struct h2_jieli_cond_waiter *next;
    h2_jieli_sdk_sem_t *wake;
    int notified;
} h2_jieli_cond_waiter_t;

struct h2_pal_cond {
    volatile uint32_t locked;
    h2_jieli_cond_waiter_t *head;
    h2_jieli_cond_waiter_t *tail;
};

/* Task-context gate for the short waiter-list operations. Yield on contention
 * so a higher-priority waiter cannot starve the task retiring its node.
 * Unlike an SDK mutex, this private gate cannot fail during mandatory unlink;
 * no stack-owned waiter may escape after wait returns. */
static void cond_lock(h2_pal_cond_t *cond)
{
    uint32_t expected = 0u;
    while (!h2_jieli_atomic_cas_u32(&cond->locked, &expected, 1u)) {
        h2_jieli_sdk_sleep_ms(1u);
        expected = 0u;
    }
}

static void cond_unlock(h2_pal_cond_t *cond)
{
    h2_jieli_atomic_store_u32(&cond->locked, 0u);
}

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
    void *user, const h2_pal_cond_config_t *config, h2_pal_cond_t **out_cond)
{
    (void)user;
    if (config == NULL || out_cond == NULL) return H2_PAL_ERR_INVALID_ARG;
    *out_cond = NULL;
    h2_pal_cond_t *cond = h2_jieli_sdk_malloc(sizeof(*cond));
    if (cond == NULL) return H2_PAL_ERR_NO_MEMORY;
    cond->locked = 0u;
    cond->head = NULL;
    cond->tail = NULL;
    *out_cond = cond;
    return H2_PAL_OK;
}

static h2_pal_result_t sync_destroy_cond(void *user, h2_pal_cond_t *cond)
{
    (void)user;
    if (cond == NULL) return H2_PAL_ERR_INVALID_ARG;
    cond_lock(cond);
    if (cond->head != NULL) {
        cond_unlock(cond);
        return H2_PAL_ERR_INVALID_STATE;
    }
    cond_unlock(cond);
    h2_jieli_sdk_free(cond);
    return H2_PAL_OK;
}

/* Caller holds the private gate. A notified node stays registered until its
 * owner returns from the SDK wait, keeping destroy from freeing a live cond. */
static void cond_unlink(h2_pal_cond_t *cond, h2_jieli_cond_waiter_t *waiter)
{
    h2_jieli_cond_waiter_t *previous = NULL;
    for (h2_jieli_cond_waiter_t *node = cond->head; node != NULL; node = node->next) {
        if (node == waiter) {
            if (previous == NULL) cond->head = node->next;
            else previous->next = node->next;
            if (cond->tail == node) cond->tail = previous;
            return;
        }
        previous = node;
    }
}

h2_pal_result_t h2_jieli_wl82_cond_wait_owned(
    h2_pal_cond_t *cond, h2_pal_mutex_t *mutex, uint32_t timeout_ms, int *out_locked)
{
    if (out_locked == NULL) return H2_PAL_ERR_INVALID_ARG;
    *out_locked = 1;
    if (cond == NULL || mutex == NULL || mutex->recursive) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    h2_jieli_cond_waiter_t waiter = {0};
    waiter.wake = h2_jieli_sdk_sem_create(0u);
    if (waiter.wake == NULL) return H2_PAL_ERR_NO_MEMORY;
    cond_lock(cond);
    if (cond->tail == NULL) cond->head = &waiter;
    else cond->tail->next = &waiter;
    cond->tail = &waiter;
    cond_unlock(cond);

    const int unlock_result = h2_jieli_sdk_mutex_unlock(mutex->native);
    if (unlock_result == 0) *out_locked = 0;
    h2_pal_result_t result = unlock_result == 0
        ? map_wait(h2_jieli_sdk_sem_take(waiter.wake, timeout_ms))
        : H2_PAL_ERR_IO;
    cond_lock(cond);
    cond_unlink(cond, &waiter);
    cond_unlock(cond);
    h2_jieli_sdk_sem_destroy(waiter.wake);
    if (unlock_result == 0 &&
        h2_jieli_sdk_mutex_lock(mutex->native, H2_JIELI_SDK_WAIT_FOREVER) != 0) {
        return H2_PAL_ERR_IO;
    }
    *out_locked = 1;
    return result;
}

static h2_pal_result_t sync_wait_cond(
    void *user, h2_pal_cond_t *cond, h2_pal_mutex_t *mutex, uint32_t timeout_ms)
{
    (void)user;
    int locked;
    return h2_jieli_wl82_cond_wait_owned(cond, mutex, timeout_ms, &locked);
}

static h2_pal_result_t cond_wake(h2_pal_cond_t *cond, int all)
{
    if (cond == NULL) return H2_PAL_ERR_INVALID_ARG;
    h2_pal_result_t result = H2_PAL_OK;
    cond_lock(cond);
    for (h2_jieli_cond_waiter_t *node = cond->head; node != NULL; node = node->next) {
        if (node->notified) continue;
        if (h2_jieli_sdk_sem_give(node->wake) != 0) {
            result = H2_PAL_ERR_IO;
            break;
        }
        node->notified = 1;
        if (!all) break;
    }
    cond_unlock(cond);
    return result;
}

static h2_pal_result_t sync_signal_cond(void *user, h2_pal_cond_t *cond)
{
    (void)user;
    return cond_wake(cond, 0);
}

static h2_pal_result_t sync_broadcast_cond(void *user, h2_pal_cond_t *cond)
{
    (void)user;
    return cond_wake(cond, 1);
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
