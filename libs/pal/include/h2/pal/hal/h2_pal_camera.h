#ifndef H2_PAL_CAMERA_H
#define H2_PAL_CAMERA_H
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "h2/pal/core/h2_pal_errors.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Serialized, single-consumer capture. A borrowed frame remains valid until
 * release; stop must return BUSY while a frame is borrowed. Pixel bytes are
 * U0,Y0,V0,Y1. Callers must honor stride and size. */
typedef struct {
    const uint8_t *data;
    size_t size;
    uint32_t width, height, stride;
    uint64_t token;
} h2_pal_camera_frame_t;
typedef struct {
    h2_pal_result_t (*start)(void *user);
    h2_pal_result_t (*acquire)(void *user, h2_pal_camera_frame_t *frame);
    h2_pal_result_t (*release)(void *user, const h2_pal_camera_frame_t *frame);
    h2_pal_result_t (*stop)(void *user);
} h2_pal_camera_vtable_t;
typedef struct {
    void *user;
    const h2_pal_camera_vtable_t *vtable;
} h2_pal_camera_api_t;
/* This initial API exposes UYVY only. Acquire uses the provider's finite
 * timeout, documented by that provider. Missing optional APIs return UNSUPPORTED.
 * Callers serialize all operations and keep the acquired descriptor until a
 * successful release; release failure retains the loan for retry. Acquire
 * clears its output even on error: never reuse a live loan as acquire output.
 * Successful stop is idempotent. A failed stop retains provider state for retry.
 * No Runtime binding, format negotiation or concurrent consumer is implied. */
static inline h2_pal_result_t h2_pal_camera_start(const h2_pal_camera_api_t *a) {
    return a && a->vtable && a->vtable->start ? a->vtable->start(a->user) : H2_PAL_ERR_UNSUPPORTED;
}
static inline h2_pal_result_t h2_pal_camera_acquire(const h2_pal_camera_api_t *a, h2_pal_camera_frame_t *f) {
    if (!f) return H2_PAL_ERR_INVALID_ARG;
    memset(f, 0, sizeof(*f));
    return a && a->vtable && a->vtable->acquire ? a->vtable->acquire(a->user, f) : H2_PAL_ERR_UNSUPPORTED;
}
static inline h2_pal_result_t h2_pal_camera_release(const h2_pal_camera_api_t *a, const h2_pal_camera_frame_t *f) {
    if (!f) return H2_PAL_ERR_INVALID_ARG;
    return a && a->vtable && a->vtable->release ? a->vtable->release(a->user, f) : H2_PAL_ERR_UNSUPPORTED;
}
static inline h2_pal_result_t h2_pal_camera_stop(const h2_pal_camera_api_t *a) {
    return a && a->vtable && a->vtable->stop ? a->vtable->stop(a->user) : H2_PAL_ERR_UNSUPPORTED;
}
#ifdef __cplusplus
}
#endif
#endif
