#ifndef H2_PAL_LOG_H
#define H2_PAL_LOG_H

#include "h2/pal/core/h2_pal_errors.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define H2_PAL_LOG_MESSAGE_MAX 256u

typedef h2_pal_result_t h2_pal_log_result_t;

typedef enum h2_pal_log_level {
    H2_PAL_LOG_DEBUG = 0,
    H2_PAL_LOG_INFO = 1,
    H2_PAL_LOG_WARN = 2,
    H2_PAL_LOG_ERROR = 3,
} h2_pal_log_level_t;

typedef struct h2_pal_log_vtable {
    int (*write)(void *user, h2_pal_log_level_t level, const char *scope, const char *message);
} h2_pal_log_vtable_t;

typedef struct h2_pal_log_api {
    void *user;
    const h2_pal_log_vtable_t *vtable;
} h2_pal_log_api_t;

static inline h2_pal_result_t h2_pal_log_write(
    const h2_pal_log_api_t *api,
    h2_pal_log_level_t level,
    const char *scope,
    const char *message) {
    if (message == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL || api->vtable->write == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return (h2_pal_result_t)api->vtable->write(api->user, level, scope, message);
}

#ifdef __cplusplus
}
#endif

#endif
