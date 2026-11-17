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
 * support random, P-256 generation and uncompressed point export; completion
 * additionally requires P-256 ECDH; payer codes require AES-128-CBC without
 * padding and HMAC-SHA1. The caller
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
  /** Original device classification. Only child-watch type 3 is supported
   * by the current binding protocol; other values return UNSUPPORTED. */
  uint32_t product_type;
  uint32_t feature;
  uint32_t compile_timestamp_s;
  /** Optional synchronous cancellation predicate and borrowed context.
   * Called from the blocking HTTP provider; must be safe against the caller's
   * cancellation writer. CLOSED propagates after cancellation. */
  h2_pal_http_cancel_fn cancel_cb;
  void* cancel_user;
  /** Optional caller-owned legacy item store for the original migration hooks.
   * Both fields are NULL to disable it. Otherwise use a dedicated namespace
   * whose provider supplies the product's device binding, integrity and
   * confidentiality. Items use stable old_<8 lowercase hex digits> blob keys.
   * This adapter does not discover or decode an existing vendor store. */
  const h2_pal_pref_api_t* legacy_pref;
  const char* legacy_pref_namespace;
  /** Optional original binding-progress hook. The facade invokes it on the
   * notifying caller's thread outside its mutex. The callback and user remain
   * borrowed until release after all calls return. UI users dispatch their own
   * events; the library does not change threads or synthesize progress. */
  void (*binding_progress)(void* user, int status);
  void* binding_progress_user;
  /** Optional factory activation asset as raw JSON, excluding NUL. Borrowed
   * until release. Supplying this replaces the vendor-owned asset-registration
   * hook and makes the device ready for explicit alipay_active_device calls.
   * The integration obtains this material through its selected factory store.
   * No opaque asset or private factory credential is built into the library.
   * Received provisioning JSON uses the owned device_certificate Pref key,
   * which survives account unbind/reset. The selected Pref must protect and
   * retain it according to the product's factory-storage contract. */
  const uint8_t* activation_asset;
  size_t activation_asset_length;
  /** Retained for configuration ABI compatibility; ignored. Downloaded transit
   * scripts are unsupported and no interpreter is linked. */
  size_t script_stack_limit;
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
  H2_ALIPAY_CLIENT_QUERY_BIND,
  H2_ALIPAY_CLIENT_FACTORS,
  H2_ALIPAY_CLIENT_FINISH_BIND,
  H2_ALIPAY_CLIENT_BINDING_COMMIT,
  H2_ALIPAY_CLIENT_PAYCODE,
  H2_ALIPAY_CLIENT_QUERY_PAYMENT,
  H2_ALIPAY_CLIENT_PAYCODE_DECRYPT,
  H2_ALIPAY_CLIENT_UNBIND,
  H2_ALIPAY_CLIENT_COLLECTION_CODE,
  H2_ALIPAY_CLIENT_QUERY_REMOTE_BINDING,
  H2_ALIPAY_CLIENT_QUERY_BALANCE,
  H2_ALIPAY_CLIENT_QUERY_INCOME,
  H2_ALIPAY_CLIENT_DZS_PRECHECK,
  H2_ALIPAY_CLIENT_DZS_GENERATE,
  H2_ALIPAY_CLIENT_DZS_QUERY,
  H2_ALIPAY_CLIENT_DZS_CANCEL,
  H2_ALIPAY_CLIENT_SCAN_ROUTE,
  H2_ALIPAY_CLIENT_SCAN_CONSULT,
  H2_ALIPAY_CLIENT_SCAN_PAY,
  H2_ALIPAY_CLIENT_PROVISION,
  H2_ALIPAY_CLIENT_DEVICE_ACTIVATE,
} h2_alipay_client_stage_t;

/** Caller-owned result. code and length contain binding payload text only;
 * the caller owns any QR/barcode encoding, rendering and display lifecycle.
 * private_key and token are sensitive in-memory state for this pending code;
 * retain them until completion or cancellation, then clear the entire result.
 * finish_acknowledged prevents repeat finish RPCs when a local commit fails
 * after an acknowledged finish in this session. Never modify it manually.
 * A failed code request clears code, token and private_key while retaining
 * numeric diagnostics. Query failures retain the retry state described below.
 * registered means the code request obtained a device name.
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
  bool finish_acknowledged;
} h2_alipay_client_result_t;

/** Non-secret local/query outcome. No identifiers, seeds or keys escape.
 * COMPLETE means finishBind succeeded, or an explicit validated legacy
 * conversion completed, and an atomic record was committed and read back in
 * the injected Pref. A legacy conversion may have no TID, matching the
 * delivery. Retention is the caller-selected provider policy; a volatile
 * provider does not establish reset durability. NEEDS_VERIFICATION means staged
 * data or a storage error, never sufficient evidence of completed binding. */
typedef enum h2_alipay_client_binding_state {
  H2_ALIPAY_CLIENT_UNBOUND = 0,
  H2_ALIPAY_CLIENT_PENDING,
  H2_ALIPAY_CLIENT_COMPLETE,
  H2_ALIPAY_CLIENT_FAILED,
  H2_ALIPAY_CLIENT_NEEDS_VERIFICATION,
} h2_alipay_client_binding_state_t;

