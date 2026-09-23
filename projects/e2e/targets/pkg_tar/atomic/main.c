#include "h2_atomic_e2e.h"
#include "h2_web_platform.h"
#include <emscripten.h>
#include <inttypes.h>
#include <stdio.h>

EM_JS(void, atomic_result, (int ok), {
  if (typeof document !== 'undefined') {
    const element = document.getElementById('result');
    element.textContent = ok ? 'PASS (cooperative WASM; concurrency skipped)' : 'FAIL';
    element.dataset.terminal = ok ? 'pass' : 'fail';
  }
});

static int monotonic_us(void *user, uint64_t *out) {
  (void)user;
  *out = (uint64_t)(emscripten_get_now() * 1000.0);
  return 0;
}
static int sleep_ms(void *user, uint32_t ms) {
  (void)user;
  emscripten_sleep(ms);
  return 0;
}
static const h2_pal_time_api_t time_api = {
    NULL, &(const h2_pal_time_vtable_t){.get_monotonic_us = monotonic_us,
                                         .sleep_ms = sleep_ms}};
static void pump(void *user) {
  (void)h2_web_platform_pump(user, 16u, NULL);
}

int main(void) {
  const h2_web_platform_config_t config = {.display_width = 1,
                                            .display_height = 1};
  h2_web_platform_t *platform = h2_web_platform_create(&config);
  if (platform == NULL) {
    atomic_result(0);
    return 1;
  }
  const h2_atomic_e2e_backend_t *backends[] = {
      h2_atomic_e2e_h2_backend(), h2_atomic_e2e_c11_backend()};
  int passed = 1;
  for (unsigned i = 0; i < 2u; ++i) {
    h2_atomic_e2e_result_t result;
    const int rc = h2_atomic_e2e_run(
        h2_web_platform_mem_api(), h2_web_platform_task_api(platform),
        &time_api, backends[i], 10000u, false, pump, platform, &result);
    printf("ATOMIC_E2E backend=%s concurrent=SKIP expected=%u incremented=%u "
           "compared=%u elapsed_us=%" PRIu64 " rc=%d\n",
           backends[i]->name, result.expected, result.incremented,
           result.compared, result.elapsed_us, rc);
    if (rc != 0) passed = 0;
  }
  h2_web_platform_destroy(platform);
  atomic_result(passed);
  return passed ? 0 : 1;
}
