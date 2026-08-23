#include "h2_esp_platform_core.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <stdlib.h>
#include <string.h>

struct h2_pal_mutex {
    SemaphoreHandle_t handle;
    const h2_pal_mem_api_t *allocator;
    int recursive;
};

struct h2_pal_semaphore {
    SemaphoreHandle_t handle;
    const h2_pal_mem_api_t *allocator;
};

struct h2_pal_cond {
    SemaphoreHandle_t signal;
    SemaphoreHandle_t lock;
    const h2_pal_mem_api_t *allocator;
    uint32_t waiters;
    uint32_t pending_signals;
};

static void *sync_alloc(const h2_pal_mem_api_t *allocator, size_t len) {
    return allocator != NULL ? h2_pal_mem_alloc(allocator, len) : calloc(1u, len);
}

static void sync_free(const h2_pal_mem_api_t *allocator, void *ptr) {
    if (allocator != NULL) {
        h2_pal_mem_free(allocator, ptr);
    } else {
        free(ptr);
    }
}

static TickType_t timeout_ticks(uint32_t timeout_ms) {
    if (timeout_ms == H2_PAL_SYNC_WAIT_FOREVER) {
        return portMAX_DELAY;
    }
    if (timeout_ms == H2_PAL_SYNC_NO_WAIT) {
        return 0;
    }
    return pdMS_TO_TICKS(timeout_ms);
}

static h2_pal_result_t esp_mutex_create(
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
    mutex->allocator = config->allocator;
    mutex->recursive = (config->flags & H2_PAL_MUTEX_FLAG_RECURSIVE) != 0u;
    mutex->handle = mutex->recursive ? xSemaphoreCreateRecursiveMutex() : xSemaphoreCreateMutex();
    if (mutex->handle == NULL) {
        sync_free(config->allocator, mutex);
        return H2_PAL_ERR_NO_MEMORY;
    }

    *out_mutex = mutex;
    return H2_PAL_OK;
}

