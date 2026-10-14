#include "h2_pal_storage_e2e.h"
#include "h2_web_application_host.h"
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
static reporter_t r;
static h2_pal_storage_result_t result;
static h2_pal_result_t run(h2_application_host_t *host, h2_runtime_t *runtime, void *user) {
  (void)user;
  const h2_pal_storage_config_t tests = {.root = "/storage/run",
                                         .namespace_a = "h2storea",
                                         .namespace_b = "h2storeb",
                                         .nonce = r.nonce,
                                         .phase =
                                             (h2_pal_storage_phase_t)r.phase,
                                         .case_result = report,
                                         .user = &r};
  int rc = h2_pal_storage_e2e_run(runtime, &tests, &result);
  if (result.retained_cleanup) h2_application_host_retain(host, result.retained_cleanup, H2_PAL_ERR_BUSY);
  return rc;
}
int main(int argc, char **argv) {
  if (argc != 3 || emscripten_is_main_runtime_thread()) return 2;
  r = (reporter_t){(unsigned)strtoul(argv[1], NULL, 10), (uint32_t)strtoul(argv[2], NULL, 10)};
  const h2_web_application_host_config_t config = {
      .name = "pal-storage", .display_width = 1, .display_height = 1, .persistent_root = "/storage"};
  const h2_application_host_config_t app = {.entry = run};
  h2_application_host_result_t lifecycle = {0};
  int rc = h2_web_application_host_run_app(&config, &app, &lifecycle);
  if (lifecycle.retained) return 6;
  /* Successful Host cleanup includes both IndexedDB flush and platform destroy. */
  int close_rc = lifecycle.cleanup, destroy_rc = lifecycle.cleanup;
  printf("H2_STORAGE_PHASE "
         "{\"contract\":%u,\"phase\":%u,\"nonce\":%u,\"passed\":%zu,\"failed\":%zu,\"blocked\":"
         "%zu,\"cleanup\":%d,\"fs_close\":%d,\"platform_destroy\":%d}\n",
         H2_PAL_STORAGE_CONTRACT_VERSION, r.phase, r.nonce, result.passed, result.failed, result.blocked,
         result.cleanup_result, close_rc, destroy_rc);
  return rc || close_rc || destroy_rc ? 1 : 0;
}
