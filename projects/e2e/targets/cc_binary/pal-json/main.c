#include "h2_desktop_platform.h"
#include "h2_pal_json_e2e.h"
#include "h2_smoke_host_runtime.h"
#include "h2_yyjson_json.h"
#include "probe.h"
#include <stdio.h>
#include <stdlib.h>

int main(void) {
    const h2_pal_mem_api_t *mem = h2_desktop_platform_default_allocator();
    h2_yyjson_json_t *provider = NULL;
    if (h2_yyjson_json_create(mem, &provider) != H2_PAL_OK) return 2;
    h2_runtime_config_t config = h2_smoke_host_runtime_config(
        "pal-json", "desktop", "host", mem,
        h2_desktop_platform_time_api(), h2_desktop_platform_queue_api(),
        h2_pal_unsupported_display_api());
    h2_runtime_t *runtime = NULL;
    int rc = h2_runtime_init(&config, &runtime);
    h2_pal_json_e2e_result_t result = {0};
    if (!rc) rc = h2_pal_json_e2e_run(runtime, h2_yyjson_json_api(provider), h2_json_yyjson_probe, &result);
    if (runtime) h2_runtime_deinit(runtime);
    int teardown = h2_yyjson_json_destroy(&provider);
    FILE *evidence = NULL;
    const char *directory = getenv("TEST_UNDECLARED_OUTPUTS_DIR");
    if (directory) {
        char path[4096];
        int n = snprintf(path, sizeof(path), "%s/qualified.json", directory);
        if (n < 0 || (size_t)n >= sizeof(path)) return 3;
        evidence = fopen(path, "w");
        if (!evidence) return 3;
    }
    const char *names[] = {"NOT_RUN", "PASS", "FAIL", "BLOCKED"};
    if (evidence) fprintf(evidence, "{\"platform\":\"macos\",\"contract\":1,\"operations\":24,\"passed\":%zu,\"failed\":%zu,\"blocked\":%zu,\"qualified\":%d,\"rc\":%d,\"teardown\":%d,\"cases\":[", result.passed, result.failed, result.blocked, result.qualified, rc, teardown);
    for (size_t i = 0; i < H2_PAL_JSON_E2E_CASE_COUNT; ++i) {
        const h2_pal_json_e2e_case_t *c = &result.cases[i];
        printf("H2_JSON_CASE {\"id\":\"%s\",\"status\":\"%s\",\"rc\":%d}\n", c->id, names[c->status], c->result);
        if (evidence) fprintf(evidence, "%s{\"id\":\"%s\",\"status\":\"%s\",\"rc\":%d}", i ? "," : "", c->id, names[c->status], c->result);
    }
    if (evidence) {
        fputs("]}\n", evidence);
        if (fclose(evidence)) rc = H2_PAL_ERR_IO;
    }
    printf("H2_JSON_REPORT {\"operations\":24,\"passed\":%zu,\"failed\":%zu,\"blocked\":%zu,\"qualified\":%d,\"rc\":%d,\"teardown\":%d}\n", result.passed, result.failed, result.blocked, result.qualified, rc, teardown);
    return rc || teardown || !result.qualified ? 1 : 0;
}
