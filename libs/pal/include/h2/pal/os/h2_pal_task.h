#ifndef H2_PAL_TASK_H
#define H2_PAL_TASK_H

#include "h2/pal/core/h2_pal_errors.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct h2_pal_task h2_pal_task_t;

typedef void (*h2_pal_task_entry_t)(void *ctx);

typedef struct h2_pal_task_options {
    const char *name;
    size_t min_stack_size;
} h2_pal_task_options_t;

typedef struct h2_pal_task_vtable {
    int (*start)(void *user,
        const h2_pal_task_options_t *options,
        h2_pal_task_entry_t entry,
        void *ctx,
        h2_pal_task_t **out_task);
    /* A successful join releases the task handle. On failure, the handle and
     * entry context remain owned by the caller so join can be retried. */
    int (*join)(void *user, h2_pal_task_t *task);
} h2_pal_task_vtable_t;

typedef struct h2_pal_task_api {
    void *user;
    const h2_pal_task_vtable_t *vtable;
} h2_pal_task_api_t;

static inline h2_pal_result_t h2_pal_task_start(
    const h2_pal_task_api_t *api,
    const h2_pal_task_options_t *options,
    h2_pal_task_entry_t entry,
    void *ctx,
    h2_pal_task_t **out_task) {
    if (entry == NULL || out_task == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL || api->vtable->start == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return (h2_pal_result_t)api->vtable->start(api->user, options, entry, ctx, out_task);
}

static inline h2_pal_result_t h2_pal_task_join(
    const h2_pal_task_api_t *api,
    h2_pal_task_t *task) {
    if (task == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL || api->vtable->join == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return (h2_pal_result_t)api->vtable->join(api->user, task);
}

#ifdef __cplusplus
}
#endif

#endif
