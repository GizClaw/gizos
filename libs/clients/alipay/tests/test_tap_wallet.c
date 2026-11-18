#include "h2_alipay_api.h"
#include "h2_alipay_context.h"
#include "h2_alipay_client_internal.h"
#include "h2_app_test_mem.h"
#include "h2_app_test_pref.h"
#include "h2_app_test_sync.h"
#include "h2_app_test_time.h"
#include "h2_wolfcrypt_crypto.h"

#include <assert.h>
#include <string.h>

typedef struct observation {
  unsigned sends, statuses;
  int callback_result;
  const char* expected_reply;
  alipay_nfc_pay_status_t expected_status;
  alipay_nfc_pay_error_t expected_error;
} observation_t;

static observation_t observation;
static const uint8_t standard_tag[] =
    "x?scheme=x%3FpageParams%3D%7B%22k%22%3A%22"
    "0123456789abcdef_SIG_a_b_c_d_0%22%7D";
static const uint8_t vip_tag[] =
    "x?scheme=x%3Fs%3Dvip%26v%3Da~b~c~"
    "0123456789abcdef_SIG_a_b_c_d_0~e";
static const char standard_reply[] = "NW310bNpvwudy1Gy9zE7Cu5/Rc2rT_8z9o_0117";
static const char vip_reply[] = "MP310bNpvwudy1Gy9zE7Cu5/Rc2rT_8z9o_0117";

static int send_reply(const uint8_t* data, uint32_t length) {
  assert(observation.expected_reply != NULL);
  assert(observation.sends == 0u && observation.statuses == 0u);
  assert(length == strlen(observation.expected_reply));
  assert(memcmp(data, observation.expected_reply, length) == 0);
  ++observation.sends;
  h2_alipay_client_payment_t diagnostics;
  assert(h2_alipay_independent_last_payment_result(&diagnostics) == 0);
  return observation.callback_result;
}

static int receive_status(alipay_payment_status_t* status) {
  assert(observation.statuses == 0u);
  assert(observation.sends == (observation.expected_reply != NULL ? 1u : 0u));
  assert(status->status == observation.expected_status);
  assert(status->code == observation.expected_error);
  for (size_t index = 0u; index < sizeof(status->amount); ++index)
    assert(status->amount[index] == '\0');
  ++observation.statuses;
  h2_alipay_independent_binding_result_t diagnostics;
  assert(h2_alipay_independent_last_binding_result(&diagnostics) == 0);
  return observation.callback_result;
}

static int unexpected_request(void* user, const h2_pal_http_request_t* request,
                              h2_pal_http_response_t* response) {
  (void)user;
  (void)request;
  (void)response;
  assert(false);
  return H2_PAL_ERR_IO;
}

static void free_response(void* user, h2_pal_http_response_t* response) {
  (void)user;
  h2_pal_http_response_reset(response);
}

static int random_bytes(void* user, uint8_t* data, size_t length) {
  (void)user;
  memset(data, 0x42, length);
  return H2_PAL_OK;
}

static void seed_binding(const h2_alipay_client_config_t* config) {
  binding_record_t binding = {
      .phase = BINDING_COMPLETE,
      .tid = "TEST_TID",
      .uid = "2088101234567890",
      .seed = {0xf8, 0x24, 0xb2, 0x77, 0xd5, 0x74, 0xe8, 0x13, 0x73, 0x2f,
               0x41, 0xba, 0x40, 0x6c, 0xc9, 0x5c, 0x12, 0x17, 0x81, 0x82,
               0x88, 0x7e, 0xeb, 0x2d, 0x19, 0xbd, 0x51, 0xf7, 0x54, 0x79,
               0x18, 0xb7, 0xbe, 0xc1, 0x53, 0x30, 0xa0, 0x22, 0xea, 0xf7,
               0x6f, 0x1c, 0xe3, 0xdb, 0x69, 0x2d, 0xd3, 0xdf, 0x13, 0x6b,
               0x9d, 0xbe, 0xca, 0x79, 0xc0, 0xe1, 0xfd, 0x33, 0xfe, 0xaf,
               0x17, 0x74, 0x1d, 0xf5, 0x70, 0xb0, 0xa0, 0x70, 0xdb, 0x05,
               0x0a, 0xaf, 0xef, 0xa4, 0x48, 0xbc, 0xb5, 0x51, 0x9c, 0x75},
      .seed_length = 80u};
  for (size_t index = 0u; index < sizeof(binding.secret); ++index)
    binding.secret[index] = (uint8_t)index;
  assert(h2_alipay_client_store_binding(config, &binding) == 0);
  h2_alipay_client_wipe(&binding, sizeof(binding));
}