typedef struct h2_alipay_client_binding {
  h2_alipay_client_binding_state_t state;
  int storage_rc;
  size_t tid_length, uid_length, shared_key_length, seed_length;
} h2_alipay_client_binding_t;

/** Validate borrowed configuration/identity without I/O or allocation.
 * Dependency pointers are checked; provider capabilities may still fail at
 * use. Pref must protect secret binding material with the product's required
 * confidentiality and integrity. This library never selects a provider. */
h2_pal_result_t h2_alipay_client_validate_config(
    const h2_alipay_client_config_t* config);

/** Build a local aid-code payload without opening a radio or network.
 * Capacity includes NUL. On success length excludes NUL; on NO_SPACE length
 * includes the required NUL and output stays unchanged. BLE indicates the
 * caller-owned 3802 service mode and does not start that service. */
h2_pal_result_t h2_alipay_client_get_aid_code(
    const h2_alipay_client_config_t* config, bool ble, char* out,
    size_t* length);

/** Read local committed binding status, without network calls. Missing binding
 * data is UNBOUND/OK; malformed, mismatched or staged data cannot be COMPLETE.
 * Caller serializes every operation on this namespace. */
h2_pal_result_t h2_alipay_client_binding_snapshot(
    const h2_alipay_client_config_t* config, h2_alipay_client_binding_t* out);

typedef enum h2_alipay_client_account_field {
  H2_ALIPAY_CLIENT_ACCOUNT_LOGIN,
  H2_ALIPAY_CLIENT_ACCOUNT_NICKNAME,
} h2_alipay_client_account_field_t;

/** Read one committed account field without network calls. Output bytes and
 * successful lengths exclude a NUL terminator. Exact capacity is sufficient.
 * On NO_SPACE, length receives the required size and out is unchanged.
 * Missing or staged binding returns NOT_FOUND without changing out or length.
 * Other PAL/storage errors propagate. Caller serializes namespace operations.
 */
h2_pal_result_t h2_alipay_client_get_account_text(
    const h2_alipay_client_config_t* config,
    h2_alipay_client_account_field_t field, uint8_t* out, size_t* length);

/** Attempt the network profile's unbind RPC when a committed account exists,
 * then remove this client's binding and registration records. Local cleanup
 * proceeds after a remote failure, matching the delivery; numeric RPC
 * diagnostics remain in out. Returns local storage/configuration errors.
 * Other Pref keys are preserved. Blocking worker only. Caller serializes the
 * namespace and clears any pending binding session after calling this API. */
h2_pal_result_t h2_alipay_client_unbind(const h2_alipay_client_config_t* config,
                                        h2_alipay_client_result_t* out);

typedef enum h2_alipay_client_remote_binding {
  H2_ALIPAY_CLIENT_REMOTE_BOUND,
  H2_ALIPAY_CLIENT_REMOTE_UNBOUND,
  H2_ALIPAY_CLIENT_REMOTE_UNKNOWN,
} h2_alipay_client_remote_binding_t;

/** Query the network profile's remote binding state. A locally absent binding
 * returns UNBOUND without I/O. A server UNBIND/1007 runs the same unbind and
 * owned-record cleanup as an explicit unbind. PAL and parse errors propagate;
 * state is valid only on OK. Blocking worker only; serialize the namespace. */
h2_pal_result_t h2_alipay_client_query_remote_binding(
    const h2_alipay_client_config_t* config,
    h2_alipay_client_remote_binding_t* state, h2_alipay_client_result_t* out);

/** Query child-watch balance and project it into the original 112x32 bitmap.
 * Amount text stays private. On failure bitmap is unchanged. On success only
 * the first four bytes and glyph cells are overwritten, matching the delivery;
 * callers initialize the entire matrix when a blank background is required.
 * Blocking worker only. No display, NFC or BLE operation is performed. */
h2_pal_result_t h2_alipay_client_get_balance_bitmap(
    const h2_alipay_client_config_t* config, char bitmap[32][14],
    h2_alipay_client_result_t* out);

/** One explicit query, then (when authorized by its response) finishBind.
 * session comes from get_binding_code and remains caller-owned. PENDING/OK
 * means poll again; transport failures preserve the pending session. Validated
 * factors are staged atomically before finish; retry resumes a staged finish
 * without querying/deriving again. A failed final commit retries locally after
 * an in-memory finish acknowledgement. Across restart a staged record reports
 * NEEDS_VERIFICATION; an explicit call may retry finish with its retained
 * token. An interrupted/lost acknowledgement can cause a repeated finish
 * request. No automatic polling, unbind, reset or payment occurs. Blocking
 * worker only. PAL errors propagate; out and session diagnostics never include
 * payloads. */
h2_pal_result_t h2_alipay_client_query_binding_result(
    const h2_alipay_client_config_t* config, h2_alipay_client_result_t* session,
    h2_alipay_client_binding_t* out);

/** Make at most one registration request if persistent state is absent, then
 * one signed prepareBind request. No request is retried. Successful device
 * name and business identifier persistence is required before prepareBind.
 * Blocking: call on a business worker, never UI or ISR. No state is retained
 * outside Pref and the caller-owned result. PAL errors propagate unchanged. */
