#include "h2_desktop_platform.h"
#include "h2_pal_webrtc_e2e.h"
#include "h2_webrtc_compat_factory.h"
#include "h2_webrtc_pion_fixture.h"
#include <stdio.h>
static int exchange(void *user, h2_pal_webrtc_str_t offer, char *answer,
                    size_t capacity, size_t *len) {
  return h2_webrtc_pion_fixture_exchange_performance(user, offer, answer,
                                                     capacity, len);
}
static int close_remote(void *user) {
  return h2_webrtc_pion_fixture_close_session(user);
}
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
  fflush(stdout);
}
int main(int argc, char **argv) {
  if (argc != 2)
    return 2;
  h2_webrtc_pion_fixture_t fixture = {0};
  h2_webrtc_compat_backend_t backend = {0};
  int rc = 1;
  if (h2_webrtc_pion_fixture_start(&fixture, argv[1], "udp") != 0 ||
      h2_webrtc_compat_backend_create(&backend) != H2_PAL_OK)
    goto cleanup;
  char stun[96];
  (void)snprintf(stun, sizeof(stun), "stun:127.0.0.1:%d", fixture.stun_port);
  h2_runtime_t runtime = {.webrtc = backend.api,
                          .mem = h2_desktop_platform_default_allocator(),
                          .time = h2_desktop_platform_time_api()};
  const h2_pal_webrtc_e2e_config_t config = {.runtime = &runtime,
                                             .stun_url = stun,
                                             .exchange_offer = exchange,
                                             .close_remote = close_remote,
                                             .fixture_user = &fixture,
                                             .report = report};
  h2_pal_webrtc_e2e_result_t result;
  rc = h2_pal_webrtc_e2e_run(&config, &result);
  printf("H2_PAL_WEBRTC_SUMMARY "
         "{\"passed\":%u,\"failed\":%u,\"blocked\":%u,\"retained_allocations\":"
         "%zu}\n",
         result.passed, result.failed, result.blocked,
         result.retained_allocations);
cleanup:
  if (fixture.pid > 0)
    (void)h2_webrtc_pion_fixture_close_session(&fixture);
  if (backend.destroy)
    backend.destroy(backend.state);
  h2_webrtc_pion_fixture_stop(&fixture);
  return rc == H2_PAL_OK ? 0 : 1;
}
