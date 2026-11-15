#include "h2_alipay_sdk.h"
#include "h2_alipay_sdk_internal.h"
#include "alipay_pay.h"
#include "alipay_pay_result.h"
#include "alipay_nfc_offline_pay.h"
#include <assert.h>
#include <string.h>

/* Synthetic payer values and SDK callbacks only; never contacts a service. */
static alipay_nfc_payment_callback_t callbacks;
static unsigned writes, locked, queries;
static int sdk_result, write_result, init_result;
static unsigned broadcasts;
static bool direct_mode;
static uint8_t result_type = 1u;
static alipay_payment_status_e sdk_status = ALIPAY_PAYMENT_STATUS_NOTHING;
static const char code[] = "123456789012345678";
static const uint8_t reply[] = {'M', 'P', '3', '1', '0', 'X'};
static uint32_t returned_length = 18u;
int h2_alipay_sdk_call_lock(void) {
  assert(!locked);
  ++locked;
  return 0;
}
int h2_alipay_sdk_call_unlock(int rc) {
  assert(locked);
  --locked;
  return rc;
}
uint64_t h2_alipay_sdk_now(void) { return 1u; }
void h2_alipay_sdk_trace(const char* a, const char* b, int c, uint32_t d,
                         uint32_t e, uint64_t f, const char* g,
                         const void* bytes, size_t length) {
  (void)a;
  (void)b;
  (void)c;
  (void)d;
  (void)e;
  (void)f;
  (void)g;
  assert(bytes == NULL && length == 0u);
}
retval_e alipay_get_paycode(uint8_t* out, uint32_t* length) {
  assert(locked && *length >= 20u);
  memcpy(out, code, sizeof(code));
  *length = returned_length;
  return sdk_result;
}
retval_e alipay_query_order_result_string(char* requested,
                                          alipay_payment_status_e* status,
                                          char amount[32], char* ext,
                                          uint32_t* ext_len) {
  assert(locked && strcmp(requested, code) == 0);
  (void)ext;
  (void)ext_len;
  ++queries;
  *status = sdk_status;
  memcpy(amount, "0.00", 5u);
  return sdk_result;
}
void alipay_nfc_init_callbacks(alipay_nfc_payment_callback_t value) {
  assert(locked);
  callbacks = value;
  assert(value.send_data_handle && value.ble_broadcast_data_handle &&
         value.ble_notify_data_handle && value.notify_payment_result_handle &&
         value.ble_broadcast_direct_pay_data_handle &&
         value.quick_pay_flag_change_callback);
}
int alipay_nfc_recv_data_handle(const uint8_t* data, uint32_t length) {
  assert(locked && data != NULL && length > 0u);
  const uint8_t uuid[] = "0000fe3c-0000-1000-8000-00805f9b34fb";
  int sent = direct_mode ? callbacks.ble_broadcast_direct_pay_data_handle(
                               0x1234, reply, sizeof(reply), uuid,
                               sizeof(uuid) - 1u, 0u)
                         : callbacks.send_data_handle(reply, sizeof(reply));
  /* Deliberately report internal success despite failed transmission. */
  alipay_payment_status_t status = {.status = NFC_PAYMENT_STATUS_SUCCESS,
                                    .code = NFC_PAYMENT_FAIL_ERROR_SUCCESS};
  assert(callbacks.notify_payment_result_handle(&status) == 0);
  (void)sent;
  return sdk_result;
}
retval_e alipay_query_last_nfc_pay_result(alipay_payment_status_e* status,
                                          char amount[32], char* ext,
                                          uint32_t* ext_len) {
  assert(locked);
  (void)ext;
  (void)ext_len;
  *status = sdk_status;
  memcpy(amount, "0.00", 5u);
  ++queries;
  return sdk_result;
}
retval_e alipay_npay_ble_broadcast_init(void) { return init_result; }
retval_e alipay_npay_ble_scan_pay_res_data_handle_v2(
    const uint8_t* data, uint32_t length, alipay_npay_ble_pay_result_t* out) {
  assert(locked && data != NULL && length == 27u);
  *out = (alipay_npay_ble_pay_result_t){.pay_result_type = result_type,
                                        .real_pay_amount_fen = 0u};
  return sdk_result;
}
static int write_reply(void* user, const uint8_t* data, size_t length) {
  assert(user == &writes && locked);
  assert(length == sizeof(reply) && memcmp(data, reply, length) == 0);
  ++writes;
  return write_result;
}
static int broadcast(void* user, uint16_t manufacturer, const uint8_t* data,
                     size_t length, const uint8_t* uuid, size_t uuid_length,
                     bool quick) {
  assert(user == &broadcasts && locked && manufacturer == 0x1234u && !quick);
  assert(length == sizeof(reply) && memcmp(data, reply, length) == 0);
  assert(uuid_length == 2u && uuid[0] == 0x3cu && uuid[1] == 0xfeu);
  ++broadcasts;
  return 0;
}
int main(void) {
  const h2_alipay_payment_api_t* api = h2_alipay_sdk_payment_api();
  assert(api != NULL && api->vtable != NULL);
  uint8_t output[32];
  uint32_t length = sizeof(output);
  int raw;
  assert(api->vtable->get_code(api->user, output, &length, &raw) == 0);
  assert(raw == 0 && length == 18u && strcmp((char*)output, code) == 0);
  sdk_result = 99;
  length = sizeof(output);
  assert(api->vtable->get_code(api->user, output, &length, &raw) == 0 &&
         raw == 99);
  assert(length == 0u && memcmp(output, (uint8_t[32]){0}, 32u) == 0);
  sdk_result = 0;
  returned_length = 20u;
  length = sizeof(output);
  assert(api->vtable->get_code(api->user, output, &length, &raw) ==
         H2_PAL_ERR_FORMAT);
  assert(length == 0u && output[0] == 0u);
  returned_length = 18u;
  h2_alipay_payment_result_t result;
  assert(api->vtable->query_code(api->user, code, &result) == 0);
  assert(result.state == H2_ALIPAY_PAYMENT_PENDING && result.amount[0] == 0);
  sdk_status = ALIPAY_PAYMENT_STATUS_SUCCESS;
  sdk_result = 7;
  assert(api->vtable->query_code(api->user, code, &result) == 0);
  assert(result.state == H2_ALIPAY_PAYMENT_UNKNOWN && result.amount[0] == 0);
  sdk_result = 0;
  assert(api->vtable->query_code(api->user, code, &result) == 0);
  assert(result.state == H2_ALIPAY_PAYMENT_PAID &&
         strcmp(result.amount, "0.00") == 0);
  const h2_alipay_tap_transport_t transport = {.user = &writes,
                                               .write_reply = write_reply};
  assert(api->vtable->start_tap(api->user, &transport, &result) == 0);
  write_result = H2_PAL_ERR_IO;
  assert(api->vtable->receive_ndef(api->user, reply, sizeof(reply), &result) ==
         H2_PAL_ERR_IO);
  assert(result.processing_status == NFC_PAYMENT_STATUS_SUCCESS &&
         !result.response_sent && result.state != H2_ALIPAY_PAYMENT_PAID);
  unsigned prior = queries;
  assert(api->vtable->query_tap(api->user, &result) ==
         H2_PAL_ERR_INVALID_STATE);
  assert(queries == prior);
  write_result = 0;
  assert(api->vtable->receive_ndef(api->user, reply, sizeof(reply), &result) ==
         0);
  assert(result.response_sent && result.state == H2_ALIPAY_PAYMENT_PENDING);
  assert(api->vtable->query_tap(api->user, &result) == 0);
  assert(result.state == H2_ALIPAY_PAYMENT_PAID &&
         strcmp(result.amount, "0.00") == 0);
  assert(api->vtable->stop_tap(api->user) == 0);
  assert(callbacks.send_data_handle(reply, sizeof(reply)) == H2_PAL_ERR_CLOSED);
  assert(!locked && writes == 2u);
  direct_mode = true;
  const h2_alipay_tap_transport_t ble = {.user = &broadcasts,
                                         .broadcast_direct = broadcast};
  assert(api->vtable->start_tap(api->user, &ble, &result) == 0);
  assert(api->vtable->start_tap(api->user, &ble, &result) ==
         H2_PAL_ERR_INVALID_STATE);
  assert(api->vtable->receive_ndef(api->user, reply, sizeof(reply), &result) ==
         H2_PAL_ERR_WOULD_BLOCK);
  assert(result.confirmation_required && !result.response_sent &&
         broadcasts == 0u);
  assert(result.state != H2_ALIPAY_PAYMENT_PAID);
  assert(api->vtable->confirm_tap(api->user, &result) == 0);
  assert(result.response_sent && !result.confirmation_required &&
         broadcasts == 1u);
  assert(api->vtable->confirm_tap(api->user, &result) ==
         H2_PAL_ERR_INVALID_STATE);
  assert(broadcasts == 1u);
  uint8_t packet[27] = {0};
  sdk_result = 99;
  assert(api->vtable->receive_ble_result(api->user, packet, sizeof(packet),
                                         &result) == 0);
  assert(result.state == H2_ALIPAY_PAYMENT_UNKNOWN);
  sdk_result = 0;
  result_type = 0u;
  assert(api->vtable->receive_ble_result(api->user, packet, sizeof(packet),
                                         &result) == 0);
  assert(result.state == H2_ALIPAY_PAYMENT_PAID && result.amount[0] == 0);
  result_type = 1u;
  assert(api->vtable->receive_ble_result(api->user, packet, sizeof(packet),
                                         &result) == 0);
  assert(result.state == H2_ALIPAY_PAYMENT_PAID &&
         strcmp(result.amount, "0.00") == 0);
  assert(api->vtable->stop_tap(api->user) == 0);
  init_result = 99;
  assert(api->vtable->start_tap(api->user, &ble, &result) == H2_PAL_ERR_IO &&
         result.sdk_rc == 99);
  assert(!locked);
  return 0;
}
