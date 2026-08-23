#include "h2_bk_platform_core.h"

#include <os/mem.h>
#include <os/os.h>

struct h2_pal_mutex {
    beken_mutex_t handle;
    const h2_pal_mem_api_t *allocator;
    int recursive;
};

struct h2_pal_semaphore {
    beken_semaphore_t handle;
    const h2_pal_mem_api_t *allocator;
};

struct h2_pal_cond {
    beken_semaphore_t signal;
    beken_mutex_t lock;
    const h2_pal_mem_api_t *allocator;
    uint32_t waiters;
    uint32_t pending_signals;
};

static void *sync_alloc(const h2_pal_mem_api_t *allocator, size_t len) {
    return allocator != NULL ? h2_pal_mem_alloc(allocator, len) : os_malloc(len);
}

static void sync_free(const h2_pal_mem_api_t *allocator, void *ptr) {
    if (allocator != NULL) {
        h2_pal_mem_free(allocator, ptr);
    } else if (ptr != NULL) {
        os_free(ptr);
    }
}

static uint32_t timeout_ms_to_bk(uint32_t timeout_ms) {
    return timeout_ms == H2_PAL_SYNC_WAIT_FOREVER ? BEKEN_WAIT_FOREVER : timeout_ms;
}

static h2_pal_result_t bk_mutex_create(
    void *user,
    const h2_pal_mutex_config_t *config,
    h2_pal_mutex_t **out_mutex) {
    (void)user;
    if (config == NULL || out_mutex == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_mutex = NULL;

    h2_pal_mutex_t *mutex = (h2_pal_mutex_t *)sync_alloc(config->allocator, sizeof(*mutex));
    if (mutex == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    os_memset(mutex, 0, sizeof(*mutex));
    mutex->allocator = config->allocator;
    mutex->recursive = (config->flags & H2_PAL_MUTEX_FLAG_RECURSIVE) != 0u;

    int ret = mutex->recursive
        ? rtos_init_recursive_mutex(&mutex->handle)
        : rtos_init_mutex(&mutex->handle);
    if (ret != kNoErr) {
        sync_free(config->allocator, mutex);
        return H2_PAL_ERR_NO_MEMORY;
    }

    *out_mutex = mutex;
    return H2_PAL_OK;
}

static h2_pal_result_t bk_mutex_destroy(void *user, h2_pal_mutex_t *mutex) {
    (void)user;
    if (mutex == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (mutex->handle != NULL) {
        if (mutex->recursive) {
            (void)rtos_deinit_recursive_mutex(&mutex->handle);
        } else {
            (void)rtos_deinit_mutex(&mutex->handle);
        }
    }
    const h2_pal_mem_api_t *allocator = mutex->allocator;
    sync_free(allocator, mutex);
    return H2_PAL_OK;
}

static h2_pal_result_t bk_mutex_lock(void *user, h2_pal_mutex_t *mutex) {
    (void)user;
    if (mutex == NULL || mutex->handle == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    int ret = mutex->recursive
        ? rtos_lock_recursive_mutex(&mutex->handle)
        : rtos_lock_mutex(&mutex->handle);
    return ret == kNoErr ? H2_PAL_OK : H2_PAL_ERR_IO;
}

static h2_pal_result_t bk_mutex_try_lock(void *user, h2_pal_mutex_t *mutex) {
    (void)user;
    if (mutex == NULL || mutex->handle == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (mutex->recursive) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    int ret = rtos_trylock_mutex(&mutex->handle);
    return ret == kNoErr ? H2_PAL_OK : H2_PAL_ERR_WOULD_BLOCK;
}

static h2_pal_result_t bk_mutex_unlock(void *user, h2_pal_mutex_t *mutex) {
    (void)user;
    if (mutex == NULL || mutex->handle == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    int ret = mutex->recursive
        ? rtos_unlock_recursive_mutex(&mutex->handle)
        : rtos_unlock_mutex(&mutex->handle);
    return ret == kNoErr ? H2_PAL_OK : H2_PAL_ERR_IO;
}

static h2_pal_result_t bk_semaphore_create(
    void *user,
    const h2_pal_semaphore_config_t *config,
    h2_pal_semaphore_t **out_semaphore) {
    (void)user;
    if (config == NULL || out_semaphore == NULL || config->max_count == 0u || config->initial_count > config->max_count) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_semaphore = NULL;

    h2_pal_semaphore_t *semaphore =
        (h2_pal_semaphore_t *)sync_alloc(config->allocator, sizeof(*semaphore));
    if (semaphore == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    os_memset(semaphore, 0, sizeof(*semaphore));
    semaphore->allocator = config->allocator;
    int ret = rtos_init_semaphore_ex(&semaphore->handle, (int)config->max_count, (int)config->initial_count);
    if (ret != kNoErr) {
        sync_free(config->allocator, semaphore);
        return H2_PAL_ERR_NO_MEMORY;
    }

    *out_semaphore = semaphore;
    return H2_PAL_OK;
}

static h2_pal_result_t bk_semaphore_destroy(void *user, h2_pal_semaphore_t *semaphore) {
    (void)user;
    if (semaphore == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (semaphore->handle != NULL) {
        (void)rtos_deinit_semaphore(&semaphore->handle);
    }
    const h2_pal_mem_api_t *allocator = semaphore->allocator;
    sync_free(allocator, semaphore);
    return H2_PAL_OK;
}

static h2_pal_result_t bk_semaphore_take(
    void *user,
    h2_pal_semaphore_t *semaphore,
    uint32_t timeout_ms) {
    (void)user;
    if (semaphore == NULL || semaphore->handle == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    int ret = rtos_get_semaphore(&semaphore->handle, timeout_ms_to_bk(timeout_ms));
    return ret == kNoErr ? H2_PAL_OK : H2_PAL_ERR_TIMEOUT;
}

static h2_pal_result_t bk_semaphore_give(void *user, h2_pal_semaphore_t *semaphore) {
    (void)user;
    if (semaphore == NULL || semaphore->handle == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    int ret = rtos_set_semaphore(&semaphore->handle);
    return ret == kNoErr ? H2_PAL_OK : H2_PAL_ERR_FULL;
}

static h2_pal_result_t bk_cond_create(
    void *user,
    const h2_pal_cond_config_t *config,
    h2_pal_cond_t **out_cond) {
    (void)user;
    if (config == NULL || out_cond == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_cond = NULL;
    h2_pal_cond_t *cond = (h2_pal_cond_t *)sync_alloc(config->allocator, sizeof(*cond));
    if (cond == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    os_memset(cond, 0, sizeof(*cond));
    cond->allocator = config->allocator;
    if (rtos_init_mutex(&cond->lock) != kNoErr ||
        rtos_init_semaphore_ex(&cond->signal, UINT16_MAX, 0) != kNoErr) {
        if (cond->signal != NULL) (void)rtos_deinit_semaphore(&cond->signal);
        if (cond->lock != NULL) (void)rtos_deinit_mutex(&cond->lock);
        sync_free(config->allocator, cond);
        return H2_PAL_ERR_NO_MEMORY;
    }
    *out_cond = cond;
    return H2_PAL_OK;
}

static h2_pal_result_t bk_cond_destroy(void *user, h2_pal_cond_t *cond) {
    (void)user;
    if (cond == NULL || cond->lock == NULL || cond->signal == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (rtos_lock_mutex(&cond->lock) != kNoErr) {
        return H2_PAL_ERR_IO;
    }
    if (cond->waiters != 0u) {
        (void)rtos_unlock_mutex(&cond->lock);
        return H2_PAL_ERR_INVALID_STATE;
    }
    (void)rtos_unlock_mutex(&cond->lock);
    (void)rtos_deinit_semaphore(&cond->signal);
    (void)rtos_deinit_mutex(&cond->lock);
    const h2_pal_mem_api_t *allocator = cond->allocator;
    sync_free(allocator, cond);
    return H2_PAL_OK;
}

static h2_pal_result_t bk_cond_wait(
    void *user,
    h2_pal_cond_t *cond,
    h2_pal_mutex_t *mutex,
    uint32_t timeout_ms) {
    (void)user;
    if (cond == NULL || cond->lock == NULL || cond->signal == NULL ||
        mutex == NULL || mutex->handle == NULL || mutex->recursive) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (rtos_lock_mutex(&cond->lock) != kNoErr) {
        return H2_PAL_ERR_IO;
    }
    cond->waiters++;
    (void)rtos_unlock_mutex(&cond->lock);
    if (rtos_unlock_mutex(&mutex->handle) != kNoErr) {
        (void)rtos_lock_mutex(&cond->lock);
        cond->waiters--;
        (void)rtos_unlock_mutex(&cond->lock);
        return H2_PAL_ERR_IO;
    }
    int ret = rtos_get_semaphore(&cond->signal, timeout_ms_to_bk(timeout_ms));
    if (rtos_lock_mutex(&cond->lock) != kNoErr) {
        (void)rtos_lock_mutex(&mutex->handle);
        return H2_PAL_ERR_IO;
    }
    if (ret != kNoErr && cond->pending_signals > 0u &&
        rtos_get_semaphore(&cond->signal, 0u) == kNoErr) {
        ret = kNoErr;
    }
    cond->waiters--;
    if (ret == kNoErr && cond->pending_signals > 0u) {
        cond->pending_signals--;
    }
    (void)rtos_unlock_mutex(&cond->lock);
    if (rtos_lock_mutex(&mutex->handle) != kNoErr) {
        return H2_PAL_ERR_IO;
    }
    return ret == kNoErr ? H2_PAL_OK : H2_PAL_ERR_TIMEOUT;
}

static h2_pal_result_t bk_cond_wake(h2_pal_cond_t *cond, bool all) {
    if (cond == NULL || cond->lock == NULL || cond->signal == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (rtos_lock_mutex(&cond->lock) != kNoErr) {
        return H2_PAL_ERR_IO;
    }
    uint32_t available = cond->waiters > cond->pending_signals
                             ? cond->waiters - cond->pending_signals
                             : 0u;
    uint32_t count = all ? available : (available > 0u ? 1u : 0u);
    h2_pal_result_t rc = H2_PAL_OK;
    for (uint32_t i = 0u; i < count; ++i) {
        if (rtos_set_semaphore(&cond->signal) != kNoErr) {
            rc = H2_PAL_ERR_FULL;
            break;
        }
        cond->pending_signals++;
    }
    (void)rtos_unlock_mutex(&cond->lock);
    return rc;
}

static h2_pal_result_t bk_cond_signal(void *user, h2_pal_cond_t *cond) {
    (void)user;
    return bk_cond_wake(cond, false);
}

static h2_pal_result_t bk_cond_broadcast(void *user, h2_pal_cond_t *cond) {
    (void)user;
    return bk_cond_wake(cond, true);
}

const h2_pal_sync_api_t *h2_bk_platform_sync_api(void) {
    static const h2_pal_sync_vtable_t vtable = {
        .create_mutex = bk_mutex_create,
        .destroy_mutex = bk_mutex_destroy,
        .lock_mutex = bk_mutex_lock,
        .try_lock_mutex = bk_mutex_try_lock,
        .unlock_mutex = bk_mutex_unlock,
        .create_semaphore = bk_semaphore_create,
        .destroy_semaphore = bk_semaphore_destroy,
        .take_semaphore = bk_semaphore_take,
        .give_semaphore = bk_semaphore_give,
        .create_cond = bk_cond_create,
        .destroy_cond = bk_cond_destroy,
        .wait_cond = bk_cond_wait,
        .signal_cond = bk_cond_signal,
        .broadcast_cond = bk_cond_broadcast,
    };
    static const h2_pal_sync_api_t api = {
        .user = NULL,
        .vtable = &vtable,
    };
    return &api;
}
