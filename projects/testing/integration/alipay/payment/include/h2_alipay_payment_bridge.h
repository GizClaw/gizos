#ifndef H2_ALIPAY_PAYMENT_BRIDGE_H
#define H2_ALIPAY_PAYMENT_BRIDGE_H

#include "h2_alipay_payment.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Borrowed execution boundary for one SDK implementation. The caller
 * serializes all payment operations and joins workers before release.
 * enter validates initialization and acquires any implementation lock;
 * leave releases it while preserving an earlier error. record accepts only
 * non-secret diagnostics. The hooks and user outlive the returned API. */
typedef struct h2_alipay_payment_bridge {
  void* user;
  h2_pal_result_t (*enter)(void* user);
  h2_pal_result_t (*leave)(void* user, h2_pal_result_t result);
  void (*record)(void* user, const char* operation, int result, uint32_t item);
} h2_alipay_payment_bridge_t;

/** Shared SDK-signature adapter. API user points to the borrowed bridge.
 * Only one tap transport may be attached at a time because the original
 * callback ABI has no user argument. stop_tap detaches and wipes that session
 * before its owner releases dependencies. */
const h2_alipay_payment_vtable_t* h2_alipay_payment_bridge_vtable(void);

#ifdef __cplusplus
}
#endif
#endif
