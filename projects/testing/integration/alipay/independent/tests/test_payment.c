#include "h2_alipay_e2e_independent.h"
#include "h2_alipay_test_binding.h"
#include "h2_alipay_api.h"
#include "h2_app_test_mem.h"
#include "h2_app_test_pref.h"
#include "h2_app_test_sync.h"
#include "h2_app_test_time.h"
#include "h2_wolfcrypt_crypto.h"
#include <assert.h>
#include <string.h>

typedef struct fixture {
  unsigned broadcasts, writes, requests;
  int broadcast_rc, write_rc;
  uint32_t entropy;
} fixture_t;

static int entropy(void* user, uint8_t* output, size_t length) {
  fixture_t* fixture = user;
  for (size_t index = 0u; index < length; ++index) {
    fixture->entropy = fixture->entropy * 1664525u + 1013904223u;
    output[index] = (uint8_t)(fixture->entropy >> 24u);
  }
  return 0;
}

static int request(void* user, const h2_pal_http_request_t* request,
                   h2_pal_http_response_t* response) {
  fixture_t* fixture = user;
  (void)request;
  (void)response;
  ++fixture->requests;
  return H2_PAL_ERR_IO;
}

static int broadcast(void* user, uint16_t manufacturer, const uint8_t* data,
                     size_t length, const uint8_t* uuid, size_t uuid_length,
                     bool quick) {
  fixture_t* fixture = user;
  assert(manufacturer == 23u && length == 20u && data[0] == 'N');
  assert(uuid_length == 2u && uuid[0] == 0x3cu && uuid[1] == 0xfeu);
  (void)quick;
  ++fixture->broadcasts;
  return fixture->broadcast_rc;
}

static int write_reply(void* user, const uint8_t* data, size_t length) {
  fixture_t* fixture = user;
  assert(length > 5u && memcmp(data, "NW310", 5u) == 0);
  ++fixture->writes;
  return fixture->write_rc;
}

