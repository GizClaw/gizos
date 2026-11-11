#ifndef H2_ALIPAY_H
#define H2_ALIPAY_H

#include "h2/pal/core/h2_pal_errors.h"
#include "h2_alipay_trace.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define H2_ALIPAY_BINDING_CODE_CAPACITY 256u

typedef enum h2_alipay_source {
  H2_ALIPAY_SOURCE_CONTROLLED = 0,
  H2_ALIPAY_SOURCE_VENDOR_SDK = 1,
} h2_alipay_source_t;

typedef enum h2_alipay_blocker {
  H2_ALIPAY_BLOCKER_NONE = 0,
  H2_ALIPAY_BLOCKER_SECURITY_SCHEME,
  H2_ALIPAY_BLOCKER_SECURE_STORAGE,
  H2_ALIPAY_BLOCKER_CREDENTIAL,
  H2_ALIPAY_BLOCKER_ACTIVATION,
  H2_ALIPAY_BLOCKER_PRODUCT_APPROVAL,
  H2_ALIPAY_BLOCKER_FEATURES,
  H2_ALIPAY_BLOCKER_IDENTITY,
  H2_ALIPAY_BLOCKER_NETWORK,
  H2_ALIPAY_BLOCKER_TIME,
} h2_alipay_blocker_t;

/** Blocking operations, serialized by the adapter. Call from a business
 * worker, never UI/ISR. PAL success describes the adapter call; sdk_rc is
 * the unchanged vendor return. pre_init caches its first attempt, including
 * failure, for the entire SDK process/boot. Dependencies stay alive until
 * reset: the original SDK has no symmetric callback unregistration API.
 *
 * check_ready validates persistent commissioning evidence without provisioning
 * or activation. A READY flag in RAM is not evidence. Only read binding_status
 * after check_ready: the vendor bool cannot distinguish storage failure from
 * unbound. Failed get_binding_code clears output. No operation polls, unbinds,
 * resets or completes account binding. Output buffers belong to the caller.
 */
typedef struct h2_alipay_vtable {
  h2_pal_result_t (*pre_init)(void* user, int* sdk_rc, bool* first_attempt);
  h2_pal_result_t (*check_ready)(void* user, uint32_t feature,
                                 h2_alipay_blocker_t* blocker);
  h2_pal_result_t (*binding_status)(void* user, bool* bound);
  h2_pal_result_t (*get_binding_code)(void* user, uint32_t feature, char* code,
                                      size_t capacity, size_t* length,
                                      int* sdk_rc);
} h2_alipay_vtable_t;

typedef struct h2_alipay_api {
  void* user;
  const h2_alipay_vtable_t* vtable;
  h2_alipay_source_t source;
  /** Bounded trace; only read/reset after all SDK callers join. */
  h2_alipay_trace_t* trace;
  const char* sdk_manifest_sha256;
} h2_alipay_api_t;

#ifdef __cplusplus
}
#endif
#endif
