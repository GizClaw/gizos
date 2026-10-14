#include "h2_desktop_application_host.h"
#include "h2_pal_http_e2e.h"
#include "h2_desktop_platform.h"
#include <stdio.h>
#include <stdlib.h>

static void report(void *user, const h2_pal_http_e2e_case_result_t *result) {
    (void)user;
    printf("H2_PAL_HTTP_CASE {\"id\":\"%s\",\"status\":\"%s\",\"detail\":%d,\"line\":%u,\"elapsed_ms\":%llu,\"request_result\":%d}\n",
        result->id, result->passed ? "PASS" : result->blocked ? "BLOCKED" : "FAIL",
        result->detail, result->line, (unsigned long long)result->elapsed_ms, result->request_result);
    fflush(stdout);
}

static h2_pal_http_e2e_config_t tests;
static h2_pal_http_e2e_result_t result;
static h2_pal_result_t run(h2_application_host_t *host, h2_runtime_t *runtime, void *user) {
    (void)user;
    tests.runtime = runtime;
    int rc = h2_pal_http_e2e_run(&tests, &result);
    if (result.retained_allocations) h2_application_host_retain(host, NULL, H2_PAL_ERR_BUSY);
    return rc;
}
int main(int argc, char **argv) {
    if (argc != 5) return 2;
    FILE *file = fopen(argv[4], "rb");
    static uint8_t ca[8192];
    if (file == NULL) return 2;
    size_t ca_len = fread(ca, 1u, sizeof(ca), file);
    int read_ok = !ferror(file) && feof(file);
    fclose(file);
    if (!read_ok || ca_len == 0u) return 2;
    const h2_desktop_application_host_config_t platform = {
        .services = H2_DESKTOP_HOST_HTTP,
        .http = {.root_ca_pem = ca, .root_ca_pem_len = ca_len,
                 .tls_verify = H2_PAL_NET_TLS_VERIFY_REQUIRED, .io_slice_ms = 20u}};
    tests = (h2_pal_http_e2e_config_t){.http_base = argv[1], .https_base = argv[2],
        .untrusted_https_base = argv[3], .report = report};
    const h2_application_host_config_t app = {.entry = run};
    h2_application_host_result_t lifecycle = {0};
    int rc = h2_desktop_application_host_run(&platform, &app, &lifecycle);
    printf("H2_PAL_HTTP_SUMMARY {\"passed\":%u,\"failed\":%u,\"blocked\":%u,\"retained_allocations\":%zu,\"cleanup\":%d}\n",
        result.passed, result.failed, result.blocked, result.retained_allocations, lifecycle.cleanup);
    return rc == H2_PAL_OK && lifecycle.cleanup == H2_PAL_OK && !lifecycle.retained ? 0 : 1;
}
