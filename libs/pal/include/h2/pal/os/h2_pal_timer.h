#ifndef H2_PAL_TIMER_H
#define H2_PAL_TIMER_H

#include "h2/pal/core/h2_pal_errors.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct h2_pal_timer h2_pal_timer_t;

typedef enum h2_pal_timer_flag {
    H2_PAL_TIMER_FLAG_NONE = 0,
    H2_PAL_TIMER_FLAG_REPEAT = 1u << 0,
    H2_PAL_TIMER_FLAG_AUTO_START = 1u << 1,
} h2_pal_timer_flag_t;

/*
 * Timer callbacks may run from a platform timer task, RTOS timer service, host
 * event loop, or another backend-defined context. Keep callbacks short and
 * non-blocking; post to a queue or event system when app serialization is
 * needed.
 */
typedef void (*h2_pal_timer_cb_t)(
    void *user,
    h2_pal_timer_t *timer);

typedef struct h2_pal_timer_config {
    const char *name;
    uint32_t period_ms;
    uint32_t flags;
    h2_pal_timer_cb_t cb;
    void *cb_user;
} h2_pal_timer_config_t;

typedef struct h2_pal_timer_vtable {
    h2_pal_result_t (*create)(
        void *user,
        const h2_pal_timer_config_t *config,
        h2_pal_timer_t **out_timer);
    h2_pal_result_t (*destroy)(void *user, h2_pal_timer_t *timer);
    h2_pal_result_t (*start)(void *user, h2_pal_timer_t *timer);
    h2_pal_result_t (*stop)(void *user, h2_pal_timer_t *timer);
    h2_pal_result_t (*reset)(void *user, h2_pal_timer_t *timer);
    h2_pal_result_t (*set_period_ms)(
        void *user,
        h2_pal_timer_t *timer,
        uint32_t period_ms);
    h2_pal_result_t (*is_running)(
        void *user,
        h2_pal_timer_t *timer,
        int *out_running);
} h2_pal_timer_vtable_t;

typedef struct h2_pal_timer_api {
    void *user;
    const h2_pal_timer_vtable_t *vtable;
} h2_pal_timer_api_t;

static inline h2_pal_result_t h2_pal_timer_create(
    const h2_pal_timer_api_t *api,
    const h2_pal_timer_config_t *config,
    h2_pal_timer_t **out_timer) {
    if (config == NULL || out_timer == NULL || config->cb == NULL || config->period_ms == 0u) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL || api->vtable->create == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->create(api->user, config, out_timer);
}

static inline h2_pal_result_t h2_pal_timer_destroy(
    const h2_pal_timer_api_t *api,
    h2_pal_timer_t *timer) {
    if (timer == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL || api->vtable->destroy == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->destroy(api->user, timer);
}

static inline h2_pal_result_t h2_pal_timer_start(
    const h2_pal_timer_api_t *api,
    h2_pal_timer_t *timer) {
    if (timer == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL || api->vtable->start == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->start(api->user, timer);
}

static inline h2_pal_result_t h2_pal_timer_stop(
    const h2_pal_timer_api_t *api,
    h2_pal_timer_t *timer) {
    if (timer == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL || api->vtable->stop == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->stop(api->user, timer);
}

static inline h2_pal_result_t h2_pal_timer_reset(
    const h2_pal_timer_api_t *api,
    h2_pal_timer_t *timer) {
    if (timer == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL || api->vtable->reset == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->reset(api->user, timer);
}

static inline h2_pal_result_t h2_pal_timer_set_period_ms(
    const h2_pal_timer_api_t *api,
    h2_pal_timer_t *timer,
    uint32_t period_ms) {
    if (timer == NULL || period_ms == 0u) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL || api->vtable->set_period_ms == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->set_period_ms(api->user, timer, period_ms);
}

static inline h2_pal_result_t h2_pal_timer_is_running(
    const h2_pal_timer_api_t *api,
    h2_pal_timer_t *timer,
    int *out_running) {
    if (timer == NULL || out_running == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL || api->vtable->is_running == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->is_running(api->user, timer, out_running);
}

#ifdef __cplusplus
}
#endif

#endif
