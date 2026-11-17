#ifndef H2_ALIPAY_E2E_H
#define H2_ALIPAY_E2E_H

#include "h2_alipay.h"
#include "h2_alipay_payment.h"
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
  int prepare_rc, init_rc, init_sdk_rc, reentry_rc, reentry_sdk_rc, code_sdk_rc;
  bool first_init, reused, qr_visible, code_presented;
  uint64_t init_ms, code_ms, binding_ms;
  unsigned binding_queries;
  h2_alipay_binding_result_t binding;
  size_t length;
  char code[H2_ALIPAY_BINDING_CODE_CAPACITY];
  int display_rc;
} h2_alipay_e2e_result_t;

typedef struct h2_alipay_payment_buttons {
  h2_runtime_component_id_t previous;
  h2_runtime_component_id_t next;
  h2_runtime_component_id_t activate;
  h2_runtime_component_id_t back;
} h2_alipay_payment_buttons_t;

typedef struct h2_alipay_e2e_config {
  /** Borrowed until run returns, including the SDK's boot lifetime contract. */
  const h2_alipay_api_t* alipay;
  uint32_t feature;
  /** Opt in only for a provider implementing both binding operations.
   * Existing verified credentials skip code generation. Unverified local
   * credentials are reported without resetting or rebinding the account. */
  bool complete_binding;
  /** Zero timeout waits indefinitely; zero interval selects 2000 ms.
   * An explicit test deadline is checked between blocking SDK calls.
   * No SDK call is interrupted; finite transport budgets still apply.
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
  /** Optional worker-thread progress after each completed binding query.
   * result is borrowed for this call. Must not invoke SDK/LVGL or retain
   * result pointers. Use only non-secret status/length diagnostics. */
  void (*binding_progress)(h2_runtime_t* runtime, void* user,
                           const h2_alipay_e2e_result_t* result);
  /** UI-thread result callback, only after the worker joins and the final
   * page is flushed. Binding-only fixtures replay every 10 seconds; a payment
   * fixture reports binding once before transferring control to its menu.
   * May read the quiescent SDK trace; must not call the SDK or LVGL. */
  void (*report)(h2_runtime_t* runtime, void* user,
                 const h2_alipay_e2e_result_t* result, bool replay);
  /** Optional UI-thread exit request. An in-flight worker is joined before
   * return, so this cannot cancel a blocking SDK call. NULL keeps the page. */
  bool (*should_exit)(void* user);
  /** Optional payment menu after verified binding. NULL retains the binding
   * qualification fixture. Hardware acquisition remains Runtime-owned. */
  const h2_alipay_payment_api_t* payment;
  h2_runtime_component_id_t payment_nfc_component_id;
  /** The integration App owns an NFC wallet session. SDK BLE compatibility
   * remains in the client library; this App acquires no payment BLE radio. */
  /** Optional terminal-specific encoder. Called on the business worker.
   * Writes a complete NDEF message into caller storage only on success.
   * NULL leaves NFC writeback unavailable; SDK bytes are not assumed to
   * already be a complete NDEF message. user is the App configuration user. */
  h2_pal_result_t (*payment_nfc_encode_reply)(void* user, const uint8_t* data,
                                              size_t length, uint8_t* ndef,
                                              size_t capacity,
                                              size_t* ndef_length);
  /** Optional one-time UI-thread notification after the initial payment menu
   * has completed display verification. No Alipay SDK operation is allowed. */
  h2_pal_result_t (*payment_ready)(h2_runtime_t* runtime, void* user);
  /** Required when payment is set. Supply four distinct nonzero Runtime Button
   * component ids. Actions are handled on release through Runtime events;
   * the payment page does not open or poll Touch PAL. */
  h2_alipay_payment_buttons_t payment_buttons;
} h2_alipay_e2e_config_t;

/** Synchronous worker operation. Caller owns result; configuration and
 * Runtime remain borrowed through completion. Network preparation precedes
 * initialization, cached reentry and one binding-code call. With
 * complete_binding, reads the local binding snapshot before requesting a code.
 * Does not poll until the caller has actually displayed that code. Does not
 * provision, activate, unbind or reset. */
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

/** Worker continuation after a terminal SDK binding failure. Refreshes only
 * when a local snapshot confirms UNBOUND. Never refreshes pending sessions,
 * overwrites unverified credentials, or repeats a completed finishBind.
 * Requests one new code after the configured polling interval; the caller
 * must display it before resuming completion. No provisioning/reset/unbind. */
h2_pal_result_t h2_alipay_e2e_refresh_binding(
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
