#ifndef H2_ALIPAY_H
#define H2_ALIPAY_H

#include "core/h2_pal_errors.h"
#include "h2_alipay_trace.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define H2_ALIPAY_BINDING_CODE_CAPACITY 256u

/** Adapter-owned boot state. Serialize access; never reset after an SDK
 * attempt. An error is cached too, since callback registration may have
 * partially completed. The driver callback is invoked at most once. */
typedef struct h2_alipay_initialization {
  bool attempted;
  int sdk_rc;
} h2_alipay_initialization_t;
h2_pal_result_t h2_alipay_initialize_once(h2_alipay_initialization_t* state,
                                          int (*initialize)(void), int* sdk_rc,
                                          bool* first_attempt);

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
 * check_ready is a legacy optional policy callback; the vendor adapter leaves
 * it NULL and binding calls do not invoke it. binding_status is an optional
 * SDK query, not a prerequisite for requesting a binding code. Failed
 * get_binding_code clears output. No operation polls, unbinds or completes
 * account binding. Output buffers belong to the caller.
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