static void expect(const char* reply, alipay_nfc_pay_status_t status,
                   alipay_nfc_pay_error_t error, int callback_result) {
  observation = (observation_t){.expected_reply = reply,
                                .expected_status = status,
                                .expected_error = error,
                                .callback_result = callback_result};
}

static void wallet_replies(void) {
  for (unsigned callback = 0u; callback < 2u; ++callback) {
    int callback_result = callback == 0u ? 0 : 17;
    expect(standard_reply, NFC_PAYMENT_STATUS_SUCCESS,
           NFC_PAYMENT_FAIL_ERROR_SUCCESS, callback_result);
    assert(alipay_nfc_recv_data_handle(standard_tag,
                                       sizeof(standard_tag) - 1u) == 0);
    assert(observation.sends == 1u && observation.statuses == 1u);

    uint8_t amount[16];
    memset(amount, 0xa5, sizeof(amount));
    expect(standard_reply, NFC_PAYMENT_STATUS_SUCCESS,
           NFC_PAYMENT_FAIL_ERROR_SUCCESS, callback_result);
    assert(alipay_nfc_recv_data_handle_with_context(
               NFC_PAY_READER_CONTEXT_WITHIN_ALIPAY_WALLET, standard_tag,
               sizeof(standard_tag) - 1u, amount) == 0);
    assert(observation.sends == 1u && observation.statuses == 1u);
    for (size_t index = 0u; index < sizeof(amount); ++index)
      assert(amount[index] == 0u);

    uint8_t ndef[sizeof(standard_tag) + 4u] = {0xd1u, 1u, sizeof(standard_tag),
                                               'U', 4u};
    memcpy(ndef + 5u, standard_tag, sizeof(standard_tag) - 1u);
    expect(standard_reply, NFC_PAYMENT_STATUS_SUCCESS,
           NFC_PAYMENT_FAIL_ERROR_SUCCESS, callback_result);
    assert(alipay_nfc_recv_data_handle(ndef, sizeof(ndef)) == 0);
    assert(observation.sends == 1u && observation.statuses == 1u);

    expect(vip_reply, NFC_PAYMENT_STATUS_SUCCESS,
           NFC_PAYMENT_FAIL_ERROR_SUCCESS, callback_result);
    assert(alipay_nfc_recv_data_handle(vip_tag, sizeof(vip_tag) - 1u) == 0);
    assert(observation.sends == 1u && observation.statuses == 1u);
  }
}

static void contextual_boundary(void) {
  static const alipay_nfc_pay_reader_context_t contexts[] = {
      NFC_PAY_READER_CONTEXT_AWAKE,
      NFC_PAY_READER_CONTEXT_SLEEP,
      NFC_PAY_READER_CONTEXT_LOCK_SCREEN,
      NFC_PAY_READER_CONTEXT_OTHER,
  };
  for (size_t index = 0u; index < sizeof(contexts) / sizeof(*contexts);
       ++index) {
    uint8_t amount[16];
    memset(amount, 0xa5, sizeof(amount));
    expect(NULL, NFC_PAYMENT_STATUS_SUCCESS, NFC_PAYMENT_FAIL_ERROR_SUCCESS, 0);
    assert(alipay_nfc_recv_data_handle_with_context(
               contexts[index], standard_tag, sizeof(standard_tag) - 1u,
               amount) == 0);
    assert(observation.sends == 0u && observation.statuses == 0u);
    assert(memcmp(amount, "SIG", 3u) == 0 && amount[3] == 0u);
  }
  expect(standard_reply, NFC_PAYMENT_STATUS_SUCCESS,
         NFC_PAYMENT_FAIL_ERROR_SUCCESS, 0);
  assert(alipay_nfc_recv_data_handle(standard_tag, sizeof(standard_tag) - 1u) ==
         0);
  assert(observation.sends == 1u && observation.statuses == 1u);
}

