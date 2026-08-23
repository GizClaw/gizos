#ifndef H2_PAL_SYNC_H
#define H2_PAL_SYNC_H

#include "h2/pal/core/h2_pal_errors.h"
#include "h2/pal/os/h2_pal_mem.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define H2_PAL_SYNC_NO_WAIT 0u
#define H2_PAL_SYNC_WAIT_FOREVER UINT32_MAX

typedef struct h2_pal_mutex h2_pal_mutex_t;
typedef struct h2_pal_semaphore h2_pal_semaphore_t;
typedef struct h2_pal_cond h2_pal_cond_t;

typedef enum h2_pal_mutex_flag {
    H2_PAL_MUTEX_FLAG_NONE = 0,
    H2_PAL_MUTEX_FLAG_RECURSIVE = 1u << 0,
} h2_pal_mutex_flag_t;

typedef struct h2_pal_mutex_config {
    const char *name;
    const h2_pal_mem_api_t *allocator;
    uint32_t flags;
} h2_pal_mutex_config_t;

typedef struct h2_pal_semaphore_config {
    const char *name;
    const h2_pal_mem_api_t *allocator;
    uint32_t initial_count;
    uint32_t max_count;
} h2_pal_semaphore_config_t;

typedef struct h2_pal_cond_config {
    const char *name;
    const h2_pal_mem_api_t *allocator;
} h2_pal_cond_config_t;

/**
 * Condition wait contract:
 *
 * - mutex must be a locked, non-recursive mutex owned by the caller;
 * - wait atomically releases mutex while blocking;
 * - normal success and timeout/error returns reacquire mutex before returning.
 *
 * A backend that cannot preserve this ownership contract must return
 * H2_PAL_ERR_UNSUPPORTED from condition creation rather than expose a partial
 * condition implementation.
 */
typedef struct h2_pal_sync_vtable {
    h2_pal_result_t (*create_mutex)(
        void *user,
        const h2_pal_mutex_config_t *config,
        h2_pal_mutex_t **out_mutex);
    h2_pal_result_t (*destroy_mutex)(void *user, h2_pal_mutex_t *mutex);
    h2_pal_result_t (*lock_mutex)(void *user, h2_pal_mutex_t *mutex);
    h2_pal_result_t (*try_lock_mutex)(void *user, h2_pal_mutex_t *mutex);
    h2_pal_result_t (*unlock_mutex)(void *user, h2_pal_mutex_t *mutex);

    h2_pal_result_t (*create_semaphore)(
        void *user,
        const h2_pal_semaphore_config_t *config,
        h2_pal_semaphore_t **out_semaphore);
    h2_pal_result_t (*destroy_semaphore)(void *user, h2_pal_semaphore_t *semaphore);
    h2_pal_result_t (*take_semaphore)(
        void *user,
        h2_pal_semaphore_t *semaphore,
        uint32_t timeout_ms);
    h2_pal_result_t (*give_semaphore)(void *user, h2_pal_semaphore_t *semaphore);

    h2_pal_result_t (*create_cond)(
        void *user,
        const h2_pal_cond_config_t *config,
        h2_pal_cond_t **out_cond);
    h2_pal_result_t (*destroy_cond)(void *user, h2_pal_cond_t *cond);
    h2_pal_result_t (*wait_cond)(
        void *user,
        h2_pal_cond_t *cond,
        h2_pal_mutex_t *mutex,
        uint32_t timeout_ms);
    h2_pal_result_t (*signal_cond)(void *user, h2_pal_cond_t *cond);
    h2_pal_result_t (*broadcast_cond)(void *user, h2_pal_cond_t *cond);
} h2_pal_sync_vtable_t;

typedef struct h2_pal_sync_api {
    void *user;
    const h2_pal_sync_vtable_t *vtable;
} h2_pal_sync_api_t;

