#ifndef H2_PAL_TOUCH_H
#define H2_PAL_TOUCH_H

#include "h2/pal/core/h2_pal_errors.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum h2_pal_touch_event_kind {
    H2_PAL_TOUCH_EVENT_DOWN = 1,
    H2_PAL_TOUCH_EVENT_MOVE,
    H2_PAL_TOUCH_EVENT_UP,
} h2_pal_touch_event_kind_t;

/** Logical viewport exposed by one single-pointer Touch provider. */
typedef struct h2_pal_touch_info {
    uint32_t width;
    uint32_t height;
} h2_pal_touch_info_t;

/** One calibrated raw pointer edge. */
typedef struct h2_pal_touch_event {
    h2_pal_touch_event_kind_t kind;
    int32_t x;
    int32_t y;
} h2_pal_touch_event_t;

/**
 * Touch provider lifecycle and nonblocking event source.
 *
 * get_info() is valid after open(). poll_event() returns
 * H2_PAL_ERR_WOULD_BLOCK when no complete event is available. close() releases
 * provider resources and must tolerate a successfully opened session.
 */
typedef struct h2_pal_touch_vtable {
    h2_pal_result_t (*open)(void *user);
    h2_pal_result_t (*get_info)(void *user, h2_pal_touch_info_t *out_info);
    h2_pal_result_t (*poll_event)(void *user, h2_pal_touch_event_t *out_event);
    h2_pal_result_t (*close)(void *user);
} h2_pal_touch_vtable_t;

typedef struct h2_pal_touch_api {
    void *user;
    const h2_pal_touch_vtable_t *vtable;
} h2_pal_touch_api_t;

/** Open the borrowed Touch provider. */
static inline h2_pal_result_t h2_pal_touch_open(
    const h2_pal_touch_api_t *api) {
    if (api == NULL || api->vtable == NULL || api->vtable->open == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->open(api->user);
}

/** Read logical viewport metadata for an open provider. */
static inline h2_pal_result_t h2_pal_touch_get_info(
    const h2_pal_touch_api_t *api,
    h2_pal_touch_info_t *out_info) {
    if (out_info == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL || api->vtable->get_info == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->get_info(api->user, out_info);
}

/** Poll one complete down, move, or up event without blocking. */
static inline h2_pal_result_t h2_pal_touch_poll_event(
    const h2_pal_touch_api_t *api,
    h2_pal_touch_event_t *out_event) {
    if (out_event == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL || api->vtable->poll_event == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->poll_event(api->user, out_event);
}

/** Close the Touch provider and release its session resources. */
static inline h2_pal_result_t h2_pal_touch_close(
    const h2_pal_touch_api_t *api) {
    if (api == NULL || api->vtable == NULL || api->vtable->close == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->close(api->user);
}

#ifdef __cplusplus
}
#endif

#endif
