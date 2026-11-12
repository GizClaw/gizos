#ifndef H2_ALIPAY_CLIENT_H
#define H2_ALIPAY_CLIENT_H

#include "h2_alipay_protocol.h"
#include "application/h2_pal_http.h"
#include "os/h2_pal_crypto.h"
#include "os/h2_pal_pref.h"
#include "os/h2_pal_time.h"

#ifdef __cplusplus
extern "C" {
#endif

#define H2_ALIPAY_CLIENT_BINDING_CODE_CAPACITY 256u

/** Borrowed dependencies and strings, valid until the blocking call returns.
 * HTTP must provide verified HTTPS and response-header delivery. crypto must
 * support random, P-256 generation and uncompressed point export. The caller
 * supplies calibrated UTC and serializes operations for a Pref namespace.
 * This implementation covers the child-watch (type 3), network/ECDH profile.
 * It neither links nor calls the vendor SDK. */
typedef struct h2_alipay_client_config {
  const h2_pal_mem_api_t* mem;
  const h2_pal_time_api_t* time;
  const h2_pal_http_api_t* http;
  const h2_pal_pref_api_t* pref;
  const h2_pal_crypto_api_t* crypto;
  h2_alipay_protocol_identity_t identity;
  const char* company;
  /** Dedicated stable namespace. Existing SDK namespaces are never read,
   * copied or reset. Identity changes reject an existing mismatched record. */
  const char* pref_namespace;
  uint32_t feature;
  uint32_t compile_timestamp_s;
} h2_alipay_client_config_t;

/** Last attempted operation, retained for diagnosis without payload export. */
typedef enum h2_alipay_client_stage {
  H2_ALIPAY_CLIENT_NOT_STARTED = 0,
  H2_ALIPAY_CLIENT_STORAGE,
  H2_ALIPAY_CLIENT_RANDOM,
  H2_ALIPAY_CLIENT_KEYPAIR,
  H2_ALIPAY_CLIENT_PUBLIC_KEY,
  H2_ALIPAY_CLIENT_ENCODE,
  H2_ALIPAY_CLIENT_REGISTER,
  H2_ALIPAY_CLIENT_COMMIT,
  H2_ALIPAY_CLIENT_PREPARE_BIND,
  H2_ALIPAY_CLIENT_TOKEN,
  H2_ALIPAY_CLIENT_QR,
} h2_alipay_client_stage_t;

/** Caller-owned result. code and length contain binding payload text only;
 * the caller owns any QR/barcode encoding, rendering and display lifecycle.
 * private_key and token are sensitive in-memory state for this pending code;
 * retain them while displaying it, then clear the entire result. No account
 * binding completion, polling, reset, unbind or payment operation is exposed.
 * Numeric diagnostics remain available after failure, while code, token and
 * private_key are cleared. registered means this call obtained a device name.
 */
typedef struct h2_alipay_client_result {
  h2_alipay_client_stage_t stage;
  int http_status;
  int rpc_status;
  unsigned requests;
  bool registered;
  char code[H2_ALIPAY_CLIENT_BINDING_CODE_CAPACITY];
  size_t length;
  char token[129];
  h2_pal_p256_private_key_t private_key;
} h2_alipay_client_result_t;

/** Make at most one registration request if persistent state is absent, then
 * one signed prepareBind request. No request is retried. Successful device
 * name and business identifier persistence is required before prepareBind.
 * Blocking: call on a business worker, never UI or ISR. No state is retained
 * outside Pref and the caller-owned result. PAL errors propagate unchanged. */
h2_pal_result_t h2_alipay_client_get_binding_code(
    const h2_alipay_client_config_t* config, h2_alipay_client_result_t* out);

/** Synchronously wipe a caller-owned result, including its private material.
 * NULL is harmless. Do not clear a result concurrently with an operation. */
void h2_alipay_client_result_clear(h2_alipay_client_result_t* result);

#ifdef __cplusplus
}
#endif
#endif
