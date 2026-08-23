#include "h2_windows_internal.h"

#include <limits.h>
#include <string.h>

struct h2_pal_mutex {
    h2_windows_platform_t *platform;
    const h2_pal_mem_api_t *allocator;
    CRITICAL_SECTION native;
    volatile LONG owner;
    uint32_t depth;
    int recursive;
};

struct h2_pal_semaphore {
    h2_windows_platform_t *platform;
    const h2_pal_mem_api_t *allocator;
    HANDLE native;
    uint32_t max_count;
    LONG waiters;
};

struct h2_pal_cond {
    h2_windows_platform_t *platform;
    const h2_pal_mem_api_t *allocator;
    CONDITION_VARIABLE native;
    LONG waiters;
};

static void *sync_alloc(const h2_pal_mem_api_t *allocator, size_t size) {
    return allocator == NULL ? h2_windows_heap_alloc(size)
                             : h2_pal_mem_alloc(allocator, size);
}

static void sync_free(const h2_pal_mem_api_t *allocator, void *memory) {
    if (allocator == NULL) {
        h2_windows_heap_free(memory);
    } else {
        h2_pal_mem_free(allocator, memory);
    }
}

static h2_pal_result_t windows_sync_create_mutex(
    void *user, const h2_pal_mutex_config_t *config,
    h2_pal_mutex_t **out_mutex) {
    h2_windows_platform_t *platform = user;
    if (config == NULL || out_mutex == NULL ||
        (config->flags & ~H2_PAL_MUTEX_FLAG_RECURSIVE) != 0u) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_mutex = NULL;
    h2_pal_mutex_t *mutex = sync_alloc(config->allocator, sizeof(*mutex));
    if (mutex == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    memset(mutex, 0, sizeof(*mutex));
    mutex->platform = platform;
    mutex->allocator = config->allocator;
    mutex->recursive =
        (config->flags & H2_PAL_MUTEX_FLAG_RECURSIVE) != 0u;
    InitializeCriticalSection(&mutex->native);
    h2_windows_object_acquire(platform);
    *out_mutex = mutex;
    return H2_PAL_OK;
}

static h2_pal_result_t windows_sync_destroy_mutex(void *user,
                                                   h2_pal_mutex_t *mutex) {
    h2_windows_platform_t *platform = user;
    if (mutex == NULL || mutex->platform != platform) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (!TryEnterCriticalSection(&mutex->native)) {
        return H2_PAL_ERR_BUSY;
    }
    if (mutex->depth != 0u) {
        LeaveCriticalSection(&mutex->native);
        return H2_PAL_ERR_BUSY;
    }
    LeaveCriticalSection(&mutex->native);
    DeleteCriticalSection(&mutex->native);
    const h2_pal_mem_api_t *allocator = mutex->allocator;
    sync_free(allocator, mutex);
    h2_windows_object_release(platform);
    return H2_PAL_OK;
}

static h2_pal_result_t windows_sync_lock_mutex(void *user,
                                                h2_pal_mutex_t *mutex) {
    if (mutex == NULL || mutex->platform != user) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    DWORD current = GetCurrentThreadId();
    if (!mutex->recursive &&
        (DWORD)InterlockedCompareExchange(&mutex->owner, 0, 0) == current) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    EnterCriticalSection(&mutex->native);
    (void)InterlockedExchange(&mutex->owner, (LONG)current);
    ++mutex->depth;
    return H2_PAL_OK;
}

static h2_pal_result_t windows_sync_try_lock_mutex(void *user,
                                                    h2_pal_mutex_t *mutex) {
    if (mutex == NULL || mutex->platform != user) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    DWORD current = GetCurrentThreadId();
    if (!mutex->recursive &&
        (DWORD)InterlockedCompareExchange(&mutex->owner, 0, 0) == current) {
        return H2_PAL_ERR_WOULD_BLOCK;
    }
    if (!TryEnterCriticalSection(&mutex->native)) {
        return H2_PAL_ERR_WOULD_BLOCK;
    }
    (void)InterlockedExchange(&mutex->owner, (LONG)current);
    ++mutex->depth;
    return H2_PAL_OK;
}

static h2_pal_result_t windows_sync_unlock_mutex(void *user,
                                                  h2_pal_mutex_t *mutex) {
    if (mutex == NULL || mutex->platform != user ||
        (DWORD)InterlockedCompareExchange(&mutex->owner, 0, 0) !=
            GetCurrentThreadId() ||
        mutex->depth == 0u) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    --mutex->depth;
    if (mutex->depth == 0u) {
        (void)InterlockedExchange(&mutex->owner, 0);
    }
    LeaveCriticalSection(&mutex->native);
    return H2_PAL_OK;
}

static h2_pal_result_t windows_sync_create_semaphore(
    void *user, const h2_pal_semaphore_config_t *config,
    h2_pal_semaphore_t **out_semaphore) {
    h2_windows_platform_t *platform = user;
    if (config == NULL || out_semaphore == NULL || config->max_count == 0u ||
        config->initial_count > config->max_count ||
        config->max_count > (uint32_t)LONG_MAX) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_semaphore = NULL;
    h2_pal_semaphore_t *semaphore =
        sync_alloc(config->allocator, sizeof(*semaphore));
    if (semaphore == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    memset(semaphore, 0, sizeof(*semaphore));
    semaphore->platform = platform;
    semaphore->allocator = config->allocator;
    semaphore->max_count = config->max_count;
    semaphore->native = CreateSemaphoreW(NULL, (LONG)config->initial_count,
                                         (LONG)config->max_count, NULL);
    if (semaphore->native == NULL) {
        sync_free(config->allocator, semaphore);
        return h2_windows_error_from_win32(GetLastError());
    }
    h2_windows_object_acquire(platform);
    *out_semaphore = semaphore;
    return H2_PAL_OK;
}

static h2_pal_result_t windows_sync_destroy_semaphore(
    void *user, h2_pal_semaphore_t *semaphore) {
    h2_windows_platform_t *platform = user;
    if (semaphore == NULL || semaphore->platform != platform) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (InterlockedCompareExchange(&semaphore->waiters, 0, 0) != 0) {
        return H2_PAL_ERR_BUSY;
    }
    if (!CloseHandle(semaphore->native)) {
        return h2_windows_error_from_win32(GetLastError());
    }
    const h2_pal_mem_api_t *allocator = semaphore->allocator;
    sync_free(allocator, semaphore);
    h2_windows_object_release(platform);
    return H2_PAL_OK;
}

static h2_pal_result_t windows_sync_take_semaphore(
    void *user, h2_pal_semaphore_t *semaphore, uint32_t timeout_ms) {
    if (semaphore == NULL || semaphore->platform != user) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    (void)InterlockedIncrement(&semaphore->waiters);
    DWORD result = WaitForSingleObject(
        semaphore->native,
        timeout_ms == H2_PAL_SYNC_WAIT_FOREVER ? INFINITE : timeout_ms);
    (void)InterlockedDecrement(&semaphore->waiters);
    WakeAllConditionVariable(&semaphore->platform->idle);
    if (result == WAIT_OBJECT_0) {
        return H2_PAL_OK;
    }
    return result == WAIT_TIMEOUT ? H2_PAL_ERR_TIMEOUT : H2_PAL_ERR_IO;
}

static h2_pal_result_t windows_sync_give_semaphore(
    void *user, h2_pal_semaphore_t *semaphore) {
    if (semaphore == NULL || semaphore->platform != user) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (ReleaseSemaphore(semaphore->native, 1, NULL)) {
        return H2_PAL_OK;
    }
    DWORD error = GetLastError();
    return error == ERROR_TOO_MANY_POSTS ? H2_PAL_ERR_FULL
                                         : h2_windows_error_from_win32(error);
}

static h2_pal_result_t windows_sync_create_cond(
    void *user, const h2_pal_cond_config_t *config,
    h2_pal_cond_t **out_cond) {
    h2_windows_platform_t *platform = user;
    if (config == NULL || out_cond == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_cond = NULL;
    h2_pal_cond_t *condition = sync_alloc(config->allocator, sizeof(*condition));
    if (condition == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    memset(condition, 0, sizeof(*condition));
    condition->platform = platform;
    condition->allocator = config->allocator;
    InitializeConditionVariable(&condition->native);
    h2_windows_object_acquire(platform);
    *out_cond = condition;
    return H2_PAL_OK;
}

static h2_pal_result_t windows_sync_destroy_cond(void *user,
                                                  h2_pal_cond_t *condition) {
    h2_windows_platform_t *platform = user;
    if (condition == NULL || condition->platform != platform) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (InterlockedCompareExchange(&condition->waiters, 0, 0) != 0) {
        return H2_PAL_ERR_BUSY;
    }
    const h2_pal_mem_api_t *allocator = condition->allocator;
    sync_free(allocator, condition);
    h2_windows_object_release(platform);
    return H2_PAL_OK;
}

static h2_pal_result_t windows_sync_wait_cond(
    void *user, h2_pal_cond_t *condition, h2_pal_mutex_t *mutex,
    uint32_t timeout_ms) {
    if (condition == NULL || mutex == NULL || condition->platform != user ||
        mutex->platform != user || mutex->recursive || mutex->depth != 1u ||
        (DWORD)InterlockedCompareExchange(&mutex->owner, 0, 0) !=
            GetCurrentThreadId()) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (timeout_ms == H2_PAL_SYNC_NO_WAIT) {
        return H2_PAL_ERR_TIMEOUT;
    }
    (void)InterlockedExchange(&mutex->owner, 0);
    mutex->depth = 0u;
    (void)InterlockedIncrement(&condition->waiters);
    BOOL result = SleepConditionVariableCS(
        &condition->native, &mutex->native,
        timeout_ms == H2_PAL_SYNC_WAIT_FOREVER ? INFINITE : timeout_ms);
    (void)InterlockedDecrement(&condition->waiters);
    (void)InterlockedExchange(&mutex->owner, (LONG)GetCurrentThreadId());
    mutex->depth = 1u;
    if (result) {
        return H2_PAL_OK;
    }
    DWORD error = GetLastError();
    return error == ERROR_TIMEOUT ? H2_PAL_ERR_TIMEOUT
                                  : h2_windows_error_from_win32(error);
}

static h2_pal_result_t windows_sync_signal_cond(void *user,
                                                 h2_pal_cond_t *condition) {
    if (condition == NULL || condition->platform != user) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    WakeConditionVariable(&condition->native);
    return H2_PAL_OK;
}

static h2_pal_result_t windows_sync_broadcast_cond(void *user,
                                                    h2_pal_cond_t *condition) {
    if (condition == NULL || condition->platform != user) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    WakeAllConditionVariable(&condition->native);
    return H2_PAL_OK;
}

const h2_pal_sync_vtable_t h2_windows_sync_vtable = {
    .create_mutex = windows_sync_create_mutex,
    .destroy_mutex = windows_sync_destroy_mutex,
    .lock_mutex = windows_sync_lock_mutex,
    .try_lock_mutex = windows_sync_try_lock_mutex,
    .unlock_mutex = windows_sync_unlock_mutex,
    .create_semaphore = windows_sync_create_semaphore,
    .destroy_semaphore = windows_sync_destroy_semaphore,
    .take_semaphore = windows_sync_take_semaphore,
    .give_semaphore = windows_sync_give_semaphore,
    .create_cond = windows_sync_create_cond,
    .destroy_cond = windows_sync_destroy_cond,
    .wait_cond = windows_sync_wait_cond,
    .signal_cond = windows_sync_signal_cond,
    .broadcast_cond = windows_sync_broadcast_cond,
};