static void rejected_tags(void) {
  static const uint8_t collection[] = "x?scheme=x%3FcodeContent%3DTEST0";
  static const uint8_t unknown[] = "x?other=TEST0";
  expect(NULL, NFC_PAYMENT_STATUS_FAIL,
         NFC_PAYMENT_FAIL_ERROR_UNSUPPORT_WITHOUT_NETWORK, 17);
  assert(alipay_nfc_recv_data_handle(collection, sizeof(collection) - 1u) ==
         99);
  assert(observation.sends == 0u && observation.statuses == 1u);
  expect(NULL, NFC_PAYMENT_STATUS_FAIL, NFC_PAYMENT_FAIL_ERROR_UNKNOWN_TAG, 0);
  assert(alipay_nfc_recv_data_handle(unknown, sizeof(unknown) - 1u) == 99);
  assert(observation.sends == 0u && observation.statuses == 1u);
}

int main(void) {
  h2_app_test_mem_t memory;
  h2_app_test_mem_init(&memory, NULL);
  h2_app_test_pref_t pref;
  assert(h2_app_test_pref_init(&pref, &memory.api) == 0);
  h2_app_test_sync_t sync;
  h2_app_test_sync_init(&sync);
  h2_app_test_time_t time;
  h2_app_test_time_init(&time, 0u);
  assert(h2_pal_time_set_wall_ms(&time.api, 1794449000000u) == 0);
  const h2_wolfcrypt_crypto_config_t crypto = {
      .entropy = random_bytes,
  };
  assert(h2_wolfcrypt_crypto_init(&crypto) == 0);
  const h2_pal_http_vtable_t http_vtable = {.request = unexpected_request,
                                            .response_free = free_response};
  const h2_pal_http_api_t http = {.vtable = &http_vtable};
  const h2_alipay_client_config_t config = {
      .mem = &memory.api,
      .pref = &pref.api,
      .time = &time.api,
      .http = &http,
      .gateway_url = "https://iotapi.alipay.com/mgw.htm",
      .crypto = h2_wolfcrypt_crypto_api(),
      .identity = {"TEST_SN", "TEST_SN", "12:34:56:78:9A:BC", "H106"},
      .company = "HAIVIVI",
      .pref_namespace = "tap_wallet",
      .product_type = 3u,
      .feature = 16385u,
      .compile_timestamp_s = 1794449000u};
  const alipay_nfc_payment_callback_t callbacks = {
      .send_data_handle = send_reply,
      .notify_payment_result_handle = receive_status};
  alipay_nfc_init_callbacks(callbacks);
  assert(h2_alipay_independent_configure(&config, &sync.api) == 0);
  assert(alipay_pre_init() == 0);
  expect(NULL, NFC_PAYMENT_STATUS_FAIL, NFC_PAYMENT_FAIL_ERROR_NOT_BIND, 0);
  assert(alipay_nfc_recv_data_handle(standard_tag, sizeof(standard_tag) - 1u) ==
         99);
  assert(observation.sends == 0u && observation.statuses == 1u);
  seed_binding(&config);
  wallet_replies();
  contextual_boundary();
  rejected_tags();
  assert(h2_alipay_independent_release() == 0);
  assert(pref.active_handles == 0u);
  assert(h2_app_test_pref_deinit(&pref) == 0);
  assert(memory.live_blocks == 0u);
  h2_wolfcrypt_crypto_deinit();
  return 0;
}
