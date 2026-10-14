#include "h2_desktop_application_host.h"
#include "h2_pal_crypto_e2e.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static h2_pal_crypto_e2e_result_t result;
static h2_pal_result_t run(h2_application_host_t *host, h2_runtime_t *runtime, void *user) {
  (void)user;
  (void)host;
  return h2_pal_crypto_e2e_run(runtime, &result);
}
int main(void) {
  const h2_desktop_application_host_config_t config = {.services = H2_DESKTOP_HOST_CRYPTO};
  const h2_application_host_config_t app = {.entry = run};
  h2_application_host_result_t lifecycle = {0};
  int rc = h2_desktop_application_host_run(&config, &app, &lifecycle);
  int teardown = lifecycle.cleanup;
  if (teardown != H2_PAL_OK || lifecycle.retained) return 1;
  FILE *evidence = NULL;
  const char *directory = getenv("TEST_UNDECLARED_OUTPUTS_DIR");
  if (directory) {
    char path[4096];
    int n = snprintf(path, sizeof(path), "%s/qualified.json", directory);
    if (n < 0 || (size_t)n >= sizeof(path))
      return 4;
    evidence = fopen(path, "w");
    if (!evidence)
      return 4;
  }
  if (evidence)
    fprintf(
        evidence,
        "{\"platform\":\"desktop\",\"contract\":1,\"operations\":15,\"passed\":"
        "%zu,\"failed\":%zu,\"blocked\":%zu,\"qualified\":%d,\"cases\":[\n",
        result.passed, result.failed, result.blocked, result.qualified);
  const char *names[] = {"NOT_RUN", "PASS", "FAIL", "BLOCKED"};
  for (size_t i = 0; i < H2_PAL_CRYPTO_E2E_CASE_COUNT; ++i) {
    const h2_pal_crypto_e2e_case_t *c = &result.cases[i];
    if (!c->id) continue;
    printf("PAL_CRYPTO_CASE id=%s status=%s rc=%d\n", c->id, names[c->status],
           c->result);
    if (evidence)
      fprintf(evidence, "%s{\"id\":\"%s\",\"status\":\"%s\",\"rc\":%d}\n",
              i ? "," : "", c->id, names[c->status], c->result);
  }
  if (evidence) {
    fputs("]}\n", evidence);
    if (fclose(evidence))
      rc = H2_PAL_ERR_IO;
  }
  printf("PAL_CRYPTO_REPORT operations=15 passed=%zu failed=%zu blocked=%zu "
         "qualified=%d\n",
         result.passed, result.failed, result.blocked, result.qualified);
  return rc == 0 && !lifecycle.retained && result.qualified ? 0 : 1;
}
