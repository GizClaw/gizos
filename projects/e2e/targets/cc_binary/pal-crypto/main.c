#include "h2_desktop_platform.h"
#include "h2_pal_crypto_e2e.h"
#include "h2_smoke_host_runtime.h"
#include "h2_wolfssl.h"
#if defined(__APPLE__)
#include "h2_darwin_platform.h"
#define host_entropy h2_darwin_entropy
#else
#include "h2_linux_platform.h"
#define host_entropy h2_linux_entropy
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
  const h2_wolfssl_config_t crypto = {
      .mem = *h2_desktop_platform_default_allocator(), .entropy = host_entropy};
  if (h2_wolfssl_init(&crypto) != H2_PAL_OK)
    return 2;
  h2_runtime_config_t config = h2_smoke_host_runtime_config(
      "pal-crypto", "desktop", "host", h2_desktop_platform_default_allocator(),
      h2_desktop_platform_time_api(), h2_desktop_platform_queue_api(),
      h2_pal_unsupported_display_api());
  config.crypto = h2_wolfssl_crypto_api();
  h2_runtime_t *runtime = NULL;
  if (h2_runtime_init(&config, &runtime) != H2_PAL_OK) {
    (void)h2_wolfssl_deinit();
    return 3;
  }
  h2_pal_crypto_e2e_result_t result;
  int rc = h2_pal_crypto_e2e_run(runtime, &result);
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
  h2_runtime_deinit(runtime);
  (void)h2_wolfssl_deinit();
  return rc == 0 && result.qualified ? 0 : 1;
}
