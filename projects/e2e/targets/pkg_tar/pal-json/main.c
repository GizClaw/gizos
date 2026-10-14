#include "h2_web_application_host.h"
#include "h2_pal_json_e2e.h"
#include "probe.h"
#include <emscripten/threading.h>
#include <stdio.h>
static h2_pal_json_e2e_result_t result;
static h2_pal_result_t run(h2_application_host_t *host, h2_runtime_t *runtime, void *user) {
  (void)user;
  return h2_pal_json_e2e_run(runtime, h2_web_application_host_json(host), h2_json_yyjson_probe, &result);
}
int main(void) {
  const h2_web_application_host_config_t config = {.name = "pal-json", .display_width = 1, .display_height = 1, .json = 1};
  const h2_application_host_config_t app = {.entry = run};
  h2_application_host_result_t lifecycle = {0};
  int rc = h2_web_application_host_run_app(&config, &app, &lifecycle);
  int teardown = lifecycle.cleanup;
  const char *names[] = {"NOT_RUN", "PASS", "FAIL", "BLOCKED"};
  for (size_t i = 0; i < H2_PAL_JSON_E2E_CASE_COUNT; ++i) {
    const h2_pal_json_e2e_case_t *c = &result.cases[i];
    if (c->id)
      printf("H2_JSON_CASE {\"id\":\"%s\",\"status\":\"%s\",\"rc\":%d}\n",
             c->id, names[c->status], c->result);
  }
  printf("H2_JSON_REPORT "
         "{\"platform\":\"wasm-chromium\",\"contract\":1,\"operations\":24,"
         "\"passed\":%zu,\"failed\":%zu,\"blocked\":%zu,\"not_run\":%zu,"
         "\"complete\":%d,\"qualified\":%d,\"rc\":%d,\"teardown\":%d}\n",
         result.passed, result.failed, result.blocked, result.not_run,
         result.complete, result.qualified, rc, teardown);
  return rc || teardown || lifecycle.retained || !result.qualified ? 1 : 0;
}
