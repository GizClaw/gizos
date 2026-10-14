#include "h2_desktop_application_host.h"
#include "h2_pal_json_e2e.h"
#include "probe.h"
#include <stdio.h>
#include <stdlib.h>

static h2_pal_json_e2e_result_t result;
static h2_pal_result_t run(h2_application_host_t *host, h2_runtime_t *runtime, void *user) {
  (void)user;
  return h2_pal_json_e2e_run(runtime, h2_desktop_application_host_json(host), h2_json_yyjson_probe, &result);
}
int main(void) {
  const h2_desktop_application_host_config_t config = {.services = H2_DESKTOP_HOST_JSON};
  const h2_application_host_config_t app = {.entry = run};
  h2_application_host_result_t lifecycle = {0};
  int rc = h2_desktop_application_host_run(&config, &app, &lifecycle);
  int teardown = lifecycle.cleanup;
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
        if (!c->id) continue;
        printf("H2_JSON_CASE {\"id\":\"%s\",\"status\":\"%s\",\"rc\":%d}\n", c->id, names[c->status], c->result);
        if (evidence) fprintf(evidence, "%s{\"id\":\"%s\",\"status\":\"%s\",\"rc\":%d}", i ? "," : "", c->id, names[c->status], c->result);
    }
    if (evidence) {
        fputs("]}\n", evidence);
        if (fclose(evidence)) rc = H2_PAL_ERR_IO;
    }
    printf("H2_JSON_REPORT {\"operations\":24,\"passed\":%zu,\"failed\":%zu,\"blocked\":%zu,\"qualified\":%d,\"rc\":%d,\"teardown\":%d}\n", result.passed, result.failed, result.blocked, result.qualified, rc, teardown);
    return rc || teardown || lifecycle.retained || !result.qualified ? 1 : 0;
}
