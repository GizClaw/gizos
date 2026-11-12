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
  H2_ALIPAY_SOURCE_INDEPENDENT = 2,
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

typedef enum h2_alipay_binding_state {
  H2_ALIPAY_BINDING_UNBOUND = 0,
  H2_ALIPAY_BINDING_PENDING,
  H2_ALIPAY_BINDING_STARTED,
  H2_ALIPAY_BINDING_COMPLETE,
  H2_ALIPAY_BINDING_FAILED,
  H2_ALIPAY_BINDING_NEEDS_VERIFICATION,
} h2_alipay_binding_state_t;

/** Caller-owned, non-secret binding diagnostics. COMPLETE requires the SDK
 * completion result and durable readback of TID, UID, shared key and seed,
 * plus a completion record matching those values. Lengths exclude any NUL.
 * No account identifiers, key bytes or seed bytes are returned.
 * sdk_rc and sdk_status preserve vendor results; storage_rc is a PAL result.
 * For a successful local snapshot, sdk_status maps the SDK's local boolean
 * to bound/unbound and sdk_rc is zero; no server response was obtained.
 */
typedef struct h2_alipay_binding_result {
  h2_alipay_binding_state_t state;
  int sdk_rc;
  int sdk_status;
  int storage_rc;
  size_t tid_length;
  size_t uid_length;
  size_t shared_key_length;
  size_t seed_length;
} h2_alipay_binding_result_t;

/** Blocking operations, serialized by the adapter. Call from a business
 * worker, never UI/ISR. PAL success describes the adapter call; sdk_rc is
 * the original C return for the selected backend. A failure before invoking
 * that entry may instead retain a negative PAL result in this field.
 * pre_init caches its first attempt, including
 * failure, for the entire SDK process/boot. Dependencies stay alive until
 * reset: the original SDK has no symmetric callback unregistration API.
 *
 * check_ready is a legacy optional policy callback; the vendor adapter leaves
 * it NULL and binding calls do not invoke it. binding_status is an optional
 * SDK query, not a prerequisite for requesting a binding code. Failed
 * get_binding_code clears output. Optional query_binding_result performs one
 * binding-result query, including the vendor's finishBind path. It requires a
 * successful code request in this boot and never runs an automatic retry.
 * binding_snapshot checks only local durable state; it makes no RPC.
 * Missing optional operations are unsupported. The adapter never calls
 * standalone unbind/reset APIs; the vendor may perform its own failed-binding
 * cleanup. Output buffers belong to the caller.
 */
typedef struct h2_alipay_vtable {
  h2_pal_result_t (*pre_init)(void* user, int* sdk_rc, bool* first_attempt);
  h2_pal_result_t (*check_ready)(void* user, uint32_t feature,
                                 h2_alipay_blocker_t* blocker);
  h2_pal_result_t (*binding_status)(void* user, bool* bound);
  h2_pal_result_t (*get_binding_code)(void* user, uint32_t feature, char* code,
                                      size_t capacity, size_t* length,
                                      int* sdk_rc);
  h2_pal_result_t (*query_binding_result)(void* user,
                                          h2_alipay_binding_result_t* out);
  h2_pal_result_t (*binding_snapshot)(void* user,
                                      h2_alipay_binding_result_t* out);
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
