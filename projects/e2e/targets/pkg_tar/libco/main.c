#include "h2_libco_smoke.h"
#include "h2_smoke_host_runtime.h"
#include "h2_web_platform.h"

#include <emscripten.h>
#include <stdio.h>

EM_JS(void, h2_web_libco_result,
      (int result, uint32_t switches, uint64_t elapsed_ms), {
  const element = globalThis.document && document.getElementById('result');
  if (element) {
    element.textContent = result === 0
      ? `PASS switches=${switches} elapsed_ms=${elapsed_ms}`
      : `FAIL rc=${result} switches=${switches} elapsed_ms=${elapsed_ms}`;
    element.dataset.terminal = result === 0 ? 'pass' : 'fail';
  }
});

static h2_pal_result_t h2_web_root_monotonic(void *user,
                                              uint64_t *out_ms) {
  (void)user;
  if (out_ms == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  *out_ms = (uint64_t)emscripten_get_now();
  return H2_PAL_OK;
}

static h2_pal_result_t h2_web_root_monotonic_us(void *user,
                                                uint64_t *out_us) {
  (void)user;
  if (out_us == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  *out_us = (uint64_t)(emscripten_get_now() * 1000.0);
  return H2_PAL_OK;
}

static h2_pal_result_t h2_web_root_sleep(void *user, uint32_t milliseconds) {
  (void)user;
  emscripten_sleep(milliseconds);
  return H2_PAL_OK;
}

static const h2_pal_time_api_t *h2_web_root_time_api(void) {
  static const h2_pal_time_vtable_t vtable = {
      .get_monotonic_ms = h2_web_root_monotonic,
      .get_monotonic_us = h2_web_root_monotonic_us,
      .sleep_ms = h2_web_root_sleep,
  };
  static const h2_pal_time_api_t api = {
      .user = NULL,
      .vtable = &vtable,
  };
  return &api;
}

int main(void) {
  const h2_web_platform_config_t platform_config = {
      .display_width = 1,
      .display_height = 1,
  };
  h2_web_platform_t *platform = h2_web_platform_create(&platform_config);
  if (platform == NULL) {
    h2_web_libco_result(H2_PAL_ERR_NO_MEMORY, 0u, 0u);
    return 1;
  }
  h2_runtime_config_t config = h2_smoke_host_runtime_config(
      "browser", "webassembly", "wasm32", h2_web_platform_mem_api(),
      h2_web_root_time_api(), h2_web_platform_queue_api(platform),
      h2_pal_unsupported_display_api());
  config.log = h2_web_platform_log_api();
  h2_runtime_t *runtime = NULL;
  h2_pal_result_t result = h2_runtime_init(&config, &runtime);
  const uint64_t started_ms = (uint64_t)emscripten_get_now();
  if (result == H2_PAL_OK) {
    const h2_libco_smoke_config_t smoke = {
        .task_stack_size = H2_LIBCO_SMOKE_DEFAULT_STACK_SIZE,
        .switch_iterations = H2_LIBCO_SMOKE_DEFAULT_SWITCH_ITERATIONS,
    };
    result = (h2_pal_result_t)h2_libco_smoke_run(runtime, &smoke);
  }
  if (runtime != NULL) {
    h2_runtime_deinit(runtime);
  }
  h2_web_platform_destroy(platform);
  const uint64_t elapsed_ms = (uint64_t)emscripten_get_now() - started_ms;
  h2_web_libco_result(result, H2_LIBCO_SMOKE_DEFAULT_SWITCH_ITERATIONS,
                      elapsed_ms);
  fprintf(result == H2_PAL_OK ? stdout : stderr,
          "H2_WEB_LIBCO_E2E result=%s rc=%d switches=%u elapsed_ms=%llu\n",
          result == H2_PAL_OK ? "PASS" : "FAIL", result,
          H2_LIBCO_SMOKE_DEFAULT_SWITCH_ITERATIONS,
          (unsigned long long)elapsed_ms);
  return result == H2_PAL_OK ? 0 : 1;
}
