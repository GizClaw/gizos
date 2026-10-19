#include "h2_pal_json_e2e.h"
#include "h2_smoke_host_runtime.h"
#include "h2_web_platform.h"
#include "h2_yyjson_json.h"
#include "probe.h"
#include <emscripten/threading.h>
#include <stdio.h>
int main(void) {
  if (emscripten_is_main_runtime_thread())
    return 2;
  const h2_web_platform_config_t config = {.display_width = 1,
                                           .display_height = 1};
  h2_web_platform_t *platform = h2_web_platform_create(&config);
  if (!platform)
    return 3;
  h2_runtime_config_t cfg = h2_smoke_host_runtime_config(
      "pal-json", "web", "wasm32", h2_web_platform_mem_api(),
      h2_web_platform_time_api(platform), h2_web_platform_queue_api(platform),
      h2_pal_unsupported_display_api());
  h2_yyjson_json_t *provider = NULL;
  int rc = h2_yyjson_json_create(h2_web_platform_mem_api(), &provider);
  h2_runtime_t *runtime = NULL;
  if (!rc) rc = h2_runtime_init(&cfg, &runtime);
  h2_pal_json_e2e_result_t result = {0};
  if (!rc)
    rc = h2_pal_json_e2e_run(runtime, h2_yyjson_json_api(provider), h2_json_yyjson_probe, &result);
  if (runtime)
    h2_runtime_deinit(runtime);
  int teardown = h2_yyjson_json_destroy(&provider);
  int web_teardown = h2_web_platform_destroy(platform);
  if (!teardown) teardown = web_teardown;
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
  return rc || teardown || !result.qualified ? 1 : 0;
}
