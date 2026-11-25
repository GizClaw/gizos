#ifndef H2_ALIPAY_SCAN_H
#define H2_ALIPAY_SCAN_H

#include "h2_alipay_client.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct h2_alipay_scan h2_alipay_scan_t;

typedef enum h2_alipay_scan_failure {
  H2_ALIPAY_SCAN_AMOUNT_LIMIT,
  H2_ALIPAY_SCAN_BALANCE_INSUFFICIENT,
  H2_ALIPAY_SCAN_COMMON_FAILURE,
  H2_ALIPAY_SCAN_ACCOUNT_MISSING,
  H2_ALIPAY_SCAN_CASE_UNSUPPORTED,
} h2_alipay_scan_failure_t;

typedef struct h2_alipay_scan_code {
  bool success;
  h2_alipay_scan_failure_t failure;
  size_t length;
  char code[1024];
} h2_alipay_scan_code_t;

typedef struct h2_alipay_scan_order {
  bool success;
  char error[8];
  char name[128];
  char logo[1024];
  char amount[32];
} h2_alipay_scan_order_t;

typedef struct h2_alipay_union h2_alipay_union_t;

/** Allocate a blocking, caller-serialized QR/NFC payment session. Configuration
 * is copied and its dependencies remain borrowed until close. */
h2_pal_result_t h2_alipay_union_open(const h2_alipay_client_config_t* config,
                                     h2_alipay_union_t** out);

/** Wipe the session after all calls return. Does not issue a payment or RPC. */
void h2_alipay_union_close(h2_alipay_union_t* session);

/** Parse a bounded code or nested NFC URL, replacing the previous order.
 * NFC input extracts scheme, then codeContent, and drops its query suffix.
 * Parsing can consult the server but never submits a debit. The result uses
 * the original fixed field sizes; unchanged NFC failure fields are preserved.
 */
h2_pal_result_t h2_alipay_union_parse(h2_alipay_union_t* session,
                                      const char* input, bool nfc,
                                      h2_alipay_scan_order_t* order,
                                      h2_alipay_client_payment_t* result);

/** Submit the current order once, then perform at most one result query.
 * amount must match a fixed consulted amount, or supply the open amount.
 * The caller must retain an unknown outcome and must not retry as a new
 * payment. A new parse replaces the previous session and its business ID. */
h2_pal_result_t h2_alipay_union_confirm(h2_alipay_union_t* session,
                                        const h2_alipay_scan_order_t* order,
                                        h2_alipay_scan_failure_t* failure,
                                        h2_alipay_client_payment_t* result);

/** Allocate a serialized scan-payment session using config->mem. Configuration
 * is copied; its dependencies and strings are borrowed until close. Calls are
 * blocking and never create tasks. The caller serializes every session call. */
h2_pal_result_t h2_alipay_scan_open(const h2_alipay_client_config_t* config,
                                    h2_alipay_scan_t** out);

/** After all calls have returned, wipe and release the session. NULL is safe.
 * Close performs no remote cancellation; cancel an active DZS code explicitly
 * before close when its server-side invalidation is required. */
void h2_alipay_scan_close(h2_alipay_scan_t* scan);

/** Query the child-watch CODE_TYPE permission. enabled becomes zero before
 * I/O; it is valid on OK. No latest-payment state is changed. */
h2_pal_result_t h2_alipay_scan_dzs_precheck(h2_alipay_scan_t* scan,
                                            uint8_t* enabled,
                                            h2_alipay_client_payment_t* result);

/** Replace the latest local DZS session and submit an amount of at most 31
 * bytes. The server controls success/failure. Code text is NUL-terminated and
 * length excludes NUL. An empty successful server code has length zero and
 * creates no active query session. No debit or implicit retry is performed. */
h2_pal_result_t h2_alipay_scan_dzs_generate(h2_alipay_scan_t* scan,
                                            const char* amount,
                                            h2_alipay_scan_code_t* out,
                                            h2_alipay_client_payment_t* result);

/** Query the latest generated DZS code once. NOT_FOUND means no active code,
 * without a request. UNKNOWN, PAID and PROCESSING are separate outcomes.
 * A successful query does not discard the session or automatically poll. */
h2_pal_result_t h2_alipay_scan_dzs_query(h2_alipay_scan_t* scan,
                                         h2_alipay_client_payment_t* result);

/** Attempt remote invalidation once and clear local latest-payment state.
 * Repeated cancellation is OK without another request. Remote failure remains
 * visible in result; local clearance does not prove server-side invalidation.
 * Failures before submitting HTTP retain the session for retry. */
h2_pal_result_t h2_alipay_scan_dzs_cancel(h2_alipay_scan_t* scan,
                                          h2_alipay_client_payment_t* result);

#ifdef __cplusplus
}
#endif
#endif
