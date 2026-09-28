#include "mobile_runner.h"
#include <stdio.h>
#include <unistd.h>

int h2_http_mobile_run(h2_runtime_config_t config, const char *http_base,
    const char *https_base, const char *untrusted_base, h2_pal_http_e2e_result_t *out) {
    h2_runtime_t *runtime = NULL;
    int rc = h2_runtime_init(&config, &runtime);
    if (rc == H2_PAL_OK) {
        const h2_pal_http_e2e_config_t fixture = {
            .runtime = runtime, .http_base = http_base, .https_base = https_base,
            .untrusted_https_base = untrusted_base,
        };
        rc = h2_pal_http_e2e_run(&fixture, out);
        h2_runtime_deinit(runtime);
    }
    return rc;
}
int h2_http_mobile_report(const char *path, const char *platform, const char *version,
    const h2_pal_http_e2e_result_t *result, int rc, int teardown) {
    FILE *file = fopen(path, "w");
    if (file == NULL) return H2_PAL_ERR_IO;
    fprintf(file, "{\"platform\":\"%s\",\"version\":\"%s\",\"pid\":%ld,\"contract\":1,\"operations\":2,\"passed\":%u,\"failed\":%u,\"blocked\":%u,\"retained_allocations\":%zu,\"rc\":%d,\"teardown\":%d,\"cases\":[",
        platform, version, (long)getpid(), result->passed, result->failed,
        result->blocked, result->retained_allocations, rc, teardown);
    for (unsigned index = 0u; index < H2_PAL_HTTP_E2E_CASE_COUNT; ++index) {
        const h2_pal_http_e2e_case_result_t *item = &result->cases[index];
        fprintf(file, "%s{\"id\":\"%s\",\"status\":\"%s\",\"detail\":%d,\"line\":%u,\"elapsed_ms\":%llu,\"request_result\":%d}",
            index == 0u ? "" : ",", item->id != NULL ? item->id : "",
            item->passed ? "PASS" : item->blocked ? "BLOCKED" : "FAIL", item->detail, item->line, (unsigned long long)item->elapsed_ms, item->request_result);
    }
    fputs("]}\n", file);
    if (ferror(file)) { fclose(file); return H2_PAL_ERR_IO; }
    if (fclose(file) != 0) return H2_PAL_ERR_IO;
    return rc == H2_PAL_OK && teardown == H2_PAL_OK &&
        result->passed == H2_PAL_HTTP_E2E_CASE_COUNT ? H2_PAL_OK : H2_PAL_ERR_IO;
}
