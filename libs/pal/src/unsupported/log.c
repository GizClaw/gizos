#include "h2_pal.h"
#include <stddef.h>
#include <string.h>

static int unsupported_log_write(void *p0, h2_pal_log_level_t p1, const char *p2, const char *p3) {
    (void)p0;
    (void)p1;
    (void)p2;
    (void)p3;
    return H2_PAL_ERR_UNSUPPORTED;
}

static const h2_pal_log_vtable_t unsupported_log_vtable = {
    .write = unsupported_log_write,
};
static const h2_pal_log_api_t unsupported_log_api = { .user = NULL, .vtable = &unsupported_log_vtable };
const h2_pal_log_api_t *h2_pal_unsupported_log_api(void) { return &unsupported_log_api; }
