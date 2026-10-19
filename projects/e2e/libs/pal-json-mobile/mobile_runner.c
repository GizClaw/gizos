#include "mobile_runner.h"
#include "h2_pal_json_e2e.h"
#include "h2_yyjson_json.h"
#include "probe.h"
#include <stdio.h>
#include <unistd.h>
static h2_pal_result_t default_create(const h2_pal_mem_api_t *mem,
                                     void **out_provider,
                                     const h2_pal_json_api_t **out_api) {
  if (!out_provider || !out_api) return H2_PAL_ERR_INVALID_ARG;
  *out_provider = NULL;
  *out_api = NULL;
  h2_yyjson_json_t *provider = NULL;
  h2_pal_result_t rc = h2_yyjson_json_create(mem, &provider);
  if (rc) return rc;
  *out_provider = provider;
  *out_api = h2_yyjson_json_api(provider);
  return H2_PAL_OK;
}
static h2_pal_result_t default_destroy(void **provider) {
  if (!provider) return H2_PAL_ERR_INVALID_ARG;
  h2_yyjson_json_t *typed = *provider;
  h2_pal_result_t rc = h2_yyjson_json_destroy(&typed);
  if (!rc) *provider = NULL;
  return rc;
}
int h2_json_mobile_run(h2_runtime_config_t config, const char *platform,
                         const char *version, const char *path,
                         const h2_json_provider_factory_t *factory,
                         int (*shutdown)(void)) {
  static const h2_json_provider_factory_t fallback = {
      .create = default_create, .destroy = default_destroy};
  if (!factory) factory = &fallback;
  if (!factory->create || !factory->destroy) return H2_PAL_ERR_INVALID_ARG;
  h2_runtime_t *runtime = NULL;
  h2_pal_json_e2e_result_t result = {0};
  void *provider = NULL;
  const h2_pal_json_api_t *api = NULL;
  int rc = factory->create(config.mem, &provider, &api);
  if (!rc) rc = h2_runtime_init(&config, &runtime);
  if (!rc)
    rc = h2_pal_json_e2e_run(runtime, api, h2_json_yyjson_probe, &result);
  if (runtime)
    h2_runtime_deinit(runtime);
  int teardown = factory->destroy(&provider);
  int core_shutdown = shutdown();
  if (!teardown) teardown = core_shutdown;
  FILE *f = fopen(path, "w");
  if (!f)
    return H2_PAL_ERR_IO;
  fprintf(f,
          "{\"platform\":\"%s\",\"version\":\"%s\",\"pid\":%ld,\"contract\":1,"
          "\"operations\":24,\"passed\":%zu,\"failed\":%zu,\"blocked\":%zu,"
          "\"not_run\":%zu,\"complete\":%d,\"qualified\":%d,\"rc\":%d,"
          "\"teardown\":%d,\"cases\":[",
          platform, version, (long)getpid(), result.passed, result.failed,
          result.blocked, result.not_run, result.complete, result.qualified, rc,
          teardown);
  const char *names[] = {"NOT_RUN", "PASS", "FAIL", "BLOCKED"};
  for (size_t i = 0; i < H2_PAL_JSON_E2E_CASE_COUNT; ++i) {
    const h2_pal_json_e2e_case_t *c = &result.cases[i];
    fprintf(f, "%s{\"id\":\"%s\",\"status\":\"%s\",\"rc\":%d}", i ? "," : "",
            c->id ? c->id : "", names[c->status], c->result);
  }
  fputs("]}\n", f);
  if (fclose(f))
    return H2_PAL_ERR_IO;
  return rc || teardown || !result.qualified ? H2_PAL_ERR_INVALID_STATE
                                             : H2_PAL_OK;
}
