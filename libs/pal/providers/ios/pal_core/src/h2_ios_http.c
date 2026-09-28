#include "h2_ios_platform.h"
#include "h2_ios_tls_internal.h"
#include "h2_corehttp.h"
#include "h2_posix_pal_core.h"
#include "h2/pal/h2_pal_unsupported.h"
#include <string.h>

struct h2_ios_http {
    h2_corehttp_t *provider;
    h2_pal_http_api_t api;
};

h2_pal_result_t h2_ios_http_create(const uint8_t *root_ca_pem,
    size_t root_ca_pem_len, h2_ios_http_t **out) {
    if (out == NULL) return H2_PAL_ERR_INVALID_ARG;
    *out = NULL;
    if ((root_ca_pem == NULL) != (root_ca_pem_len == 0u)) return H2_PAL_ERR_INVALID_ARG;
    int rc = h2_ios_tls_acquire();
    if (rc != H2_PAL_OK) return (h2_pal_result_t)rc;
    const h2_pal_mem_api_t *memory = h2_ios_platform_mem_api();
    h2_ios_http_t *http = h2_pal_mem_alloc(memory, sizeof(*http));
    if (http == NULL) {
        (void)h2_ios_tls_release();
        return H2_PAL_ERR_NO_MEMORY;
    }
    memset(http, 0, sizeof(*http));
    const h2_corehttp_config_t config = {
        .allocator = memory, .net = h2_posix_net_api(),
        .time = h2_ios_platform_time_api(),
        .root_ca_pem = root_ca_pem, .root_ca_pem_len = root_ca_pem_len,
        .tls_verify = H2_PAL_NET_TLS_VERIFY_REQUIRED,
        .io_slice_ms = 20u,
    };
    rc = h2_corehttp_create(&config, &http->provider, &http->api);
    if (rc != H2_PAL_OK) {
        h2_pal_mem_free(memory, http);
        (void)h2_ios_tls_release();
        return (h2_pal_result_t)rc;
    }
    *out = http;
    return H2_PAL_OK;
}
const h2_pal_http_api_t *h2_ios_http_api(h2_ios_http_t *http) {
    return http != NULL ? &http->api : h2_pal_unsupported_http_api();
}
void h2_ios_http_destroy(h2_ios_http_t *http) {
    if (http == NULL) return;
    h2_corehttp_destroy(http->provider);
    h2_pal_mem_free(h2_ios_platform_mem_api(), http);
    (void)h2_ios_tls_release();
}
