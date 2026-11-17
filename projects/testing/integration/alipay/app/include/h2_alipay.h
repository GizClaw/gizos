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

typedef enum h2_alipay_source {
  H2_ALIPAY_SOURCE_CONTROLLED = 0,
  H2_ALIPAY_SOURCE_INDEPENDENT = 2,
} h2_alipay_source_t;

/** Retention selected by final-entry storage assembly. Session mode verifies
 * writes/readback only within this boot; it never establishes durability. */
typedef enum h2_alipay_storage_scope {
  H2_ALIPAY_STORAGE_PERSISTENT = 0,
  H2_ALIPAY_STORAGE_SESSION = 1,
} h2_alipay_storage_scope_t;

typedef enum h2_alipay_binding_state {
  H2_ALIPAY_BINDING_UNBOUND = 0,
  H2_ALIPAY_BINDING_PENDING,
  H2_ALIPAY_BINDING_STARTED,
  H2_ALIPAY_BINDING_COMPLETE,
  H2_ALIPAY_BINDING_FAILED,
  H2_ALIPAY_BINDING_NEEDS_VERIFICATION,
} h2_alipay_binding_state_t;

/** Caller-owned, non-secret binding diagnostics. COMPLETE requires the SDK
 * completion result and readback of TID, UID, shared key and seed in the API's
 * selected storage scope, plus a completion record matching those values.
 * Lengths exclude any NUL. No account identifiers, key bytes or seed bytes are
 * returned. sdk_rc and sdk_status preserve vendor results; storage_rc is a PAL
 * result. For a successful local snapshot, sdk_status maps the SDK's local
 * boolean to bound/unbound and sdk_rc is zero; no server response was obtained.
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

/** Blocking App-facing binding operations. Call on one serialized business
 * worker, never UI/ISR. sdk_rc preserves the compatible C API result.
 * pre_init caches its first attempt until the adapter is released after all
 * callers join. binding_snapshot reads only local state; query_binding_result
 * advances one bounded binding step. Missing operations report UNSUPPORTED.
 * get_binding_code clears output on failure. The adapter never resets or
 * unbinds an account implicitly. Caller owns all output buffers. */
typedef struct h2_alipay_vtable {
  h2_pal_result_t (*pre_init)(void* user, int* sdk_rc, bool* first_attempt);
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
  h2_alipay_storage_scope_t storage_scope;
} h2_alipay_api_t;

#ifdef __cplusplus
}
#endif
#endif
