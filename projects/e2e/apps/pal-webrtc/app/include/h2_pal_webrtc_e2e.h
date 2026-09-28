#ifndef H2_PAL_WEBRTC_E2E_H
#define H2_PAL_WEBRTC_E2E_H
#include "h2_runtime.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef enum h2_pal_webrtc_e2e_case {
#define H2_PAL_WEBRTC_CASE(symbol, id) H2_PAL_WEBRTC_E2E_##symbol,
#include "h2_pal_webrtc_cases.inc"
#undef H2_PAL_WEBRTC_CASE
  H2_PAL_WEBRTC_E2E_CASE_COUNT
} h2_pal_webrtc_e2e_case_t;
typedef struct h2_pal_webrtc_e2e_case_result {
  const char *id;
  int passed, blocked, detail;
  unsigned line;
  uint64_t elapsed_ms;
} h2_pal_webrtc_e2e_case_result_t;
typedef struct h2_pal_webrtc_e2e_result {
  unsigned passed, failed, blocked;
  size_t retained_allocations;
  h2_pal_webrtc_e2e_case_result_t cases[H2_PAL_WEBRTC_E2E_CASE_COUNT];
} h2_pal_webrtc_e2e_result_t;
typedef struct h2_pal_webrtc_e2e_config {
  /** Borrowed real WebRTC, Memory and Time providers. */
  const h2_runtime_t *runtime;
  /** Borrowed STUN URL owned by the launcher's isolated Pion fixture. */
  const char *stun_url;
  /** Synchronously exchange one complete offer with real Pion. Copies answer
   * into caller storage; no SDP or credential is included in the ledger. */
  int (*exchange_offer)(void *user, h2_pal_webrtc_str_t offer, char *answer,
                        size_t capacity, size_t *out_len);
  int (*close_remote)(void *user);
  void *fixture_user;
  /** Optional platform event maintenance; never substitutes a provider. */
  void (*pump)(void *user);
  void *pump_user;
  void (*report)(void *user, const h2_pal_webrtc_e2e_case_result_t *result);
  void *report_user;
} h2_pal_webrtc_e2e_config_t;
/** Complete conformance gate. Missing operations block every case. Owns and
 * closes peers/channels, releases events and unsets borrowed media tracks.
 * All input dependencies remain borrowed through return. */
int h2_pal_webrtc_e2e_run(const h2_pal_webrtc_e2e_config_t *config,
                          h2_pal_webrtc_e2e_result_t *out_result);
#ifdef __cplusplus
}
#endif
#endif
