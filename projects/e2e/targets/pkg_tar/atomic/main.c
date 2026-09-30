#include "h2_atomic_e2e.h"
#include "h2_web_main_thread.h"
#include "h2_web_platform.h"
#include <emscripten.h>
#include <inttypes.h>
#include <stdio.h>
#include <pthread.h>

/* clang-format off */
EM_JS(void, atomic_result,
      (void *context, h2_web_main_result_t *result, h2_web_main_completion_t *completion), {
  h2WebMain(context, result, completion, ["i32"], null,
    (ok) => {
  if (typeof document !== 'undefined') {
    const element = document.getElementById('result');
    element.textContent = ok ? 'PASS (pthread Workers; concurrent)' : 'FAIL';
    element.dataset.terminal = ok ? 'pass' : 'fail';
  }
});
});
/* clang-format on */

static int monotonic_us(void *user, uint64_t *out) {
  (void)user;
  *out = (uint64_t)(emscripten_get_now() * 1000.0);
  return 0;
}
static int sleep_ms(void *user, uint32_t ms) {
  (void)user;
  h2_web_worker_sleep(ms);
  return 0;
}
static const h2_pal_time_api_t time_api = {
    NULL, &(const h2_pal_time_vtable_t){.get_monotonic_us = monotonic_us,
                                         .sleep_ms = sleep_ms}};
static void pump(void *user) {
  (void)h2_web_platform_pump(user, 16u, NULL);
}

EM_JS(int, worker_valid, (), {
  return typeof document === 'undefined' && HEAPU8.buffer instanceof SharedArrayBuffer ? 1 : 0;
});
static int current_worker(void *unused) {
  (void)unused;
  return worker_valid() ? (int)(uintptr_t)pthread_self() : -1;
}
int main(void) {
  const h2_web_platform_config_t config = {.display_width = 1,
                                            .display_height = 1};
  h2_web_platform_t *platform = h2_web_platform_create(&config);
  if (platform == NULL) {
    (void)h2_web_main_call(atomic_result, (const void *[]){&(int){0}});
    return 1;
  }
  const h2_atomic_e2e_backend_t *backends[] = {
      h2_atomic_e2e_h2_backend(), h2_atomic_e2e_c11_backend()};
  const h2_atomic_qualification_config_t qualification_config = {
      .mem = h2_web_platform_mem_api(), .task = h2_web_platform_task_api(platform),
      .time = &time_api, .pump = pump, .pump_user = platform,
      .current_core = current_worker, .expected_core = {-1,-1},
      .require_distinct_workers = true};
  h2_atomic_qualification_result_t qualification;
  int qualification_rc = h2_atomic_e2e_qualify(&qualification_config, &qualification);
  h2_atomic_e2e_print("wasm", "shared-memory", &qualification);
  int passed = !qualification_rc && qualification.worker_core[0] != -1 && qualification.worker_core[1] != -1;
  printf("ATOMIC_WEB_WORKERS shared_memory=1 identities=%d,%d verdict=%s\n",
      qualification.worker_core[0], qualification.worker_core[1], passed ? "PASS" : "FAIL");
  for (unsigned i = 0; i < 2u; ++i) {
    h2_atomic_e2e_result_t result;
    const int rc = h2_atomic_e2e_run(
        h2_web_platform_mem_api(), h2_web_platform_task_api(platform),
        &time_api, backends[i], 10000u, true, false,
        NULL, NULL, pump, platform, &result);
    printf("ATOMIC_E2E backend=%s concurrent=%s expected=%u incremented=%u "
           "compared=%u elapsed_us=%" PRIu64 " rc=%d\n",
           backends[i]->name, rc ? "FAIL" : "PASS", result.expected, result.incremented,
           result.compared, result.elapsed_us, rc);
    if (rc != 0) passed = 0;
  }
  if (!qualification.teardown) h2_web_platform_destroy(platform);
  (void)h2_web_main_call(atomic_result, (const void *[]){&(int){passed}});
  return passed ? 0 : 1;
}
