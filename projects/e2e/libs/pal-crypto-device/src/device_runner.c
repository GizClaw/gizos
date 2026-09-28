#include "device_runner.h"
#include <stdio.h>
static h2_pal_crypto_e2e_result_t result;
static const char *image_version;
static int run_rc;
static int complete;
int h2_crypto_device_run(h2_runtime_t *runtime, const char *version) {
  if (image_version || !runtime || !version)
    return H2_PAL_ERR_INVALID_STATE;
  image_version = version;
  printf("H2_CRYPTO_BOOT version=%s\n", version);
  run_rc = h2_pal_crypto_e2e_run(runtime, &result);
  complete = 1;
  return run_rc;
}
void h2_crypto_device_replay(h2_runtime_t *runtime) {
  if (!complete)
    return;
  const char *names[] = {"NOT_RUN", "PASS", "FAIL", "BLOCKED"};
  for (size_t i = 0; i < H2_PAL_CRYPTO_E2E_CASE_COUNT; ++i) {
    const h2_pal_crypto_e2e_case_t *c = &result.cases[i];
    printf("H2_CRYPTO_CASE "
           "{\"version\":\"%s\",\"id\":\"%s\",\"status\":\"%s\",\"rc\":%d}\n",
           image_version, c->id, names[c->status], c->result);
    h2_pal_time_sleep_ms(runtime->time, 90);
  }
  printf("H2_CRYPTO_REPORT "
         "{\"version\":\"%s\",\"contract\":1,\"operations\":15,\"passed\":%zu,"
         "\"failed\":%zu,\"blocked\":%zu,\"not_run\":%zu,\"complete\":%d,"
         "\"qualified\":%d,\"rc\":%d}\n",
         image_version, result.passed, result.failed, result.blocked,
         result.not_run, result.complete, result.qualified, run_rc);
}