h2_pal_result_t h2_alipay_client_get_binding_code(
    const h2_alipay_client_config_t* config, h2_alipay_client_result_t* out);

typedef enum h2_alipay_client_payment_state {
  H2_ALIPAY_CLIENT_PAYMENT_UNKNOWN = 0,
  H2_ALIPAY_CLIENT_PAYMENT_PAID,
  H2_ALIPAY_CLIENT_PAYMENT_PENDING,
  H2_ALIPAY_CLIENT_PAYMENT_FAILED,
  H2_ALIPAY_CLIENT_PAYMENT_UNBOUND,
  H2_ALIPAY_CLIENT_PAYMENT_DISABLED,
  H2_ALIPAY_CLIENT_PAYMENT_PROCESSING,
} h2_alipay_client_payment_state_t;

/** Last attempted payment operation, without code or credential export.
 * PAID requires a successful business response. ack_result is independent:
 * an acknowledgement error does not change that business outcome. */
typedef struct h2_alipay_client_payment {
  h2_alipay_client_payment_state_t state;
  h2_alipay_client_stage_t stage;
  int result, http_status, rpc_status, ack_result;
  unsigned requests;
  char amount[32];
} h2_alipay_client_payment_t;

/** Generate the delivered ECDH profile's payer code locally from completed
 * binding, device MAC and current UTC. No HTTP request is made. *length is
 * input capacity including NUL, output digit count excluding NUL; a short
 * buffer is untouched and receives required count. Uses injected Memory PAL
 * for temporary material and wipes it before release. Caller serializes
 * operations on this configuration and namespace. */
h2_pal_result_t h2_alipay_client_get_paycode(
    const h2_alipay_client_config_t* config, uint8_t* out, size_t* length,
    h2_alipay_client_payment_t* result);

/** Generate the child-watch collection code from the payer code and current
 * 32-bit time. No network call. Output is NUL-terminated; successful length
 * excludes NUL. On NO_SPACE, length receives the required size and out is
 * unchanged. Caller serializes namespace and clock changes. */
h2_pal_result_t h2_alipay_client_get_collectioncode(
    const h2_alipay_client_config_t* config, uint8_t* out, size_t* length,
    h2_alipay_client_payment_t* result);

/** Query the actual previously displayed 18/19-digit code using verified
 * HTTPS, original signing and current local TID/UID. At most one query plus
 * a business-success acknowledgement; no automatic polling or debit request.
 * ext_length is capacity on input, raw failure-detail length on output.
 * NULL ext_data requires zero capacity and discards details. A short buffer
 * is untouched and reports the required length. Server DELSEED responses
 * remove only this namespace's binding record; registration is preserved.
 * Blocking worker call; all temporary allocations use the injected Memory
 * PAL and are wiped/released before return. PAL failures remain UNKNOWN.
 * All outputs must be distinct from code and each other. */
h2_pal_result_t h2_alipay_client_query_order_result(
    const h2_alipay_client_config_t* config, const char* code,
    h2_alipay_client_payment_t* result, char* ext_data, size_t* ext_length);

/** Query a previously generated payer code with its NFC token in npayToken.
 * Request/result, acknowledgement, cancellation and ownership semantics match
 * query_order_result. token is borrowed for this blocking call only. */
h2_pal_result_t h2_alipay_client_query_nfc_payment(
    const h2_alipay_client_config_t* config, const char* code,
    const char* token, h2_alipay_client_payment_t* result, char* ext_data,
    size_t* ext_length);

/** Original bitmap-result variant. Confirmed success writes the first four
 * bitmap bytes and glyph cells; other outcomes preserve the matrix. The
 * legacy bitmap API treats failure-detail and sound-wave pushes as pending.
 * All request,
 * acknowledgement, blocking and ownership rules of the string query apply. */
h2_pal_result_t h2_alipay_client_query_order_bitmap(
    const h2_alipay_client_config_t* config, const char* code,
    h2_alipay_client_payment_t* result, char bitmap[32][14]);

typedef struct h2_alipay_client_income {
  bool success;
  char bitmap[32][14];
  char message[256];
} h2_alipay_client_income_t;

/** Query the first income record for a collection code of at most 1024 bytes.
 * Requires registration context, but does not require a local bound account.
 * success becomes false before I/O. Success overwrites glyph cells and, when
 * present, the 256-byte remark field. Other bytes are preserved. A full-length
 * remark is not NUL-terminated. Blocking, borrowed dependencies and allocation
 * rules match order-result queries. No acknowledgement or debit is issued. */
h2_pal_result_t h2_alipay_client_query_income(
    const h2_alipay_client_config_t* config, const char* code,
    h2_alipay_client_income_t* out, h2_alipay_client_payment_t* result);

/** Synchronously wipe a caller-owned result, including its private material.
 * NULL is harmless. Do not clear a result concurrently with an operation. */
void h2_alipay_client_result_clear(h2_alipay_client_result_t* result);

#ifdef __cplusplus
}
#endif
#endif
