#ifndef H2_PAL_QUEUE_H
#define H2_PAL_QUEUE_H

#include "h2/pal/core/h2_pal_errors.h"
#include "h2/pal/os/h2_pal_mem.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define H2_PAL_QUEUE_NO_WAIT 0u
#define H2_PAL_QUEUE_WAIT_FOREVER UINT32_MAX

typedef struct h2_pal_queue h2_pal_queue_t;

typedef h2_pal_result_t h2_pal_queue_result_t;

typedef struct h2_pal_queue_config {
    const char *name;
    size_t item_size;
    size_t item_count;
    const h2_pal_mem_api_t *allocator;
} h2_pal_queue_config_t;

typedef struct h2_pal_queue_vtable {
    int (*create)(void *user, const h2_pal_queue_config_t *config, h2_pal_queue_t **out_queue);
    void (*destroy)(void *user, h2_pal_queue_t *queue);
    int (*send)(void *user, h2_pal_queue_t *queue, const void *item, uint32_t timeout_ms);
    int (*send_latest)(void *user, h2_pal_queue_t *queue, const void *item);
    int (*recv)(void *user, h2_pal_queue_t *queue, void *out_item, uint32_t timeout_ms);
    int (*reset)(void *user, h2_pal_queue_t *queue);
    int (*close)(void *user, h2_pal_queue_t *queue);
} h2_pal_queue_vtable_t;

typedef struct h2_pal_queue_api {
    void *user;
    const h2_pal_queue_vtable_t *vtable;
} h2_pal_queue_api_t;

static inline int h2_pal_queue_create(
    const h2_pal_queue_api_t *api,
    const h2_pal_queue_config_t *config,
    h2_pal_queue_t **out_queue) {
    if (config == NULL || out_queue == NULL) {
        return H2_PAL_QUEUE_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL || api->vtable->create == NULL) {
        return H2_PAL_QUEUE_ERR_INVALID_ARG;
    }
    return api->vtable->create(api->user, config, out_queue);
}

static inline void h2_pal_queue_destroy(const h2_pal_queue_api_t *api, h2_pal_queue_t *queue) {
    if (api == NULL || api->vtable == NULL || api->vtable->destroy == NULL || queue == NULL) {
        return;
    }
    api->vtable->destroy(api->user, queue);
}

static inline int h2_pal_queue_send(
    const h2_pal_queue_api_t *api,
    h2_pal_queue_t *queue,
    const void *item,
    uint32_t timeout_ms) {
    if (queue == NULL || item == NULL) {
        return H2_PAL_QUEUE_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL || api->vtable->send == NULL) {
        return H2_PAL_QUEUE_ERR_INVALID_ARG;
    }
    return api->vtable->send(api->user, queue, item, timeout_ms);
}

static inline int h2_pal_queue_send_latest(
    const h2_pal_queue_api_t *api,
    h2_pal_queue_t *queue,
    const void *item) {
    if (api == NULL || queue == NULL || item == NULL) {
        return H2_PAL_QUEUE_ERR_INVALID_ARG;
    }
    if (api->vtable != NULL && api->vtable->send_latest != NULL) {
        return api->vtable->send_latest(api->user, queue, item);
    }
    return h2_pal_queue_send(api, queue, item, H2_PAL_QUEUE_NO_WAIT);
}

static inline int h2_pal_queue_recv(
    const h2_pal_queue_api_t *api,
    h2_pal_queue_t *queue,
    void *out_item,
    uint32_t timeout_ms) {
    if (queue == NULL || out_item == NULL) {
        return H2_PAL_QUEUE_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL || api->vtable->recv == NULL) {
        return H2_PAL_QUEUE_ERR_INVALID_ARG;
    }
    return api->vtable->recv(api->user, queue, out_item, timeout_ms);
}

static inline int h2_pal_queue_reset(const h2_pal_queue_api_t *api, h2_pal_queue_t *queue) {
    if (queue == NULL) {
        return H2_PAL_QUEUE_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL || api->vtable->reset == NULL) {
        return H2_PAL_QUEUE_ERR_INVALID_ARG;
    }
    return api->vtable->reset(api->user, queue);
}

static inline int h2_pal_queue_close(const h2_pal_queue_api_t *api, h2_pal_queue_t *queue) {
    if (queue == NULL) {
        return H2_PAL_QUEUE_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL || api->vtable->close == NULL) {
        return H2_PAL_QUEUE_ERR_INVALID_ARG;
    }
    return api->vtable->close(api->user, queue);
}

#ifdef __cplusplus
}
#endif

#endif