static inline h2_pal_result_t h2_pal_mutex_create(
    const h2_pal_sync_api_t *api,
    const h2_pal_mutex_config_t *config,
    h2_pal_mutex_t **out_mutex) {
    if (config == NULL || out_mutex == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL || api->vtable->create_mutex == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->create_mutex(api->user, config, out_mutex);
}

static inline h2_pal_result_t h2_pal_mutex_destroy(
    const h2_pal_sync_api_t *api,
    h2_pal_mutex_t *mutex) {
    if (mutex == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL || api->vtable->destroy_mutex == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->destroy_mutex(api->user, mutex);
}

static inline h2_pal_result_t h2_pal_mutex_lock(
    const h2_pal_sync_api_t *api,
    h2_pal_mutex_t *mutex) {
    if (mutex == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL || api->vtable->lock_mutex == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->lock_mutex(api->user, mutex);
}

static inline h2_pal_result_t h2_pal_mutex_try_lock(
    const h2_pal_sync_api_t *api,
    h2_pal_mutex_t *mutex) {
    if (mutex == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL || api->vtable->try_lock_mutex == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->try_lock_mutex(api->user, mutex);
}

static inline h2_pal_result_t h2_pal_mutex_unlock(
    const h2_pal_sync_api_t *api,
    h2_pal_mutex_t *mutex) {
    if (mutex == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL || api->vtable->unlock_mutex == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->unlock_mutex(api->user, mutex);
}

static inline h2_pal_result_t h2_pal_semaphore_create(
    const h2_pal_sync_api_t *api,
    const h2_pal_semaphore_config_t *config,
    h2_pal_semaphore_t **out_semaphore) {
    if (config == NULL || out_semaphore == NULL || config->max_count == 0u || config->initial_count > config->max_count) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL || api->vtable->create_semaphore == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->create_semaphore(api->user, config, out_semaphore);
}

static inline h2_pal_result_t h2_pal_semaphore_destroy(
    const h2_pal_sync_api_t *api,
    h2_pal_semaphore_t *semaphore) {
    if (semaphore == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL || api->vtable->destroy_semaphore == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->destroy_semaphore(api->user, semaphore);
}

static inline h2_pal_result_t h2_pal_semaphore_take(
    const h2_pal_sync_api_t *api,
    h2_pal_semaphore_t *semaphore,
    uint32_t timeout_ms) {
    if (semaphore == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL || api->vtable->take_semaphore == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->take_semaphore(api->user, semaphore, timeout_ms);
}

static inline h2_pal_result_t h2_pal_semaphore_give(
    const h2_pal_sync_api_t *api,
    h2_pal_semaphore_t *semaphore) {
    if (semaphore == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL || api->vtable->give_semaphore == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->give_semaphore(api->user, semaphore);
}

static inline h2_pal_result_t h2_pal_cond_create(
    const h2_pal_sync_api_t *api,
    const h2_pal_cond_config_t *config,
    h2_pal_cond_t **out_cond) {
    if (config == NULL || out_cond == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL || api->vtable->create_cond == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->create_cond(api->user, config, out_cond);
}

static inline h2_pal_result_t h2_pal_cond_destroy(
    const h2_pal_sync_api_t *api,
    h2_pal_cond_t *cond) {
    if (cond == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL || api->vtable->destroy_cond == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->destroy_cond(api->user, cond);
}

static inline h2_pal_result_t h2_pal_cond_wait(
    const h2_pal_sync_api_t *api,
    h2_pal_cond_t *cond,
    h2_pal_mutex_t *mutex,
    uint32_t timeout_ms) {
    if (cond == NULL || mutex == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL || api->vtable->wait_cond == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->wait_cond(api->user, cond, mutex, timeout_ms);
}

static inline h2_pal_result_t h2_pal_cond_signal(
    const h2_pal_sync_api_t *api,
    h2_pal_cond_t *cond) {
    if (cond == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL || api->vtable->signal_cond == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->signal_cond(api->user, cond);
}

static inline h2_pal_result_t h2_pal_cond_broadcast(
    const h2_pal_sync_api_t *api,
    h2_pal_cond_t *cond) {
    if (cond == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL || api->vtable->broadcast_cond == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->broadcast_cond(api->user, cond);
}

#ifdef __cplusplus
}
#endif

#endif
