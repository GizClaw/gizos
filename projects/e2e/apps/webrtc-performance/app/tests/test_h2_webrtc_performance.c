#include "h2_webrtc_performance.h"

#include <stdio.h>

static int exchange_offer(void *user, h2_pal_webrtc_str_t offer, char *answer,
                          size_t answer_capacity, size_t *answer_len) {
  (void)user;
  (void)offer;
  (void)answer;
  (void)answer_capacity;
  (void)answer_len;
  return -1;
}

int main(void) {
  h2_runtime_t runtime = {0};
  const h2_webrtc_performance_config_t config = {
      .profile = "smoke",
      .exchange_offer = exchange_offer,
  };
  h2_webrtc_performance_result_t result = {0};
  if (h2_webrtc_performance_run(NULL, &config, &result) != 2 ||
      h2_webrtc_performance_run(&runtime, &config, &result) != 2) {
    fprintf(stderr, "invalid Runtime dependencies were accepted\n");
    return 1;
  }
  runtime.time = (const h2_pal_time_api_t *)1;
  runtime.webrtc = (const h2_pal_webrtc_api_t *)1;
  h2_webrtc_performance_config_t invalid_profile = config;
  invalid_profile.profile = "invalid";
  if (h2_webrtc_performance_run(&runtime, &invalid_profile, &result) != 2) {
    fprintf(stderr, "invalid profile was accepted\n");
    return 1;
  }
  return 0;
}
