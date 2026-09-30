#include "device_runner.h"
#include <stdio.h>
static h2_atomic_qualification_result_t results[2];
static const char *image;
static char execution[33];
static bool admitted;
static int cleanup_result, confirmation_result;
static const h2_pal_time_api_t *report_time;
int h2_atomic_device_run(h2_runtime_t *runtime, const char *version,
                         h2_atomic_qualification_config_t *config,
                         const h2_pal_mem_api_t *external) {
  if (!runtime || !version || !config || !external || image)
    return H2_PAL_ERR_INVALID_ARG;
  uint8_t nonce[16];
  int random_rc = h2_pal_crypto_random(runtime->crypto, nonce, sizeof(nonce));
  if (random_rc)
    return random_rc;
  static const char hex[] = "0123456789abcdef";
  for (unsigned i = 0; i < 16; ++i) {
    execution[2 * i] = hex[nonce[i] >> 4];
    execution[2 * i + 1] = hex[nonce[i] & 15];
  }
  execution[32] = 0;
  report_time = runtime->time;
  image = version;
  printf("H2_ATOMIC_BOOT version=%s execution=%s\n", version, execution);
  const h2_pal_mem_api_t *internal = config->mem;
  int aggregate = 0;
  for (unsigned placement = 0; placement < 2; ++placement) {
    config->mem = placement ? external : internal;
    /* Placement callback borrows this bool only during this synchronous run. */
    bool psram = placement != 0;
    config->placement_user = &psram;
    int rc = h2_atomic_e2e_qualify(config, &results[placement]);
    if (rc)
      aggregate = rc;
    printf("H2_ATOMIC_PLACEMENT_RESULT placement=%s passed=%u failed=%u "
           "workers=%u/%u rc=%d\n",
           placement ? "psram-wrapper" : "internal", results[placement].passed,
           results[placement].failed, results[placement].workers_joined,
           results[placement].workers_started, rc);
    if (results[placement].teardown)
      break;
  }
  config->mem = internal;
  config->placement_user = NULL;
  return aggregate;
}
void h2_atomic_device_admit(int cleanup, int confirmation) {
  cleanup_result = cleanup;
  confirmation_result = confirmation;
  admitted = cleanup == 0 && confirmation == 0 && results[0].qualified &&
             results[1].qualified;
}
void h2_atomic_device_replay(void) {
  if (!image || !admitted)
    return;
  printf("H2_ATOMIC_EXECUTION "
         "{\"version\":\"%s\",\"execution\":\"%s\",\"cleanup\":%d,\"confirm\":%"
         "d}\n",
         image, execution, cleanup_result, confirmation_result);
  for (unsigned p = 0; p < 2; ++p) {
    for (unsigned i = 0; i < H2_ATOMIC_QUALIFICATION_CASE_COUNT; ++i) {
      const h2_atomic_qualification_case_t *c = &results[p].cases[i];
      printf("H2_ATOMIC_CASE "
             "{\"version\":\"%s\",\"placement\":\"%s\",\"id\":\"%s\","
             "\"status\":\"%s\",\"rc\":%d}\n",
             image, p ? "psram-wrapper" : "internal", c->id ? c->id : "",
             c->status == 1   ? "PASS"
             : c->status == 2 ? "FAIL"
                              : "NOT_RUN",
             c->rc);
      (void)h2_pal_time_sleep_ms(report_time, 50);
    }
  }
  printf("H2_ATOMIC_REPORT "
         "{\"version\":\"%s\",\"passed\":%u,\"failed\":%u,\"not_run\":%u,"
         "\"workers_started\":%u,\"workers_joined\":%u,\"qualified\":%u,"
         "\"teardown\":%d}\n",
         image, results[0].passed + results[1].passed,
         results[0].failed + results[1].failed,
         results[0].not_run + results[1].not_run,
         results[0].workers_started + results[1].workers_started,
         results[0].workers_joined + results[1].workers_joined,
         results[0].qualified && results[1].qualified,
         results[0].teardown ? results[0].teardown : results[1].teardown);
}
