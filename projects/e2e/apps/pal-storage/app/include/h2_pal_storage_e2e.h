#ifndef H2_PAL_STORAGE_E2E_H
#define H2_PAL_STORAGE_E2E_H
#include "h2_runtime.h"
#ifdef __cplusplus
extern "C" {
#endif
#define H2_PAL_STORAGE_CONTRACT_VERSION 2u
/* The caller exclusively owns all paths/namespaces and keeps Runtime/provider
 * instances alive until all calls and retained cleanup have finished. */
typedef enum h2_pal_storage_phase {
  /* Run in separate provider/process lifetimes, in this order. */
  H2_PAL_STORAGE_SEED = 1,
  H2_PAL_STORAGE_VERIFY = 2,
  /* Read-only verification after VERIFY committed remove/clear. Safe to
   * repeat in another fresh process without reseeding or mutating data. */
  H2_PAL_STORAGE_CLEAN_VERIFY = 3,
} h2_pal_storage_phase_t;
typedef enum h2_pal_storage_status {
  H2_PAL_STORAGE_NOT_RUN = 0,
  H2_PAL_STORAGE_PASS,
  H2_PAL_STORAGE_FAIL,
  H2_PAL_STORAGE_BLOCKED,
} h2_pal_storage_status_t;
typedef struct h2_pal_storage_config {
  const char *root;
  const char *namespace_a;
  const char *namespace_b;
  uint32_t nonce;
  h2_pal_storage_phase_t phase;
  void (*case_result)(void *user, const char *id,
                      h2_pal_storage_status_t status, h2_pal_result_t result);
  void *user;
} h2_pal_storage_config_t;
typedef struct h2_pal_storage_result {
  size_t passed, failed, blocked;
  h2_pal_result_t cleanup_result;
  /* Non-NULL marks uncertain ownership after failed close. The isolated
   * runner must stop without reusing handles or destroying borrowed providers.
   */
  void *retained_cleanup;
} h2_pal_storage_result_t;
h2_pal_result_t h2_pal_storage_e2e_run(h2_runtime_t *runtime,
                                       const h2_pal_storage_config_t *config,
                                       h2_pal_storage_result_t *result);
#ifdef __cplusplus
}
#endif
#endif
