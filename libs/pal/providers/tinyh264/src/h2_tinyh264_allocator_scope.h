#ifndef H2_TINYH264_ALLOCATOR_SCOPE_H
#define H2_TINYH264_ALLOCATOR_SCOPE_H

#include "h2/pal/os/h2_pal_mem.h"

/* Caller-owned stack node, valid from enter through leave on the same task.
 * Allocator callbacks may nest scopes; every successful enter must be left. */
typedef struct h2_tinyh264_allocator_scope {
    struct h2_tinyh264_allocator_scope *next;
    const void *task;
    const h2_pal_mem_api_t *allocator;
    const h2_pal_mem_api_t *previous;
} h2_tinyh264_allocator_scope_t;

void h2_tinyh264_allocator_scope_enter(
    h2_tinyh264_allocator_scope_t *scope, const h2_pal_mem_api_t *allocator);
void h2_tinyh264_allocator_scope_leave(h2_tinyh264_allocator_scope_t *scope);

#endif
