#include "h2_pal.h"
#include <stddef.h>
#include <string.h>

static int unsupported_task_start(void *p0, const h2_pal_task_options_t *p1, h2_pal_task_entry_t p2, void *p3, h2_pal_task_t **p4) {
    (void)p0;
    (void)p1;
    (void)p2;
    (void)p3;
    (void)p4;
    return H2_PAL_ERR_UNSUPPORTED;
}

static int unsupported_task_join(void *p0, h2_pal_task_t *p1) {
    (void)p0;
    (void)p1;
    return H2_PAL_ERR_UNSUPPORTED;
}

static const h2_pal_task_vtable_t unsupported_task_vtable = {
    .start = unsupported_task_start,
    .join = unsupported_task_join,
};
static const h2_pal_task_api_t unsupported_task_api = { .user = NULL, .vtable = &unsupported_task_vtable };
const h2_pal_task_api_t *h2_pal_unsupported_task_api(void) { return &unsupported_task_api; }
