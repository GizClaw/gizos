#ifndef H2_ONENET_E2E_H
#define H2_ONENET_E2E_H

#include "h2_onenet.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Borrowed synchronous input; launcher owns providers, token and metadata.
 * The App owns client callbacks, bounded processing and client cleanup. */
typedef struct h2_onenet_e2e_config {
  h2_onenet_config_t client;
  const char* access_token;
  const char* params_json;
  uint32_t timeout_ms;
} h2_onenet_e2e_config_t;

/** Non-secret terminal ledger. A dependent unexecuted post is INVALID_STATE.
 * Incoming product commands receive 501: this App has no physical actuator. */
typedef struct h2_onenet_e2e_result {
  h2_pal_result_t connect_result;
  h2_pal_result_t post_result;
  h2_pal_result_t cleanup_result;
  int32_t platform_code;
  unsigned responses;
  unsigned commands;
  h2_onenet_operation_t last_command_operation;
  h2_pal_result_t command_reply_result;
} h2_onenet_e2e_result_t;

/** Run one CONNECT/SUBACK, one property post/platform response and cleanup.
 * All inputs remain borrowed until return; input and output cannot alias.
 * No environment/file access, provider assembly or reconnect is performed.
 * Returns OK only after all operations and cleanup succeeded. */
h2_pal_result_t h2_onenet_e2e_run(const h2_onenet_e2e_config_t* config,
                                  h2_onenet_e2e_result_t* out_result);

#ifdef __cplusplus
}
#endif
#endif
