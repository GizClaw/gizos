#ifndef H2_MQTT_CONTROL_ALLOCATOR_H
#define H2_MQTT_CONTROL_ALLOCATOR_H

#include "h2/pal/os/h2_pal_mem.h"
#include "h2/pal/core/h2_pal_errors.h"

/* Artifact-owned accounting for borrowed management buffers. Every operation
 * still delegates to the native allocator and remains visible in raw totals.
 * The lock covers both native counters and the command-owned counters. */
typedef struct h2_mqtt_control_allocator {
    const h2_pal_mem_api_t *backing;
    void *lock_user;
    void (*lock)(void *);
    void (*unlock)(void *);
    size_t (*usable_size)(void *);
    size_t allocations, bytes;
    h2_pal_mem_api_t api;
} h2_mqtt_control_allocator_t;

static void *h2_mqtt_control_alloc(void *user, size_t length) {
    h2_mqtt_control_allocator_t *s = user;
    s->lock(s->lock_user);
    void *p = h2_pal_mem_alloc(s->backing, length);
    if (p != NULL) { ++s->allocations; s->bytes += s->usable_size(p); }
    s->unlock(s->lock_user);
    return p;
}
static void h2_mqtt_control_free(void *user, void *p) {
    h2_mqtt_control_allocator_t *s = user;
    if (p == NULL) return;
    s->lock(s->lock_user);
    s->bytes -= s->usable_size(p);
    --s->allocations;
    h2_pal_mem_free(s->backing, p);
    s->unlock(s->lock_user);
}
static void *h2_mqtt_control_realloc(void *user, void *p, size_t length) {
    h2_mqtt_control_allocator_t *s = user;
    if (p == NULL) return h2_mqtt_control_alloc(user, length);
    if (length == 0u) { h2_mqtt_control_free(user, p); return NULL; }
    s->lock(s->lock_user);
    size_t before = s->usable_size(p);
    void *next = h2_pal_mem_realloc(s->backing, p, length);
    if (next != NULL) s->bytes = s->bytes - before + s->usable_size(next);
    s->unlock(s->lock_user);
    return next;
}
static inline int h2_mqtt_control_init(h2_mqtt_control_allocator_t *s) {
    static const h2_pal_mem_vtable_t vtable = {
        .alloc = h2_mqtt_control_alloc,
        .realloc = h2_mqtt_control_realloc,
        .free = h2_mqtt_control_free,
    };
    if (s == NULL || s->backing == NULL || s->backing->vtable == NULL ||
        s->backing->vtable->alloc == NULL || s->backing->vtable->realloc == NULL ||
        s->backing->vtable->free == NULL || s->lock == NULL || s->unlock == NULL ||
        s->usable_size == NULL) return H2_PAL_ERR_INVALID_ARG;
    s->api = (h2_pal_mem_api_t){.user = s, .vtable = &vtable};
    return H2_PAL_OK;
}
/* Caller holds the same lock while reading native totals and filtering them.
 * Only this allocator's actual live buffers are excluded. Owned leaks cannot
 * cancel command-session frees, and inconsistent totals fail closed. */
static inline int h2_mqtt_control_filter(
    const h2_mqtt_control_allocator_t *s, size_t totals[10]) {
    if (s == NULL || totals == NULL) return H2_PAL_ERR_INVALID_ARG;
    if (totals[8] < s->allocations || totals[9] < s->bytes)
        return H2_PAL_ERR_INVALID_STATE;
    totals[8] -= s->allocations;
    totals[9] -= s->bytes;
    return H2_PAL_OK;
}
#endif
