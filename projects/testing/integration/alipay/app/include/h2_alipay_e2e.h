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
} h2_alipay_e2e_stage_t;

typedef struct h2_alipay_e2e_result {
  h2_alipay_e2e_stage_t stage;
  h2_pal_result_t result;
  h2_alipay_blocker_t blocker;
  int prepare_rc, init_rc, init_sdk_rc, reentry_rc, reentry_sdk_rc, code_sdk_rc;
  bool first_init, reused, qr_visible;
  uint64_t init_ms, code_ms;
  size_t length;
  char code[H2_ALIPAY_BINDING_CODE_CAPACITY];
  int display_rc;
} h2_alipay_e2e_result_t;

typedef struct h2_alipay_e2e_config {
  /** Borrowed until run returns, including the SDK's boot lifetime contract. */
  const h2_alipay_api_t* alipay;
  uint32_t feature;
  void* user;
  /** Required. Worker-only network/IP and calibrated UTC preparation.
   * A failure prevents every SDK call. Must finish before teardown. */
  h2_pal_result_t (*prepare)(h2_runtime_t* runtime, void* user);
  /** Optional completed-display evidence. Called on the UI thread after
   * draw/present; pixels are native RGB565 and borrowed only for this call. */
  h2_pal_result_t (*observe)(void* user, const uint8_t* pixels,
                             size_t stride_bytes, int width, int height);
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
 * initialization, cached reentry, readiness, binding status and code.
 * Does not provision, activate, poll, finish binding, unbind or reset. */
h2_pal_result_t h2_alipay_e2e_execute(h2_runtime_t* runtime,
                                      const h2_alipay_e2e_config_t* config,
                                      h2_alipay_e2e_result_t* result);

/** Standalone single-page App. Owns LVGL/display lifetime and one SDK worker;
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
