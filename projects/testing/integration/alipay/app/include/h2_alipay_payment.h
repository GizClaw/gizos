#ifndef H2_ALIPAY_PAYMENT_H
#define H2_ALIPAY_PAYMENT_H

#include "core/h2_pal_errors.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define H2_ALIPAY_PAYMENT_CODE_CAPACITY 32u

typedef enum h2_alipay_payment_state {
  H2_ALIPAY_PAYMENT_UNKNOWN = 0,
  H2_ALIPAY_PAYMENT_PENDING,
  H2_ALIPAY_PAYMENT_PAID,
  H2_ALIPAY_PAYMENT_REJECTED,
  H2_ALIPAY_PAYMENT_UNBOUND,
  H2_ALIPAY_PAYMENT_DISABLED,
} h2_alipay_payment_state_t;

/** Non-secret diagnostics. Processing success and completed NFC writes
 * do not establish payment success; PAID requires an actual result query. */
typedef struct h2_alipay_payment_result {
  h2_alipay_payment_state_t state;
  int sdk_rc, sdk_status;
  int transport_rc, processing_status, processing_error;
  int nfc_write_rc;
  bool processing_notified, write_attempted, response_sent;
  char amount[32];
} h2_alipay_payment_result_t;

/** Borrowed synchronous NFC transport, called on the business worker.
 * Complete the physical write before returning OK; automatic NDEF readback is
 * not required. Bytes are SDK application data; the transport owns terminal
 * framing (NDEF or a negotiated private command). No SDK reentry is allowed.
 * Borrowed data lives only through
 * this call. */
typedef struct h2_alipay_tap_transport {
  void* user;
  h2_pal_result_t (*write_reply)(void* user, const uint8_t* data,
                                 size_t length);
} h2_alipay_tap_transport_t;

/** Serialized, blocking App operations. get_code/query_code are required.
 * NULL start_tap hides NFC; otherwise supply all NFC operations. sdk_rc keeps
 * the compatible C API result separate from the PAL result. Query the code
 * actually displayed for an attempt. Caller owns and wipes code buffers.
 * start_tap borrows transport until stop_tap succeeds. Each session accepts
 * one NFC input and never repeats a failed or ambiguous physical write.
 * stop_tap detaches borrowed transport without resetting the account.
 * Optional unbind is invoked only after explicit user confirmation and after
 * stop_tap succeeds. OK plus sdk_rc == 0 confirms local binding removal;
 * it does not establish successful remote notification. */
typedef struct h2_alipay_payment_vtable {
  h2_pal_result_t (*get_code)(void* user, uint8_t* code, uint32_t* length,
                              int* sdk_rc);
  h2_pal_result_t (*query_code)(void* user, const char* code,
                                h2_alipay_payment_result_t* out);
  h2_pal_result_t (*start_tap)(void* user,
                               const h2_alipay_tap_transport_t* transport,
                               h2_alipay_payment_result_t* out);
  h2_pal_result_t (*receive_ndef)(void* user, const uint8_t* data,
                                  size_t length,
                                  h2_alipay_payment_result_t* out);
  h2_pal_result_t (*query_tap)(void* user, h2_alipay_payment_result_t* out);
  h2_pal_result_t (*stop_tap)(void* user);
  h2_pal_result_t (*unbind)(void* user, int* sdk_rc);
} h2_alipay_payment_vtable_t;

typedef struct h2_alipay_payment_api {
  void* user;
  const h2_alipay_payment_vtable_t* vtable;
} h2_alipay_payment_api_t;

#ifdef __cplusplus
}
#endif
#endif
