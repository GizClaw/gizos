#ifndef H2_ALIPAY_E2E_H
#define H2_ALIPAY_E2E_H

#include "h2_alipay.h"
#include "h2_runtime.h"
#include <limits.h>

#ifdef __cplusplus
extern "C" {
#endif

#define H2_ALIPAY_E2E_NOT_CALLED INT_MIN

typedef enum h2_alipay_e2e_stage {
  H2_ALIPAY_E2E_NETWORK,
  H2_ALIPAY_E2E_PRE_INIT,
  H2_ALIPAY_E2E_REENTRY,
  H2_ALIPAY_E2E_PREREQUISITES,
  H2_ALIPAY_E2E_BOUND,
  H2_ALIPAY_E2E_GET_CODE,
  H2_ALIPAY_E2E_CODE_READY,
  H2_ALIPAY_E2E_WAIT_BINDING,
  H2_ALIPAY_E2E_BINDING_COMPLETE,
} h2_alipay_e2e_stage_t;

typedef struct h2_alipay_e2e_result {
  h2_alipay_e2e_stage_t stage;
  h2_pal_result_t result;
  h2_alipay_blocker_t blocker;
  int prepare_rc, init_rc, init_sdk_rc, reentry_rc, reentry_sdk_rc, code_sdk_rc;
  bool first_init, reused, qr_visible, code_presented;
  uint64_t init_ms, code_ms, binding_ms;
  unsigned binding_queries;
  h2_alipay_binding_result_t binding;
  size_t length;
  char code[H2_ALIPAY_BINDING_CODE_CAPACITY];
  int display_rc;
} h2_alipay_e2e_result_t;

typedef struct h2_alipay_e2e_config {
  /** Borrowed until run returns, including the SDK's boot lifetime contract. */
  const h2_alipay_api_t* alipay;
  uint32_t feature;
  /** Opt in only for a provider implementing both binding operations.
   * Existing verified credentials skip code generation. Unverified local
   * credentials are reported without resetting or rebinding the account. */
  bool complete_binding;
  /** Zero selects 60000 ms total and 2000 ms between queries.
   * Deadlines are checked between blocking SDK calls; no SDK call is
   * interrupted and its provider's own finite transport budget still applies.
   */
  uint32_t binding_timeout_ms;
  uint32_t binding_interval_ms;
  void* user;
  /** Required. Worker-only network/IP and calibrated UTC preparation.
   * A failure prevents every SDK call. Must finish before teardown. */
  h2_pal_result_t (*prepare)(h2_runtime_t* runtime, void* user);
  /** Optional completed-display evidence. Called on the UI thread after
   * draw/present; pixels are native RGB565 and borrowed only for this call. */
  h2_pal_result_t (*observe)(void* user, const uint8_t* pixels,
                             size_t stride_bytes, int width, int height);
  /** Optional UI-thread signal after the code was drawn and observed, before
   * the binding worker starts. The provider trace is quiescent here.
   * Must not invoke SDK operations or change LVGL objects. */
  void (*binding_code_ready)(h2_runtime_t* runtime, void* user,
                             const h2_alipay_e2e_result_t* result);
  /** UI-thread result callback, only after the worker joins and the final
   * page is flushed. Called again every 10 seconds with replay=true.
   * May read the quiescent SDK trace; must not call the SDK or LVGL. */
  void (*report)(h2_runtime_t* runtime, void* user,
                 const h2_alipay_e2e_result_t* result, bool replay);
  /** Optional UI-thread exit request. An in-flight worker is joined before
   * return, so this cannot cancel a blocking SDK call. NULL keeps the page. */
  bool (*should_exit)(void* user);
} h2_alipay_e2e_config_t;

/** Synchronous worker operation. Caller owns result; configuration and
 * Runtime remain borrowed through completion. Network preparation precedes
 * initialization, cached reentry and one binding-code call. Legacy readiness
 * and binding-status callbacks are not prerequisites and are not invoked.
 * With complete_binding, reads the local binding snapshot before requesting
 * a code. Does not poll until the caller has actually displayed that code.
 * Does not provision, activate, unbind or reset. */
h2_pal_result_t h2_alipay_e2e_execute(h2_runtime_t* runtime,
                                      const h2_alipay_e2e_config_t* config,
                                      h2_alipay_e2e_result_t* result);

/** Blocking worker continuation after CODE_READY was presented. Polls only
 * the same provider/session that generated that code, until durable completion,
 * failure, timeout or cancellation. cancel is borrowed and worker-safe; NULL
 * disables cancellation. No UI callback or rendering occurs here.
 * Cancellation stops new queries and never resets/deletes binding material. */
h2_pal_result_t h2_alipay_e2e_complete_binding(
    h2_runtime_t* runtime, const h2_alipay_e2e_config_t* config,
    bool (*cancel)(void*), void* cancel_user, h2_alipay_e2e_result_t* result);

/** Standalone single-page App. Owns LVGL/display lifetime and at most one SDK
 * worker at a time;
 * Runtime lifetime belongs to the launcher. No required component mappings.
 * All UI state stays on the calling thread. Keeps the final page visible
 * until should_exit; errors in SDK prerequisites remain visible results.
 * Returns only after worker join and UI teardown. A failed join is retried
 * while retaining every borrowed dependency. */
h2_pal_result_t h2_alipay_e2e_run(h2_runtime_t* runtime,
                                  const h2_alipay_e2e_config_t* config);

#ifdef __cplusplus
}
#endif
#endif
