#include "h2_desktop_platform.h"
#include "h2_webrtc_compat_factory.h"
#include "h2_webrtc_performance.h"
#include "h2_webrtc_pion_fixture.h"

#include <stdio.h>
#include <string.h>

static int exchange_offer(void *user, h2_pal_webrtc_str_t offer, char *answer,
                          size_t answer_capacity, size_t *answer_len) {
  return h2_webrtc_pion_fixture_exchange_performance(
      user, offer, answer, answer_capacity, answer_len);
}

int main(int argc, char **argv) {
  const char *profile = "smoke";
  unsigned requested_runs = 0u;
  size_t requested_transfer_bytes = 0u;
  if (argc < 2) {
    fprintf(stderr,
            "usage: %s SERVER [--profile=smoke|benchmark] [--runs=1|10] "
            "[--transfer-bytes=10485760]\n",
            argv[0]);
    return 2;
  }
  for (int index = 2; index < argc; ++index) {
    int consumed = 0;
    if (strncmp(argv[index], "--profile=", 10u) == 0) {
      profile = argv[index] + 10u;
    } else if (sscanf(argv[index], "--runs=%u%n", &requested_runs,
                      &consumed) == 1 &&
               argv[index][consumed] == '\0') {
      continue;
    } else if (sscanf(argv[index], "--transfer-bytes=%zu%n",
                      &requested_transfer_bytes, &consumed) == 1 &&
               argv[index][consumed] == '\0') {
      continue;
    } else {
      fprintf(stderr, "unknown argument: %s\n", argv[index]);
      return 2;
    }
  }
  const unsigned profile_runs = strcmp(profile, "smoke") == 0
                                    ? 1u
                                    : strcmp(profile, "benchmark") == 0 ? 10u
                                                                          : 0u;
  if (profile_runs == 0u ||
      (requested_runs != 0u && requested_runs != profile_runs) ||
      (requested_transfer_bytes != 0u &&
       requested_transfer_bytes != 10u * 1024u * 1024u)) {
    fprintf(stderr, "profile/runs/transfer-bytes combination is unsupported\n");
    return 2;
  }
  int result = 1;
  h2_webrtc_pion_fixture_t fixture = {0};
  h2_webrtc_compat_backend_t backend = {0};
  if (h2_webrtc_pion_fixture_start(&fixture, argv[1], "udp") != 0 ||
      h2_webrtc_compat_backend_create(&backend) != H2_PAL_OK) {
    fprintf(stderr, "H2_WEBRTC_PERF desktop setup failed\n");
    goto cleanup;
  }
  char stun_url[96];
  const int stun_url_len = snprintf(stun_url, sizeof(stun_url),
                                    "stun:127.0.0.1:%d", fixture.stun_port);
  if (stun_url_len <= 0 || (size_t)stun_url_len >= sizeof(stun_url)) {
    goto cleanup;
  }
  h2_runtime_t runtime = {
      .time = h2_desktop_platform_time_api(),
      .webrtc = backend.api,
  };
  const h2_webrtc_performance_config_t config = {
      .profile = profile,
      .target = "desktop",
      .provider = backend.name,
      .stun_url = {.data = stun_url, .len = (size_t)stun_url_len},
      .exchange_offer = exchange_offer,
      .exchange_offer_user = &fixture,
  };
  h2_webrtc_performance_result_t performance = {0};
  result = h2_webrtc_performance_run(&runtime, &config, &performance);
  if (result == 0) {
    h2_webrtc_ice_pair_t pair = {0};
    if (h2_webrtc_pion_fixture_ice_pair(&fixture, &pair) != 0 ||
        strcmp(pair.local_protocol, "udp") != 0 ||
        strcmp(pair.remote_protocol, "udp") != 0) {
      fprintf(stderr, "H2_WEBRTC_PERF selected ICE pair is not UDP\n");
      result = 1;
    }
  }

cleanup:
  if (fixture.pid > 0) {
    (void)h2_webrtc_pion_fixture_close_session(&fixture);
  }
  if (backend.destroy != NULL) {
    backend.destroy(backend.state);
  }
  h2_webrtc_pion_fixture_stop(&fixture);
  return result;
}
