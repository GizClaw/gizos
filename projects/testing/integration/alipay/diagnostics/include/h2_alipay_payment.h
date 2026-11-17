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

/** Non-secret diagnostics. An SDK processing callback is independent of the
 * transaction state: PAID requires an actual successful result query or a
 * current-token BLE result validated by the SDK. */
typedef struct h2_alipay_payment_result {
  h2_alipay_payment_state_t state;
  int sdk_rc, sdk_status;
  int transport_rc, processing_status, processing_error;
  int nfc_write_rc, ble_send_rc;
  bool write_attempted, broadcast_attempted;
  bool response_sent;
  bool confirmation_required;
  char amount[32];
} h2_alipay_payment_result_t;

typedef enum h2_alipay_tap_context {
  H2_ALIPAY_TAP_WALLET = 0,
  H2_ALIPAY_TAP_AWAKE,
  H2_ALIPAY_TAP_SLEEP,
  H2_ALIPAY_TAP_LOCK_SCREEN,
  H2_ALIPAY_TAP_OTHER,
} h2_alipay_tap_context_t;

/** Borrowed synchronous transport. Called on the SDK worker, never UI/ISR.
 * Must finish a real write/readback before returning OK. Reply bytes are Alipay
 * application data, not a complete NDEF message. They must be encoded according
 * to the terminal transport contract and are borrowed for this call only. No
 * SDK call is allowed inside this callback. */
typedef struct h2_alipay_tap_transport {
  void* user;
  h2_pal_result_t (*write_reply)(void* user, const uint8_t* data,
                                 size_t length);
  /** Start the requested direct advertising data. SDK buffers are borrowed
   * for this call only; copy them before returning. OK means radio submission,
   * not terminal reception or financial success. */
  h2_pal_result_t (*broadcast_direct)(void* user, uint16_t manufacturer,
                                      const uint8_t* data, size_t length,
                                      const uint8_t* service_uuid,
                                      size_t service_uuid_length,
                                      bool quick_pay);
  /** Legacy encoded advertising and connected GATT replies. notify copies or
   * submits the borrowed fragment before returning; success is not an ACK.
   * The bridge retains the final fragment until confirm_tap. */
  h2_pal_result_t (*broadcast_legacy)(void* user, const uint8_t* data,
                                      size_t length);
  h2_pal_result_t (*notify)(void* user, bool first, bool finish,
                            const uint8_t* data, size_t length);
  h2_alipay_tap_context_t context;
} h2_alipay_tap_transport_t;

/** Blocking serialized operations for business workers. get_code and
 * query_code are required for a payer-code API. A NULL start_tap means no tap
 * capability: the App hides that entry and the final target does not start NFC.
 * When start_tap is provided the remaining tap callbacks must also be provided.
 * sdk_rc preserves the
 * original return independently of the PAL call result. Code buffers remain
 * private to the App and must be wiped on expiry/exit. Query uses the code
 * actually displayed for that attempt, even after the visible QR changes.
 * start_tap borrows transport until stop_tap completes; receive_ndef can send
 * a payment response and is called only for a user-authorized tap session.
 * stop_tap detaches the transport; it never unbinds/resets the SDK. */
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
  /** Release a pending non-quick BLE response only after an explicit App
   * confirmation for this attempt. Does not regenerate or resubmit an RPC. */
  h2_pal_result_t (*confirm_tap)(void* user, h2_alipay_payment_result_t* out);
  /** SDK-validated current-attempt BLE result, exact 27-byte service payload.
   * Caller filters the SDK result UUID and requires this attempt's NFC input
   * and actual response submission before requesting parsing. */
  h2_pal_result_t (*receive_ble_result)(void* user, const uint8_t* data,
                                        size_t length,
                                        h2_alipay_payment_result_t* out);
  h2_pal_result_t (*stop_tap)(void* user);
  /** Feed one terminal GATT write after this attempt's NFC input.
   * att_mtu is the negotiated ATT MTU, at least 23. Fragments are serialized
   * with NFC, confirmation and result operations on the business worker. */
  h2_pal_result_t (*receive_ble_frame)(void* user, const uint8_t* data,
                                       size_t length, uint16_t att_mtu,
                                       h2_alipay_payment_result_t* out);
} h2_alipay_payment_vtable_t;

typedef struct h2_alipay_payment_api {
  void* user;
  const h2_alipay_payment_vtable_t* vtable;
} h2_alipay_payment_api_t;

#ifdef __cplusplus
}
#endif
#endif
