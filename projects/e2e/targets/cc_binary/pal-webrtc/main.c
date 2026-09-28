#include "h2_desktop_platform.h"
#include "h2_pal_webrtc_e2e.h"
#include "h2_webrtc_compat_factory.h"
#include "h2_webrtc_pion_fixture.h"
#include <stdio.h>
#include <string.h>
static const char *test_mode;
static unsigned exchange_count;
static int exchange(void *user, h2_pal_webrtc_str_t offer,
                    const h2_pal_webrtc_channel_config_t *negotiated, char *answer,
                    size_t capacity, size_t *len) {
  if (negotiated != NULL)
    return h2_webrtc_pion_fixture_exchange_negotiated(user, offer, negotiated,
                                                      answer, capacity, len);
  int rc = h2_webrtc_pion_fixture_exchange_performance(user, offer, answer,
                                                       capacity, len);
  if (rc == H2_PAL_OK && ++exchange_count == 2u && test_mode) {
    if (!strcmp(test_mode, "reject-malformed-auth")) {
      char *fingerprint = strstr(answer, "a=fingerprint:sha-256 ");
      if (!fingerprint) return H2_PAL_ERR_FORMAT;
      fingerprint[strlen("a=fingerprint:sha-256 ") + 1u] = 'Z';
    } else if (!strcmp(test_mode, "reject-timeout-auth")) {
      /* A valid SDP whose peer is gone cannot prove certificate rejection. */
      if (h2_webrtc_pion_fixture_close_session(user) != 0)
        return H2_PAL_ERR_IO;
    }
  }
  return rc;
}
static int close_remote(void *user) {
  return h2_webrtc_pion_fixture_close_session(user);
}
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
  fflush(stdout);
}
int main(int argc, char **argv) {
  if (argc != 2 && argc != 3)
    return 2;
  test_mode = argc == 3 ? argv[2] : NULL;
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
                                             .connection_timeout_ms = test_mode ? 1500u : 20000u,
                                             .soak_duration_ms = test_mode && !strcmp(test_mode, "soak-short") ? 2100u : 0u,
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
  if (test_mode && !strncmp(test_mode, "reject-", 7u)) {
    const h2_pal_webrtc_e2e_case_result_t *auth =
        &result.cases[H2_PAL_WEBRTC_E2E_FINGERPRINT_REJECTED];
    rc = rc != H2_PAL_OK && !auth->passed && !auth->blocked &&
             auth->detail != H2_PAL_OK && auth->observed_error != H2_PAL_ERR_TLS_VERIFY &&
             auth->authentication_evidence == 0 && result.retained_allocations == 0u
             ? H2_PAL_OK : H2_PAL_ERR_INVALID_STATE;
  } else if (test_mode && !strcmp(test_mode, "soak-short")) {
    if (!result.soak.completed || result.soak.elapsed_ms < 2100u ||
        result.soak.data_roundtrips < 3u || result.soak.opus_roundtrips != result.soak.data_roundtrips)
      rc = H2_PAL_ERR_INVALID_STATE;
    printf("H2_PAL_WEBRTC_SOAK elapsed_ms=%llu data=%u opus=%u completed=%d\n",
           (unsigned long long)result.soak.elapsed_ms, result.soak.data_roundtrips,
           result.soak.opus_roundtrips, result.soak.completed);
  }
cleanup:
  if (fixture.pid > 0)
    (void)h2_webrtc_pion_fixture_close_session(&fixture);
  if (backend.destroy)
    backend.destroy(backend.state);
  h2_webrtc_pion_fixture_stop(&fixture);
  return rc == H2_PAL_OK ? 0 : 1;
}
