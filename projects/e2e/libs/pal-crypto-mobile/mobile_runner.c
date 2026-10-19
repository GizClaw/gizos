#include "mobile_runner.h"
#include "h2_pal_crypto_e2e.h"
#include <stdio.h>
#include <unistd.h>
int h2_crypto_mobile_run(h2_runtime_config_t config, const char *platform,
                         const char *version, const char *path,
                         int (*shutdown)(void)) {
  h2_runtime_t *runtime = NULL;
  h2_pal_crypto_e2e_result_t result = {0};
  int rc = h2_runtime_init(&config, &runtime);
  if (!rc)
    rc = h2_pal_crypto_e2e_run(runtime, &result);
  if (runtime)
    h2_runtime_deinit(runtime);
  int teardown = shutdown();
  FILE *f = fopen(path, "w");
  if (!f)
    return H2_PAL_ERR_IO;
  fprintf(f,
          "{\"platform\":\"%s\",\"version\":\"%s\",\"pid\":%ld,\"contract\":1,"
          "\"operations\":15,\"passed\":%zu,\"failed\":%zu,\"blocked\":%zu,"
          "\"not_run\":%zu,\"complete\":%d,\"qualified\":%d,\"rc\":%d,"
          "\"teardown\":%d,\"cases\":[",
          platform, version, (long)getpid(), result.passed, result.failed,
          result.blocked, result.not_run, result.complete, result.qualified, rc,
          teardown);
  const char *names[] = {"NOT_RUN", "PASS", "FAIL", "BLOCKED"};
  for (size_t i = 0; i < H2_PAL_CRYPTO_E2E_CASE_COUNT; ++i) {
    const h2_pal_crypto_e2e_case_t *c = &result.cases[i];
    fprintf(f, "%s{\"id\":\"%s\",\"status\":\"%s\",\"rc\":%d}", i ? "," : "",
            c->id ? c->id : "", names[c->status], c->result);
  }
  fputs("]}\n", f);
  if (fclose(f))
    return H2_PAL_ERR_IO;
  return rc || teardown || !result.qualified ? H2_PAL_ERR_INVALID_STATE
                                             : H2_PAL_OK;
}
