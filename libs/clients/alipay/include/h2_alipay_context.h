#ifndef H2_ALIPAY_CONTEXT_H
#define H2_ALIPAY_CONTEXT_H

#include "h2_alipay_client.h"
#include "os/h2_pal_sync.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Configure the self-implemented original-API facade before alipay_pre_init.
 * Copies config, borrows its pointers and sync until release succeeds.
 * Callers serialize configure/release with every operation. The facade owns
 * one pending binding session; business calls serialize through Sync PAL.
 * Headers come unchanged from the pinned header-only Alipay API delivery.
 * No vendor archive or implementation is used. The original
 * alipay_get_companyName/productModel getters return injected fields after
 * configure, including before init. They make no network request; successful
 * lengths exclude the NUL terminator and input capacities must include it.
 * Implements initialization, binding-code generation, query/finish, durable
 * bound status, account text/static URLs, unbind/reset, remote binding,
 * balance bitmap, payer/collection codes, both payment-result variants,
 * income details and the six original device-info getters. Only product type 3
 * and PAYCODE | SQUARE with optional NFCPAY/BLEPAY have a protocol
 * implementation. String payment queries update ext_data_len only when
 * failure-detail data is returned; other outcomes preserve its input capacity,
 * matching the delivery. Account/static URL bytes exclude NUL; callers
 * terminate display strings. Static URLs are available without configure.
 * Account operations require init. alipay_query_flow_break may run concurrently
 * with an active result query. It uses the linked atomic provider and the
 * existing HTTP cancellation hook; provider non-interruptible operations can
 * delay its effect. New queries clear the prior break request.
 * Configure/release require callers joined. NFC callback registration and
 * multi-architecture setup may precede configure while callers are quiescent.
 * Registration requires both send/status hooks; invalid registrations retain
 * the previous callbacks. Function pointers are copied and borrowed until
 * replacement/release after all calls return. Quick-pay changes invoke the
 * copied callback on the caller's thread outside the facade mutex, before
 * committing the quick flag. Quick-setting operations and callback replacement
 * require caller serialization through completion. A callback may query
 * binding/diagnostic getters; recursive quick-setting calls and
 * configure/release from callbacks are forbidden. Write-result updates and
 * guide queries share this caller-serialization requirement. They parse
 * supplied raw URL/NDEF data and update local state. NFC receive entries
 * process caller-acquired raw URL/NDEF data, generate payer codes and emit
 * NFC/direct-BLE reply bytes through registered callbacks. Standard, VIP and
 * cabinet paths retain their context/quick/lock distinctions. Status SUCCESS
 * describes SDK processing; send/direct callback results do not prove a
 * physical write or a paid transaction. Connected BLE reception reassembles a
 * bounded 10-byte-header packet, verifies the delivered device signing root
 * through libs/crypto and emits an ECDH/AES-256 reply in MTU-sized Notify
 * packets. Callers serialize receive calls, callback replacement and release
 * through the entire call and callback sequence. Receive callbacks run outside
 * the mutex on the caller's thread and may read diagnostics; recursive
 * receive/business calls, configure and release are forbidden. Callback data is
 * borrowed only until that callback returns. Radio selection, acquisition,
 * transmission, confirmation and presentation belong to the integration caller.
 * Truncated headers, oversized frames and malformed fields are rejected safely.
 * Differential qualification includes actual receive callbacks and invalid
 * device signatures plus the original cipher function's successful outputs;
 * authenticated public BLE success still needs a valid vendor-signed synthetic
 * device-key fixture. A debug-token pointer borrows facade storage until the
 * next debug call or release. Callers serialize its use with those operations.
 * Static NFC cipher initialization may precede alipay_pre_init after configure
 * and remains ready across account/context cleanup. It prepares the delivered
 * transport profile through libs/crypto and starts no radio. The token-bound
 * ciphertext getter uses the current handoff context. Its length is
 * output-only: normal 18/19-byte payer strings produce 44 raw base64 bytes
 * without NUL. Callers provide at least 44 actual output bytes despite the
 * original header's 32-byte minimum annotation; undersized storage is not
 * qualified. Multi-architecture value 1 suppresses local quick-flag changes;
 * other byte values retain the delivered local-setting behavior. The original
 * progress hook forwards config.binding_progress on the notifying caller's
 * thread outside the mutex. It may query diagnostics; configure/release from
 * the handler is forbidden. No progress events are synthesized. The deprecated
 * item hooks use only the optional legacy Pref namespace and return public PAL
 * errors. Read capacity is uint32_t; short reads preserve bytes and report
 * required length. Other failures preserve bytes and length. Writes/deletes
 * commit; temporary reads are wiped before release. The caller supplies
 * device-bound protection in that provider. These hooks do not discover an
 * existing vendor store. Explicit alipay_migrate_data reads the selected legacy
 * items, derives fresh ECDH material, rewraps the seed through libs/crypto and
 * commits/readbacks the converted account. It checks a generated payer code
 * before deleting the six consumed legacy items. Like the delivery, a repeated
 * call on an already bound account returns 99. It does not synthesize a missing
 * TID from the unused legacy item 65; converted account text preserves its
 * original raw byte lengths, including stored NULs. The synchronization entry
 * accepts the original JSON commands and returns facade-owned reply storage,
 * including NUL, until the next successful sync call or release; its reported
 * length excludes NUL. Callers serialize these accesses. It reuses injected
 * Crypto and Pref through the crypto library, without selecting any
 * chip-to-chip transport. Recognized-command result "0" is the original
 * dispatch result, not proof of durable binding: query binding status/readback
 * separately. A generated synchronization private key is consumed by factor
 * processing. An exact C-string getCertificate value "yes" additionally
 * exports the configured device_certificate Pref record's public certificate
 * and a fresh ECDSA proof over the factor. The private factory scalar stays
 * in the protected caller-selected store and temporary crypto buffers.
 * Nonstring getCertificate values are rejected safely; the delivery compares
 * their NULL C-string pointers. Provisioning entries use the same injected
 * Pref and existing RPC path. Direct provisioning preserves the selected
 * ECDH profile's requirement for a supplied privateKey while no certificate
 * generation is pending. OTA's zero result is transport/dispatch completion;
 * a nested certificate write can fail independently. Activation uses only
 * config.activation_asset from the integration's factory environment and
 * suppresses another request after verified server success in this session.
 * No factory credential or opaque vendor activation blob is built in.
 * Transit entries use the same injected Pref and signed HTTP path. Module
 * registration enables owned transit-cache cleanup on account reset. Activity
 * and list operations retain the original output/short-buffer semantics.
 * alipay_break_rpc can run concurrently with an active transit operation;
 * it uses the existing HTTP cancellation hook and a separate atomic flag.
 * New transit calls reset that flag; release requires all callers joined.
 * The ONLINE render path refreshes the binary code without selecting a last
 * offline card, matching the delivery. Native ALIPAY_OFFLINE_CERT_V1/V2
 * generation uses libs/crypto for secp192k1/SHA-1 and DER encoding, tracks
 * usage and selects the last offline card. MOT double/triple-SM2 certificates
 * use libs/crypto's identity-bound SM2/SM3 and emit the original raw signature.
 * Other native certificate formats and
 * downloaded-script generators are not implemented or qualified yet.
 * alipay_get_productType returns UINT32_MAX
 * while unconfigured/lock failure. Original query ABI: a valid initialized
 * query returns OK; UNKNOWN indicates pending/transport failure, BINDING_OK
 * verified completion, FAIL other errors. Use the H2 diagnostics below to
 * distinguish PAL/storage errors. */
