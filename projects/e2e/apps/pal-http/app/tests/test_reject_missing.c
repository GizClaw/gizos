#include "h2_pal_http_e2e.h"
#include <stdlib.h>
#include <string.h>

static unsigned calls;
static int forbidden_request(void *user, const h2_pal_http_request_t *request,
                             h2_pal_http_response_t *response) {
    (void)user; (void)request; (void)response;
    ++calls;
    return H2_PAL_OK;
}
static void forbidden_free(void *user, h2_pal_http_response_t *response) {
    (void)user; (void)response;
    ++calls;
}
static void *allocate(void *user, size_t len) { (void)user; return malloc(len); }
static void *resize(void *user, void *ptr, size_t len) { (void)user; return realloc(ptr, len); }
static void release(void *user, void *ptr) { (void)user; free(ptr); }
static const h2_pal_mem_vtable_t memory_vtable = {allocate, resize, release};

int main(void) {
    const h2_pal_mem_api_t memory = {NULL, &memory_vtable};
    const h2_pal_time_api_t time = {0};
    h2_pal_http_vtable_t vtable = {forbidden_request, forbidden_free};
    h2_pal_http_api_t api = {NULL, &vtable};
    h2_runtime_t runtime = {.http = &api, .mem = &memory, .time = &time};
    h2_pal_http_e2e_config_t config = {
        .runtime = &runtime, .http_base = "http://invalid",
        .https_base = "https://invalid", .untrusted_https_base = "https://invalid",
    };
    for (unsigned missing = 0u; missing < 3u; ++missing) {
        api.vtable = &vtable;
        vtable.request = missing == 0u ? NULL : forbidden_request;
        vtable.response_free = missing == 1u ? NULL : forbidden_free;
        if (missing == 2u) api.vtable = NULL;
        h2_pal_http_e2e_result_t result;
        memset(&result, 0xa5, sizeof(result));
        if (h2_pal_http_e2e_run(&config, &result) == H2_PAL_OK || calls != 0u ||
            result.passed != 0u || result.failed != 0u ||
            result.blocked != H2_PAL_HTTP_E2E_CASE_COUNT) return 1;
    }
    return 0;
}
