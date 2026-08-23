#ifndef H2_WEBRTC_PERFORMANCE_SCENARIO_H
#define H2_WEBRTC_PERFORMANCE_SCENARIO_H

#include "h2_runtime.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int (*h2_webrtc_performance_exchange_offer_fn)(
    void *user, h2_pal_webrtc_str_t offer, char *answer, size_t answer_capacity,
    size_t *answer_len);

typedef void (*h2_webrtc_performance_checkpoint_fn)(void *user,
                                                    const char *name);

typedef struct h2_webrtc_performance_config {
  const char *profile;
  const char *target;
  const char *provider;
  h2_pal_webrtc_str_t stun_url;
  h2_webrtc_performance_exchange_offer_fn exchange_offer;
  void *exchange_offer_user;
  h2_webrtc_performance_checkpoint_fn checkpoint;
  void *checkpoint_user;
} h2_webrtc_performance_config_t;

typedef struct h2_webrtc_performance_result {
  unsigned iterations;
  uint64_t median_upload_bytes_per_second;
  uint64_t median_download_bytes_per_second;
  uint64_t median_loaded_bytes_per_second;
  uint64_t median_request_batch_ns;
  uint64_t median_audio_p99_gap_ns;
  uint64_t median_audio_max_gap_ns;
} h2_webrtc_performance_result_t;

int h2_webrtc_performance_run(
    h2_runtime_t *runtime, const h2_webrtc_performance_config_t *config,
    h2_webrtc_performance_result_t *out_result);

#ifdef __cplusplus
}
#endif

#endif