h2_pal_result_t h2_alipay_independent_configure(
    const h2_alipay_client_config_t* config, const h2_pal_sync_api_t* sync);

typedef struct h2_alipay_independent_binding_result {
  h2_alipay_client_binding_t binding;
  int result;
  h2_alipay_client_stage_t stage;
  int http_status, rpc_status;
  unsigned requests;
} h2_alipay_independent_binding_result_t;

/** Copy non-secret diagnostics of the last binding-code or binding-query
 * core call under the facade lock. Precondition failures retain the previous
 * core diagnostics. Does not issue a request. Callers serialize operations
 * when associating a request with these results. */
h2_pal_result_t h2_alipay_independent_last_binding_result(
    h2_alipay_independent_binding_result_t* out);

/** Local injected-store snapshot after init, under the facade lock. No RPC. */
h2_pal_result_t h2_alipay_independent_binding_snapshot(
    h2_alipay_client_binding_t* out);

/** Last original payer-code/result call's non-secret diagnostics, copied
 * under the facade lock. No SDK or network operation is performed. */
h2_pal_result_t h2_alipay_independent_last_payment_result(
    h2_alipay_client_payment_t* out);

/** Install an externally completed NFC/CPU payment handoff. This bridge
 * borrows no text after returning, requires initialization and a committed
 * binding, and stores no persistent state. Input lengths exclude NUL; every
 * input must contain 1..31 bytes. The caller serializes this with NFC flows.
 * It enables verification of BLE result packets and plain-code retrieval;
 * it does not implement NFC reception, encryption or acknowledge a payment. */
h2_pal_result_t h2_alipay_independent_set_nfc_context(const char* token,
                                                      const char* squeeze,
                                                      const char* paycode);

/** Release after all callers have joined. Wipes the pending session without
 * a server request or persistent deletion. A mutex-destroy failure retains
 * ownership and all borrowed dependencies for retry. Repeated release is OK. */
h2_pal_result_t h2_alipay_independent_release(void);

#ifdef __cplusplus
}
#endif
#endif
