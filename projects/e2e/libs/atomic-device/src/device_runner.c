#include "device_runner.h"
#include <stdio.h>
static h2_atomic_qualification_result_t results[2];
static const char *image;
int h2_atomic_device_run(h2_runtime_t *runtime, const char *version,
                         h2_atomic_qualification_config_t *config,
                         const h2_pal_mem_api_t *external) {
  if (!runtime || !version || !config || !external || image)
    return H2_PAL_ERR_INVALID_ARG;
  image = version;
  printf("H2_ATOMIC_BOOT version=%s\n", version);
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
    h2_atomic_e2e_print("device", placement ? "psram-wrapper" : "internal",
                        &results[placement]);
    if (results[placement].teardown)
      break;
  }
  config->mem = internal;
  config->placement_user = NULL;
  h2_atomic_device_replay();
  return aggregate;
}
void h2_atomic_device_replay(void) {
  if (!image)
    return;
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
