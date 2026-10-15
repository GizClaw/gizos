#include "h2/pal/h2_pal_unsupported.h"
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
  h2WebMain(context, result, completion, ["pointer", "u32", "pointer", "u32", "i32", "u16", "pointer", "u32", "i32", "i32"], "i32",
    async (offer, len, answer, capacity, negotiated, id, label, labelLen, ordered, reliable) => {
      const headers = {'Content-Type': 'application/sdp'};
      if (negotiated) Object.assign(headers, {
        'X-H2-Negotiated-ID': String(id),
        'X-H2-Negotiated-Label': UTF8ToString(label, labelLen),
        'X-H2-Negotiated-Ordered': ordered ? '1' : '0',
        'X-H2-Negotiated-Reliable': reliable ? '1' : '0'});
      const response = await fetch('/pion/offer', {method: 'POST',
        headers, body: UTF8ToString(offer, len), signal: AbortSignal.timeout(20000)});
      if (!response.ok) return -4;
      const text = await response.text();
      const length = lengthBytesUTF8(text);
      if (length >= capacity) return -13;
      stringToUTF8(text, answer, capacity);
      return length;
    });
});
EM_JS(void, authentication_witness_js,
    (void *context, h2_web_main_result_t *result, h2_web_main_completion_t *completion), {
  h2WebMain(context, result, completion, [], "i32", async () => {
    const response = await fetch('/pion/authentication-witness', {signal: AbortSignal.timeout(5000)});
    if (!response.ok) return -4;
    const witness = await response.json();
    console.log('H2_PAL_WEBRTC_AUTH_WITNESS ' + JSON.stringify(witness));
    if (witness.source !== 'pion-dtls-typed-alert' || witness.direction !== 'received') return -4;
    if (witness.certificate_rejection && witness.channels_opened === 0 && witness.level === 2 && (witness.alert === 42 || witness.alert === 46)) return -17;
    return witness.level ? -4 : -9;
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
static int exchange(void *user, h2_pal_webrtc_str_t offer,
                    const h2_pal_webrtc_channel_config_t *negotiated,
                    char *answer, size_t capacity, size_t *len) {
  (void)user;
  const h2_pal_webrtc_channel_config_t empty = {0};
  if (negotiated == NULL)
    negotiated = &empty;
  int rc = h2_web_main_call(
               exchange_js,
               (const void *[]){&offer.data, &offer.len, &answer, &capacity,
                                &negotiated->negotiated, &negotiated->stream_id,
                                &negotiated->label.data, &negotiated->label.len,
                                &negotiated->ordered, &negotiated->reliable})
               .i32;
  *len = rc > 0 ? (size_t)rc : 0u;
  return rc > 0 ? H2_PAL_OK : rc;
}
static int authentication_witness(void *user) {
  (void)user;
  return h2_web_main_call(authentication_witness_js, NULL).i32;
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
         "ms\":%llu,\"observed_error\":%d,\"authentication_evidence\":%d}\n",
         r->id,
         r->passed    ? "PASS"
         : r->blocked ? "BLOCKED"
                      : "FAIL",
         r->detail, r->line, (unsigned long long)r->elapsed_ms,
         r->observed_error, r->authentication_evidence);
}
int main(int argc, char **argv) {
  if (argc != 4 || emscripten_is_main_runtime_thread())
    return 2;
  h2_pal_net_addr_list_t addresses;
  h2_pal_net_resolver_t *resolver = NULL;
  h2_pal_net_addr_t addr;
  const h2_pal_net_api_t *net = h2_pal_unsupported_net_api();
  int boundary =
      h2_pal_net_resolve_all(net, "::1", H2_PAL_NET_FAMILY_IPV6, &addresses) ==
          H2_PAL_ERR_UNSUPPORTED &&
      h2_pal_net_resolve_start_family(net, "::1", H2_PAL_NET_FAMILY_IPV6,
                                      &resolver) == H2_PAL_ERR_UNSUPPORTED &&
      h2_pal_net_get_host_addr_family(net, NULL, H2_PAL_NET_FAMILY_IPV6,
                                      &addr) == H2_PAL_ERR_UNSUPPORTED;
  printf("H2_PAL_IPV6_RAW_BOUNDARY "
         "{\"status\":\"SKIP\",\"raw_ipv6_qualified\":false,\"observed_"
         "unsupported\":%d}\n",
         boundary);
  const h2_web_platform_config_t pc = {.display_width = 1, .display_height = 1};
  h2_web_platform_t *platform = h2_web_platform_create(&pc);
  if (!platform)
    return 2;
  h2_runtime_t runtime = {.webrtc = h2_web_platform_webrtc_api(platform),
                          .mem = h2_web_platform_mem_api(),
                          .time = h2_web_platform_time_api(platform)};
  const h2_pal_http_api_t *http = h2_web_platform_http_api(platform);
  uint8_t body[64];
  h2_pal_http_request_t request = {.url = {argv[2], strlen(argv[2])},
                                   .method = H2_PAL_HTTP_GET,
                                   .response_buf = body,
                                   .response_buf_cap = sizeof(body),
                                   .timeout_ms = 5000};
  h2_pal_http_response_t response = {0};
  int http_rc = h2_pal_http_do(http, &request, &response);
  int http_ok = http_rc == H2_PAL_OK && response.status_code == 200 &&
                response.body_len == strlen(argv[3]) &&
                memcmp(body, argv[3], response.body_len) == 0;
  h2_pal_http_response_free(http, &response);
  printf("H2_PAL_IPV6_HTTP {\"status\":\"%s\",\"rc\":%d}\n",
         http_ok ? "PASS" : "FAIL", http_rc);
  const h2_pal_webrtc_e2e_config_t config = {.runtime = &runtime,
                                             .stun_url = argv[1],
                                             .exchange_offer = exchange,
                                             .authentication_witness =
                                                 authentication_witness,
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
  return rc == H2_PAL_OK && cleanup == H2_PAL_OK && http_ok && boundary ? 0 : 1;
}
