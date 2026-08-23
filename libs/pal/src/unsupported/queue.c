#include "h2_pal.h"
#include <stddef.h>
#include <string.h>

static int unsupported_queue_create(void *p0, const h2_pal_queue_config_t *p1, h2_pal_queue_t **p2) {
    (void)p0;
    (void)p1;
    (void)p2;
    return H2_PAL_ERR_UNSUPPORTED;
}

static void unsupported_queue_destroy(void *p0, h2_pal_queue_t *p1) {
    (void)p0;
    (void)p1;
}

static int unsupported_queue_send(void *p0, h2_pal_queue_t *p1, const void *p2, uint32_t p3) {
    (void)p0;
    (void)p1;
    (void)p2;
    (void)p3;
    return H2_PAL_ERR_UNSUPPORTED;
}

static int unsupported_queue_send_latest(void *p0, h2_pal_queue_t *p1, const void *p2) {
    (void)p0;
    (void)p1;
    (void)p2;
    return H2_PAL_ERR_UNSUPPORTED;
}

static int unsupported_queue_recv(void *p0, h2_pal_queue_t *p1, void *p2, uint32_t p3) {
    (void)p0;
    (void)p1;
    (void)p2;
    (void)p3;
    return H2_PAL_ERR_UNSUPPORTED;
}

static int unsupported_queue_reset(void *p0, h2_pal_queue_t *p1) {
    (void)p0;
    (void)p1;
    return H2_PAL_ERR_UNSUPPORTED;
}

static int unsupported_queue_close(void *p0, h2_pal_queue_t *p1) {
    (void)p0;
    (void)p1;
    return H2_PAL_ERR_UNSUPPORTED;
}

static const h2_pal_queue_vtable_t unsupported_queue_vtable = {
    .create = unsupported_queue_create,
    .destroy = unsupported_queue_destroy,
    .send = unsupported_queue_send,
    .send_latest = unsupported_queue_send_latest,
    .recv = unsupported_queue_recv,
    .reset = unsupported_queue_reset,
    .close = unsupported_queue_close,
};
static const h2_pal_queue_api_t unsupported_queue_api = { .user = NULL, .vtable = &unsupported_queue_vtable };
const h2_pal_queue_api_t *h2_pal_unsupported_queue_api(void) { return &unsupported_queue_api; }
