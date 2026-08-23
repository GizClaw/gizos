#include "h2_pal.h"
#include <stddef.h>
#include <string.h>

static int unsupported_pref_open(void *p0, const char *p1, h2_pal_pref_open_mode_t p2, h2_pal_pref_namespace_t **p3) {
    (void)p0;
    (void)p1;
    (void)p2;
    (void)p3;
    return H2_PAL_ERR_UNSUPPORTED;
}

static const h2_pal_pref_vtable_t unsupported_pref_vtable = {
    .open = unsupported_pref_open,
};
static const h2_pal_pref_api_t unsupported_pref_api = { .user = NULL, .vtable = &unsupported_pref_vtable };
const h2_pal_pref_api_t *h2_pal_unsupported_pref_api(void) { return &unsupported_pref_api; }
