#include "h2_pal_http_e2e.h"
#include "h2_corehttp.h"
#include "h2_desktop_platform.h"
#include "h2_wolfssl.h"
#if defined(__APPLE__)
#include "h2_darwin_platform.h"
#define host_net h2_darwin_net_api
#define host_entropy h2_darwin_entropy
#else
#include "h2_linux_platform.h"
#define host_net h2_linux_net_api
#define host_entropy h2_linux_entropy
#endif
#include <stdio.h>
#include <stdlib.h>

static void report(void *user, const h2_pal_http_e2e_case_result_t *result) {
    (void)user;
    printf("H2_PAL_HTTP_CASE {\"id\":\"%s\",\"status\":\"%s\",\"detail\":%d,\"line\":%u}\n",
        result->id, result->passed ? "PASS" : result->blocked ? "BLOCKED" : "FAIL",
        result->detail, result->line);
    fflush(stdout);
}

int main(int argc, char **argv) {
    if (argc != 5) return 2;
    FILE *file = fopen(argv[4], "rb");
    uint8_t ca[8192];
    if (file == NULL) return 2;
    size_t ca_len = fread(ca, 1u, sizeof(ca), file);
    int read_ok = !ferror(file) && feof(file);
    fclose(file);
    if (!read_ok || ca_len == 0u) return 2;
    h2_runtime_t runtime = {0};
    runtime.mem = h2_desktop_platform_default_allocator();
    runtime.net = host_net();
    runtime.time = h2_desktop_platform_time_api();
    h2_wolfssl_config_t tls = {.mem = *runtime.mem, .entropy = host_entropy};
    if (h2_wolfssl_init(&tls) != H2_PAL_OK) return 2;
    h2_corehttp_t *provider = NULL;
    h2_pal_http_api_t api;
    h2_corehttp_config_t provider_config = {
        .allocator = runtime.mem, .net = runtime.net, .time = runtime.time,
        .root_ca_pem = ca, .root_ca_pem_len = ca_len,
        .tls_verify = H2_PAL_NET_TLS_VERIFY_REQUIRED, .io_slice_ms = 20u,
    };
    if (h2_corehttp_create(&provider_config, &provider, &api) != H2_PAL_OK) {
        (void)h2_wolfssl_deinit();
        return 2;
    }
    runtime.http = &api;
    h2_pal_http_e2e_config_t config = {
        .runtime = &runtime, .http_base = argv[1], .https_base = argv[2],
        .untrusted_https_base = argv[3], .report = report,
    };
    h2_pal_http_e2e_result_t result;
    int rc = h2_pal_http_e2e_run(&config, &result);
    h2_corehttp_destroy(provider);
    int cleanup = h2_wolfssl_deinit();
    printf("H2_PAL_HTTP_SUMMARY {\"passed\":%u,\"failed\":%u,\"blocked\":%u,\"retained_allocations\":%zu,\"cleanup\":%d}\n",
        result.passed, result.failed, result.blocked, result.retained_allocations, cleanup);
    return rc == H2_PAL_OK && cleanup == H2_PAL_OK ? 0 : 1;
}
