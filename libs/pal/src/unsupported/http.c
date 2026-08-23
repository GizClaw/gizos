#include "h2_pal.h"
#include <stddef.h>
#include <string.h>

static int unsupported_http_request(void *p0, const h2_pal_http_request_t *p1, h2_pal_http_response_t *p2) {
    (void)p0;
    (void)p1;
    (void)p2;
    return H2_PAL_ERR_UNSUPPORTED;
}

static void unsupported_http_response_free(void *p0, h2_pal_http_response_t *p1) {
    (void)p0;
    (void)p1;
}

static const h2_pal_http_vtable_t unsupported_http_vtable = {
    .request = unsupported_http_request,
    .response_free = unsupported_http_response_free,
};
static const h2_pal_http_api_t unsupported_http_api = { .user = NULL, .vtable = &unsupported_http_vtable };
const h2_pal_http_api_t *h2_pal_unsupported_http_api(void) { return &unsupported_http_api; }
