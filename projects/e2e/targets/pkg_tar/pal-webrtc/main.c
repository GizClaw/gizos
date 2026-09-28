#include "h2_pal_webrtc_e2e.h"
#include "h2_web_main_thread.h"
#include "h2_web_platform.h"
#include <emscripten.h>
#include <emscripten/threading.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* clang-format off */
EM_JS(void, exchange_js,
    (void *context, h2_web_main_result_t *result, h2_web_main_completion_t *completion), {
  h2WebMain(context, result, completion, ["pointer", "u32", "pointer", "u32"], "i32",
    async (offer, len, answer, capacity) => {
      const response = await fetch('/pion/offer', {method: 'POST',
        body: UTF8ToString(offer, len), signal: AbortSignal.timeout(20000)});
      if (!response.ok) return -4;
      const text = await response.text();
      const length = lengthBytesUTF8(text);
      if (length >= capacity) return -13;
      stringToUTF8(text, answer, capacity);
      return length;
    });
});
EM_JS(void, close_js,
    (void *context, h2_web_main_result_t *result, h2_web_main_completion_t *completion), {
  h2WebMain(context, result, completion, [], "i32", async () => {
    const response = await fetch('/pion/close', {method: 'POST', signal: AbortSignal.timeout(10000)});
    return response.ok ? 0 : -4;
  });
});
/* clang-format on */
static int exchange(void *user, h2_pal_webrtc_str_t offer, char *answer,
                    size_t capacity, size_t *len) {
  (void)user;
  int rc =
      h2_web_main_call(exchange_js, (const void *[]){&offer.data, &offer.len,
                                                     &answer, &capacity})
          .i32;
  *len = rc > 0 ? (size_t)rc : 0u;
  return rc > 0 ? H2_PAL_OK : rc;
}
static int close_remote(void *user) {
  (void)user;
  return h2_web_main_call(close_js, NULL).i32;
}
static void pump(void *user) { (void)h2_web_platform_pump(user, 16u, NULL); }
static void report(void *user, const h2_pal_webrtc_e2e_case_result_t *r) {
  (void)user;
  printf("H2_PAL_WEBRTC_CASE "
         "{\"id\":\"%s\",\"status\":\"%s\",\"detail\":%d,\"line\":%u,\"elapsed_"
         "ms\":%llu}\n",
         r->id,
         r->passed    ? "PASS"
         : r->blocked ? "BLOCKED"
                      : "FAIL",
         r->detail, r->line, (unsigned long long)r->elapsed_ms);
}
int main(int argc, char **argv) {
  if (argc != 2 || emscripten_is_main_runtime_thread())
    return 2;
  const h2_web_platform_config_t pc = {.display_width = 1, .display_height = 1};
  h2_web_platform_t *platform = h2_web_platform_create(&pc);
  if (!platform)
    return 2;
  h2_runtime_t runtime = {.webrtc = h2_web_platform_webrtc_api(platform),
                          .mem = h2_web_platform_mem_api(),
                          .time = h2_web_platform_time_api(platform)};
  const h2_pal_webrtc_e2e_config_t config = {.runtime = &runtime,
                                             .stun_url = argv[1],
                                             .exchange_offer = exchange,
                                             .close_remote = close_remote,
                                             .pump = pump,
                                             .pump_user = platform,
                                             .report = report};
  h2_pal_webrtc_e2e_result_t result;
  int rc = h2_pal_webrtc_e2e_run(&config, &result);
  int cleanup = h2_web_platform_destroy(platform);
  printf("H2_PAL_WEBRTC_SUMMARY "
         "{\"passed\":%u,\"failed\":%u,\"blocked\":%u,\"retained_allocations\":"
         "%zu,\"cleanup\":%d}\n",
         result.passed, result.failed, result.blocked,
         result.retained_allocations, cleanup);
  return rc == H2_PAL_OK && cleanup == H2_PAL_OK ? 0 : 1;
}
