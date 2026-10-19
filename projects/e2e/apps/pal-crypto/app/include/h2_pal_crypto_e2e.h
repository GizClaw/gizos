#ifndef H2_PAL_CRYPTO_E2E_H
#define H2_PAL_CRYPTO_E2E_H
#include "h2_runtime.h"
#ifdef __cplusplus
extern "C" {
#endif
#define H2_PAL_CRYPTO_E2E_VERSION 1u
typedef enum h2_pal_crypto_e2e_status {
  H2_PAL_CRYPTO_E2E_NOT_RUN = 0,
  H2_PAL_CRYPTO_E2E_PASS,
  H2_PAL_CRYPTO_E2E_FAIL,
  H2_PAL_CRYPTO_E2E_BLOCKED,
} h2_pal_crypto_e2e_status_t;
enum {
  H2_PAL_CRYPTO_E2E_CASE_COUNT = 0
#define H2_PAL_CRYPTO_CASE(id, function) +1
#include "h2_pal_crypto_cases.inc"
#undef H2_PAL_CRYPTO_CASE
};
typedef struct h2_pal_crypto_e2e_case {
  const char *id;
  h2_pal_crypto_e2e_status_t status;
  h2_pal_result_t result;
} h2_pal_crypto_e2e_case_t;
typedef struct h2_pal_crypto_e2e_result {
  h2_pal_crypto_e2e_case_t cases[H2_PAL_CRYPTO_E2E_CASE_COUNT];
  size_t passed, failed, blocked, not_run;
  int complete, qualified;
} h2_pal_crypto_e2e_result_t;
/** Borrow the caller's initialized Runtime and real Crypto provider for this
 * synchronous, finite run. This App never seeds/replaces provider entropy.
 * Each case owns its buffers; no private-key bytes are logged or returned.
 * Qualification is functional conformance, not an entropy/security certificate.
 * The launcher supplies an external watchdog for stuck provider operations. */
h2_pal_result_t h2_pal_crypto_e2e_run(h2_runtime_t *runtime,
                                      h2_pal_crypto_e2e_result_t *result);
#ifdef __cplusplus
}
#endif
#endif
