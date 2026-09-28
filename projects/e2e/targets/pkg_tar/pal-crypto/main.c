#include "h2_pal_crypto_e2e.h"
#include "h2_smoke_host_runtime.h"
#include "h2_web_platform.h"
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
      "pal-crypto", "web", "wasm32", h2_web_platform_mem_api(),
      h2_web_platform_time_api(platform), h2_web_platform_queue_api(platform),
      h2_pal_unsupported_display_api());
  cfg.crypto = h2_web_platform_crypto_api(platform);
  h2_runtime_t *runtime = NULL;
  int rc = h2_runtime_init(&cfg, &runtime);
  h2_pal_crypto_e2e_result_t result = {0};
  if (!rc)
    rc = h2_pal_crypto_e2e_run(runtime, &result);
  if (runtime)
    h2_runtime_deinit(runtime);
  int teardown = h2_web_platform_destroy(platform);
  const char *names[] = {"NOT_RUN", "PASS", "FAIL", "BLOCKED"};
  for (size_t i = 0; i < H2_PAL_CRYPTO_E2E_CASE_COUNT; ++i) {
    const h2_pal_crypto_e2e_case_t *c = &result.cases[i];
    if (c->id)
      printf("H2_CRYPTO_CASE {\"id\":\"%s\",\"status\":\"%s\",\"rc\":%d}\n",
             c->id, names[c->status], c->result);
  }
  printf("H2_CRYPTO_REPORT "
         "{\"platform\":\"wasm-chromium\",\"contract\":1,\"operations\":15,"
         "\"passed\":%zu,\"failed\":%zu,\"blocked\":%zu,\"not_run\":%zu,"
         "\"complete\":%d,\"qualified\":%d,\"rc\":%d,\"teardown\":%d}\n",
         result.passed, result.failed, result.blocked, result.not_run,
         result.complete, result.qualified, rc, teardown);
  return rc || teardown || !result.qualified ? 1 : 0;
}
