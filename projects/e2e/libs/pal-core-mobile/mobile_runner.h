#ifndef H2_MOBILE_CORE_RUNNER_H
#define H2_MOBILE_CORE_RUNNER_H
#include "h2_pal_core_e2e.h"
/* Platform observer functions use the real provider and OS. This harness owns
 * Runtime teardown and emits the unchanged portable case ledger as JSON. */
typedef struct h2_mobile_core_fixture {
  const char *platform;
  const char *image_version;
  h2_runtime_config_t runtime_config;
  h2_pal_core_e2e_config_t tests;
  h2_pal_result_t (*shutdown)(void);
} h2_mobile_core_fixture_t;
int h2_mobile_core_run(const h2_mobile_core_fixture_t *fixture,
                       const char *report_path);
h2_pal_result_t h2_mobile_core_clock(void *user, uint64_t *out);
h2_pal_result_t h2_mobile_core_stack(void *user, size_t *out);
#endif