int main(void) {
  fixture_t fixture = {.entropy = 1u};
  h2_app_test_mem_t memory;
  h2_app_test_mem_init(&memory, NULL);
  h2_app_test_pref_t pref;
  assert(h2_app_test_pref_init(&pref, &memory.api) == 0);
  h2_app_test_sync_t sync;
  h2_app_test_sync_init(&sync);
  h2_app_test_time_t time;
  h2_app_test_time_init(&time, 0u);
  assert(h2_pal_time_set_wall_ms(&time.api, 1794449000000u) == 0);
  const h2_wolfcrypt_crypto_config_t crypto = {&fixture, entropy};
  assert(h2_wolfcrypt_crypto_init(&crypto) == 0);
  const h2_pal_http_vtable_t http_vtable = {.request = request};
  const h2_pal_http_api_t http = {&fixture, &http_vtable};
  const h2_alipay_client_config_t config = {
      .mem = &memory.api,
      .pref = &pref.api,
      .time = &time.api,
      .http = &http,
      .crypto = h2_wolfcrypt_crypto_api(),
      .identity = {"TEST_SN", "TEST_SN", "12:34:56:78:9A:BC", "H106"},
      .company = "HAIVIVI",
      .pref_namespace = "radio_test",
      .product_type = 3u,
      .feature = 0x4009u,
      .compile_timestamp_s = 1794449000u};
  assert(h2_alipay_test_binding_seed(&config) == 0);
  const h2_alipay_payment_api_t* payment =
      h2_alipay_e2e_independent_payment_api();
  const h2_alipay_tap_transport_t direct = {.user = &fixture,
                                            .context = H2_ALIPAY_TAP_AWAKE,
                                            .broadcast_direct = broadcast};
  h2_alipay_payment_result_t result;
  assert(payment->vtable->start_tap(payment->user, &direct, &result) ==
         H2_PAL_ERR_INVALID_STATE);
  assert(h2_alipay_e2e_independent_configure(&config, &sync.api) == 0);
  const h2_alipay_api_t* api = h2_alipay_e2e_independent_api();
  bool first;
  int sdk_rc;
  assert(api->vtable->pre_init(api->user, &sdk_rc, &first) == 0);
  assert(first && sdk_rc == 0);
  static const uint8_t input[] =
      "x?scheme=x%3FpageParams%3D%7B%22k%22%3A%22"
      "0123456789abcdef_SIG_a_b_c_d_3%22%7D";
  uint8_t packet[27];
  assert(h2_alipay_test_binding_result(&config, "0123456789abcdef", 0u,
                                       packet) == 0);
  assert(payment->vtable->start_tap(payment->user, &direct, &result) == 0);
  assert(payment->vtable->receive_ble_result(
             payment->user, packet, 27u, &result) == H2_PAL_ERR_INVALID_STATE);
  assert(payment->vtable->receive_ndef(payment->user, input, sizeof(input) - 1u,
                                       &result) == H2_PAL_ERR_WOULD_BLOCK);
  assert(result.confirmation_required && !result.response_sent &&
         fixture.broadcasts == 0u && fixture.requests == 0u);
  assert(payment->vtable->confirm_tap(payment->user, &result) == 0);
  assert(result.response_sent && result.state == H2_ALIPAY_PAYMENT_PENDING &&
         fixture.broadcasts == 1u);
  assert(payment->vtable->confirm_tap(payment->user, &result) ==
         H2_PAL_ERR_INVALID_STATE);
  assert(fixture.broadcasts == 1u);
  packet[4] ^= 1u;
  assert(payment->vtable->receive_ble_result(payment->user, packet, 27u,
                                             &result) == 0);
  assert(result.sdk_rc != 0 && result.state == H2_ALIPAY_PAYMENT_UNKNOWN);
  packet[4] ^= 1u;
  assert(payment->vtable->receive_ble_result(payment->user, packet, 27u,
                                             &result) == 0);
  assert(result.state == H2_ALIPAY_PAYMENT_PAID &&
         strcmp(result.amount, "0.00") == 0);
  assert(payment->vtable->stop_tap(payment->user) == 0);
  const h2_alipay_tap_transport_t nfc = {.user = &fixture,
                                         .context = H2_ALIPAY_TAP_WALLET,
                                         .write_reply = write_reply,
                                         .broadcast_direct = broadcast};
  fixture.write_rc = H2_PAL_ERR_IO;
  assert(payment->vtable->start_tap(payment->user, &nfc, &result) == 0);
  assert(payment->vtable->receive_ndef(payment->user, input, sizeof(input) - 1u,
                                       &result) == H2_PAL_ERR_IO);
  assert(result.write_attempted && !result.response_sent &&
         !result.confirmation_required && fixture.writes == 1u &&
         fixture.broadcasts == 1u);
  assert(payment->vtable->receive_ndef(payment->user, input, sizeof(input) - 1u,
                                       &result) == H2_PAL_ERR_INVALID_STATE);
  assert(payment->vtable->stop_tap(payment->user) == 0);
  fixture.write_rc = 0;
  assert(payment->vtable->start_tap(payment->user, &nfc, &result) == 0);
  assert(payment->vtable->receive_ndef(payment->user, input, sizeof(input) - 1u,
                                       &result) == 0);
  assert(result.response_sent && !result.confirmation_required &&
         fixture.writes == 2u && fixture.broadcasts == 1u);
  assert(payment->vtable->stop_tap(payment->user) == 0);
  fixture.broadcast_rc = H2_PAL_ERR_IO;
  assert(payment->vtable->start_tap(payment->user, &direct, &result) == 0);
  assert(payment->vtable->receive_ndef(payment->user, input, sizeof(input) - 1u,
                                       &result) == H2_PAL_ERR_WOULD_BLOCK);
  assert(payment->vtable->confirm_tap(payment->user, &result) == H2_PAL_ERR_IO);
  assert(!result.response_sent && fixture.broadcasts == 2u);
  assert(payment->vtable->confirm_tap(payment->user, &result) ==
         H2_PAL_ERR_INVALID_STATE);
  assert(fixture.broadcasts == 2u);
  assert(payment->vtable->stop_tap(payment->user) == 0);
  fixture.broadcast_rc = 0;
  assert(alipay_npay_set_quick_pay_flag(true) == 0);
  assert(alipay_npay_get_quick_pay_flag());
  assert(payment->vtable->start_tap(payment->user, &direct, &result) == 0);
  static const uint8_t quick_input[] =
      "x?scheme=x%3FpageParams%3D%7B%22k%22%3A%22"
      "0123456789abcdef_SIG_a_b_c_d_2%22%7D";
  assert(payment->vtable->receive_ndef(payment->user, quick_input,
                                       sizeof(quick_input) - 1u, &result) == 0);
  assert(result.response_sent && !result.confirmation_required &&
         fixture.broadcasts == 3u);
  assert(payment->vtable->stop_tap(payment->user) == 0);
  const h2_alipay_tap_transport_t nfc_only = {
      .user = &fixture,
      .context = H2_ALIPAY_TAP_WALLET,
      .write_reply = write_reply};
  assert(payment->vtable->start_tap(payment->user, &nfc_only, &result) == 0);
  assert(payment->vtable->receive_ndef(payment->user, input, sizeof(input) - 1u,
                                       &result) == 0);
  assert(result.write_attempted && result.response_sent &&
         !result.broadcast_attempted && !result.confirmation_required &&
         fixture.writes == 3u && fixture.broadcasts == 3u);
  assert(payment->vtable->stop_tap(payment->user) == 0);
  fixture.write_rc = H2_PAL_ERR_IO;
  assert(payment->vtable->start_tap(payment->user, &nfc_only, &result) == 0);
  assert(payment->vtable->receive_ndef(payment->user, input, sizeof(input) - 1u,
                                       &result) == H2_PAL_ERR_IO);
  assert(result.write_attempted && !result.response_sent &&
         !result.broadcast_attempted && !result.confirmation_required &&
         fixture.writes == 4u && fixture.broadcasts == 3u);
  assert(h2_alipay_e2e_independent_release() == 0);
  assert(payment->vtable->receive_ble_result(
             payment->user, packet, 27u, &result) == H2_PAL_ERR_INVALID_STATE);
  assert(pref.active_handles == 0u);
  assert(h2_app_test_pref_deinit(&pref) == 0 && memory.live_blocks == 0u);
  assert(fixture.requests == 0u);
  h2_wolfcrypt_crypto_deinit();
  return 0;
}
