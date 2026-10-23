#ifndef H2_LINGXI_E2E_H
#define H2_LINGXI_E2E_H

#include "h2_lingxi.h"
#include "h2/pal/os/h2_pal_time.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Fixed ordered registry shared by host and device launchers. */
typedef enum h2_lingxi_e2e_case {
#define H2_LINGXI_CASE(symbol, id) H2_LINGXI_E2E_##symbol,
#include "h2_lingxi_cases.inc"
#undef H2_LINGXI_CASE
  H2_LINGXI_E2E_CASE_COUNT
} h2_lingxi_e2e_case_t;

typedef enum h2_lingxi_e2e_status {
  H2_LINGXI_E2E_BLOCKED = 0,
  H2_LINGXI_E2E_PASS,
  H2_LINGXI_E2E_FAIL,
} h2_lingxi_e2e_status_t;

/** Non-secret case observation; contains no identity or credential bytes. */
typedef struct h2_lingxi_e2e_case_result {
  /** Static registry ID, valid for the program lifetime. */
  const char* id;
  h2_lingxi_e2e_status_t status;
  h2_pal_result_t result;
  /** HTTP/business observation for request cases; zero for non-request cases. */
  h2_lingxi_result_t platform;
  uint64_t elapsed_ms;
} h2_lingxi_e2e_case_result_t;

typedef struct h2_lingxi_e2e_result {
  unsigned passed;
  unsigned failed;
  unsigned blocked;
  h2_lingxi_e2e_case_result_t cases[H2_LINGXI_E2E_CASE_COUNT];
} h2_lingxi_e2e_result_t;

/**
 * Borrowed configuration, valid until the synchronous run returns. The owner
 * supplies Runtime/PAL views and serializes access to the JSON provider.
 * Endpoint, credentials and metadata are injected; this App does not read an
 * environment, file, board or concrete provider. Each HTTP case uses the
 * client's finite timeout without retries. All device credentials obtained
 * during the run belong to the App and are wiped before return.
 */
typedef struct h2_lingxi_e2e_config {
  h2_lingxi_config_t client;
  const h2_pal_time_api_t* time;
  h2_lingxi_product_t product;
  h2_lingxi_identity_t identity;
  h2_lingxi_heartbeat_t heartbeat;
  /** Optional synchronous progress callback. The row is borrowed; callbacks
   * must not mutate config/providers or reenter the runner. */
  void (*report)(void* user, const h2_lingxi_e2e_case_result_t* row);
  void* report_user;
} h2_lingxi_e2e_config_t;

/**
 * @brief Execute the complete device-information / heartbeat / cleanup flow.
 * @param config Required borrowed dependencies and caller test inputs.
 * @param out_result Required caller-owned ledger, initialized on every call.
 * @return OK only when every case passes. The first failing PAL result is
 * preserved. Failed identity acquisition blocks the dependent heartbeat;
 * credential cleanup still runs, and each case receives a terminal result.
 * Missing required App inputs block every case without network activity;
 * remaining client input validation is part of the information case. Provider
 * lifecycle, outer teardown and result output belong to the launcher.
 */
h2_pal_result_t h2_lingxi_e2e_run(const h2_lingxi_e2e_config_t* config,
                                  h2_lingxi_e2e_result_t* out_result);

#ifdef __cplusplus
}
#endif
#endif
