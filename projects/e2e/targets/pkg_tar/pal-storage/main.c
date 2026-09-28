#include "h2_pal_storage_e2e.h"
#include "h2_smoke_host_runtime.h"
#include "h2_web_fs.h"
#include "h2_web_main_thread.h"
#include <emscripten.h>
#include <emscripten/threading.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct reporter {
  unsigned phase;
  uint32_t nonce;
} reporter_t;
static void report(void *user, const char *id, h2_pal_storage_status_t status,
                   h2_pal_result_t rc) {
  reporter_t *r = user;
  const char *names[] = {"NOT_RUN", "PASS", "FAIL", "BLOCKED"};
  printf(
      "H2_STORAGE_CASE "
      "{\"id\":\"%s\",\"status\":\"%s\",\"rc\":%d,\"phase\":%u,\"nonce\":%u}\n",
      id, names[status], rc, r->phase, r->nonce);
}
int main(int argc, char **argv) {
  if (argc != 3 || emscripten_is_main_runtime_thread())
    return 2;
  reporter_t r = {(unsigned)strtoul(argv[1], NULL, 10),
                  (uint32_t)strtoul(argv[2], NULL, 10)};
  const h2_web_platform_config_t config = {.display_width = 1,
                                           .display_height = 1};
  h2_web_platform_t *platform = h2_web_platform_create(&config);
  if (!platform)
    return 3;
  h2_web_fs_t *fs = NULL;
  const h2_web_fs_config_t fs_config = {.persistent_root = "/storage"};
  if (h2_web_fs_open(platform, &fs_config, &fs) != H2_PAL_OK)
    return 4;
  h2_runtime_config_t runtime_config = h2_smoke_host_runtime_config(
      "pal-storage", "web", "wasm32", h2_web_platform_mem_api(),
      h2_web_platform_time_api(platform), h2_web_platform_queue_api(platform),
      h2_pal_unsupported_display_api());
  runtime_config.task = h2_web_platform_task_api(platform);
  runtime_config.sync = h2_web_platform_sync_api(platform);
  runtime_config.fs = h2_web_fs_api(fs);
  runtime_config.pref = h2_web_platform_pref_api(platform);
  h2_runtime_t *runtime = NULL;
  if (h2_runtime_init(&runtime_config, &runtime) != H2_PAL_OK)
    return 5;
  const h2_pal_storage_config_t tests = {.root = "/storage/run",
                                         .namespace_a = "h2storea",
                                         .namespace_b = "h2storeb",
                                         .nonce = r.nonce,
                                         .phase =
                                             (h2_pal_storage_phase_t)r.phase,
                                         .case_result = report,
                                         .user = &r};
  h2_pal_storage_result_t result;
  int rc = h2_pal_storage_e2e_run(runtime, &tests, &result);
  if (result.retained_cleanup)
    return 6;
  h2_runtime_deinit(runtime);
  int close_rc = h2_web_fs_close(fs);
  int destroy_rc = h2_web_platform_destroy(platform);
  printf("H2_STORAGE_PHASE "
         "{\"phase\":%u,\"nonce\":%u,\"passed\":%zu,\"failed\":%zu,\"blocked\":"
         "%zu,\"cleanup\":%d,\"fs_close\":%d,\"platform_destroy\":%d}\n",
         r.phase, r.nonce, result.passed, result.failed, result.blocked,
         result.cleanup_result, close_rc, destroy_rc);
  return rc || close_rc || destroy_rc ? 1 : 0;
}