static h2_pal_result_t esp_mutex_destroy(void *user, h2_pal_mutex_t *mutex) {
    (void)user;
    if (mutex == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (mutex->handle != NULL) {
        vSemaphoreDelete(mutex->handle);
    }
    const h2_pal_mem_api_t *allocator = mutex->allocator;
    sync_free(allocator, mutex);
    return H2_PAL_OK;
}

static h2_pal_result_t esp_mutex_lock(void *user, h2_pal_mutex_t *mutex) {
    (void)user;
    if (mutex == NULL || mutex->handle == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    BaseType_t ok = mutex->recursive
        ? xSemaphoreTakeRecursive(mutex->handle, portMAX_DELAY)
        : xSemaphoreTake(mutex->handle, portMAX_DELAY);
    return ok == pdTRUE ? H2_PAL_OK : H2_PAL_ERR_IO;
}

static h2_pal_result_t esp_mutex_try_lock(void *user, h2_pal_mutex_t *mutex) {
    (void)user;
    if (mutex == NULL || mutex->handle == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    BaseType_t ok = mutex->recursive
        ? xSemaphoreTakeRecursive(mutex->handle, 0)
        : xSemaphoreTake(mutex->handle, 0);
    return ok == pdTRUE ? H2_PAL_OK : H2_PAL_ERR_WOULD_BLOCK;
}

static h2_pal_result_t esp_mutex_unlock(void *user, h2_pal_mutex_t *mutex) {
    (void)user;
    if (mutex == NULL || mutex->handle == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    BaseType_t ok = mutex->recursive
        ? xSemaphoreGiveRecursive(mutex->handle)
        : xSemaphoreGive(mutex->handle);
    return ok == pdTRUE ? H2_PAL_OK : H2_PAL_ERR_IO;
}

static h2_pal_result_t esp_semaphore_create(
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
    semaphore->allocator = config->allocator;
    semaphore->handle = xSemaphoreCreateCounting(config->max_count, config->initial_count);
    if (semaphore->handle == NULL) {
        sync_free(config->allocator, semaphore);
        return H2_PAL_ERR_NO_MEMORY;
    }

    *out_semaphore = semaphore;
    return H2_PAL_OK;
}

static h2_pal_result_t esp_semaphore_destroy(void *user, h2_pal_semaphore_t *semaphore) {
    (void)user;
    if (semaphore == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (semaphore->handle != NULL) {
        vSemaphoreDelete(semaphore->handle);
    }
    const h2_pal_mem_api_t *allocator = semaphore->allocator;
    sync_free(allocator, semaphore);
    return H2_PAL_OK;
}

static h2_pal_result_t esp_semaphore_take(
    void *user,
    h2_pal_semaphore_t *semaphore,
    uint32_t timeout_ms) {
    (void)user;
    if (semaphore == NULL || semaphore->handle == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    BaseType_t ok = xSemaphoreTake(semaphore->handle, timeout_ticks(timeout_ms));
    return ok == pdTRUE ? H2_PAL_OK : H2_PAL_ERR_TIMEOUT;
}

static h2_pal_result_t esp_semaphore_give(void *user, h2_pal_semaphore_t *semaphore) {
    (void)user;
    if (semaphore == NULL || semaphore->handle == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    BaseType_t ok = xSemaphoreGive(semaphore->handle);
    return ok == pdTRUE ? H2_PAL_OK : H2_PAL_ERR_FULL;
}

static h2_pal_result_t esp_cond_create(
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
    memset(cond, 0, sizeof(*cond));
    cond->allocator = config->allocator;
    cond->lock = xSemaphoreCreateMutex();
    cond->signal = xSemaphoreCreateCounting(UINT16_MAX, 0u);
    if (cond->lock == NULL || cond->signal == NULL) {
        if (cond->lock != NULL) vSemaphoreDelete(cond->lock);
        if (cond->signal != NULL) vSemaphoreDelete(cond->signal);
        sync_free(config->allocator, cond);
        return H2_PAL_ERR_NO_MEMORY;
    }
    *out_cond = cond;
    return H2_PAL_OK;
}

static h2_pal_result_t esp_cond_destroy(void *user, h2_pal_cond_t *cond) {
    (void)user;
    if (cond == NULL || cond->lock == NULL || cond->signal == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(cond->lock, portMAX_DELAY) != pdTRUE) {
        return H2_PAL_ERR_IO;
    }
    if (cond->waiters != 0u) {
        (void)xSemaphoreGive(cond->lock);
        return H2_PAL_ERR_INVALID_STATE;
    }
    (void)xSemaphoreGive(cond->lock);
    vSemaphoreDelete(cond->signal);
    vSemaphoreDelete(cond->lock);
    const h2_pal_mem_api_t *allocator = cond->allocator;
    sync_free(allocator, cond);
    return H2_PAL_OK;
}

static h2_pal_result_t esp_cond_wait(
    void *user,
    h2_pal_cond_t *cond,
    h2_pal_mutex_t *mutex,
    uint32_t timeout_ms) {
    (void)user;
    if (cond == NULL || cond->lock == NULL || cond->signal == NULL ||
        mutex == NULL || mutex->handle == NULL || mutex->recursive) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(cond->lock, portMAX_DELAY) != pdTRUE) {
        return H2_PAL_ERR_IO;
    }
    cond->waiters++;
    (void)xSemaphoreGive(cond->lock);
    if (xSemaphoreGive(mutex->handle) != pdTRUE) {
        (void)xSemaphoreTake(cond->lock, portMAX_DELAY);
        cond->waiters--;
        (void)xSemaphoreGive(cond->lock);
        return H2_PAL_ERR_IO;
    }
    BaseType_t signaled = xSemaphoreTake(cond->signal, timeout_ticks(timeout_ms));
    if (xSemaphoreTake(cond->lock, portMAX_DELAY) != pdTRUE) {
        (void)xSemaphoreTake(mutex->handle, portMAX_DELAY);
        return H2_PAL_ERR_IO;
    }
    if (signaled != pdTRUE && cond->pending_signals > 0u &&
        xSemaphoreTake(cond->signal, 0u) == pdTRUE) {
        signaled = pdTRUE;
    }
    cond->waiters--;
    if (signaled == pdTRUE && cond->pending_signals > 0u) {
        cond->pending_signals--;
    }
    (void)xSemaphoreGive(cond->lock);
    if (xSemaphoreTake(mutex->handle, portMAX_DELAY) != pdTRUE) {
        return H2_PAL_ERR_IO;
    }
    return signaled == pdTRUE ? H2_PAL_OK : H2_PAL_ERR_TIMEOUT;
}

static h2_pal_result_t esp_cond_wake(h2_pal_cond_t *cond, bool all) {
    if (cond == NULL || cond->lock == NULL || cond->signal == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(cond->lock, portMAX_DELAY) != pdTRUE) {
        return H2_PAL_ERR_IO;
    }
    uint32_t available = cond->waiters > cond->pending_signals
                             ? cond->waiters - cond->pending_signals
                             : 0u;
    uint32_t count = all ? available : (available > 0u ? 1u : 0u);
    h2_pal_result_t rc = H2_PAL_OK;
    for (uint32_t i = 0u; i < count; ++i) {
        if (xSemaphoreGive(cond->signal) != pdTRUE) {
            rc = H2_PAL_ERR_FULL;
            break;
        }
        cond->pending_signals++;
    }
    (void)xSemaphoreGive(cond->lock);
    return rc;
}

static h2_pal_result_t esp_cond_signal(void *user, h2_pal_cond_t *cond) {
    (void)user;
    return esp_cond_wake(cond, false);
}

static h2_pal_result_t esp_cond_broadcast(void *user, h2_pal_cond_t *cond) {
    (void)user;
    return esp_cond_wake(cond, true);
}

const h2_pal_sync_api_t *h2_esp_platform_sync_api(void) {
    static const h2_pal_sync_vtable_t vtable = {
        .create_mutex = esp_mutex_create,
        .destroy_mutex = esp_mutex_destroy,
        .lock_mutex = esp_mutex_lock,
        .try_lock_mutex = esp_mutex_try_lock,
        .unlock_mutex = esp_mutex_unlock,
        .create_semaphore = esp_semaphore_create,
        .destroy_semaphore = esp_semaphore_destroy,
        .take_semaphore = esp_semaphore_take,
        .give_semaphore = esp_semaphore_give,
        .create_cond = esp_cond_create,
        .destroy_cond = esp_cond_destroy,
        .wait_cond = esp_cond_wait,
        .signal_cond = esp_cond_signal,
        .broadcast_cond = esp_cond_broadcast,
    };
    static const h2_pal_sync_api_t api = {
        .user = NULL,
        .vtable = &vtable,
    };
    return &api;
}
