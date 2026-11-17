#include "h2_alipay_context.h"
#include "h2_alipay_client_internal.h"
#include "h2_alipay_tap_internal.h"
#include "h2_app_test_mem.h"
#include "h2_app_test_pref.h"
#include "h2_app_test_sync.h"
#include "h2_app_test_time.h"
#include "h2_wolfcrypt_crypto.h"
#include "h2_wolfcrypt_test_crypto.h"
#include "h2_sha256.h"
#include "h2_alipay_api.h"
#include "yyjson.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct fixture {
  h2_app_test_mem_t memory;
  h2_app_test_pref_t pref;
  h2_app_test_sync_t sync;
  h2_app_test_time_t time;
  h2_alipay_client_config_t config;
  h2_pal_crypto_vtable_t crypto_vtable;
  h2_pal_crypto_api_t crypto;
  char response[1024], query[512], ack[512];
  char prepare[4096], binding_query[1024], finish[1024];
  bool binding_mode;
  bool unbind_mode, network_error;
  bool remote_mode;
  bool income_mode;
  bool scan_mode;
  bool provision_mode;
  unsigned provision_requests;
  char provision_operation[160], provision_body[4096];
  unsigned scan_requests;
  char scan_operation[160];
  unsigned acknowledgements;
  unsigned unbind_requests;
  unsigned remote_requests;
  char remote_query[1024];
  uint32_t entropy;
  yyjson_val* union_script;
  size_t union_step;
  alipay_union_code_order_info_t union_order;
} fixture_t;

static h2_pal_p256_private_key_t controlled_private_key;
static h2_pal_p256_private_key_t migration_private_keys[2];
static bool migration_key_mode;
static unsigned migration_key_step;
static unsigned quick_callbacks;
static bool quick_last_flag;
static yyjson_val* receive_events;
static size_t receive_event_index;
static int receive_callback_result;
static uint8_t frame_expected[512];
static size_t frame_length, frame_offset, frame_packet;
static uint32_t frame_mtu;
static unsigned certificate_fault_points;
static size_t decode(yyjson_val* root, const char* name, void* output,
                     size_t capacity);
static void match(yyjson_val* root, const char* name, const void* data,
                  size_t length);

static int entropy(void* user, uint8_t* output, size_t length) {
  uint32_t* state = user;
  for (size_t index = 0u; index < length; ++index) {
    *state = *state * 1664525u + 1013904223u;
    output[index] = (uint8_t)(*state >> 24u);
  }
  return H2_PAL_OK;
}

static int request(void* user, const h2_pal_http_request_t* input,
                   h2_pal_http_response_t* output) {
  fixture_t* fixture = user;
  if (fixture->provision_mode) {
    ++fixture->provision_requests;
    bool found = false;
    for (size_t index = 0u; index < input->header_count; ++index) {
      const h2_pal_http_header_t* header = &input->headers[index];
      if (header->name.len != 14u ||
          memcmp(header->name.data, "Operation-Type", 14u) != 0)
        continue;
      assert(header->value.len < sizeof(fixture->provision_operation));
      memcpy(fixture->provision_operation, header->value.data,
             header->value.len);
      fixture->provision_operation[header->value.len] = '\0';
      found = true;
    }
    assert(found && strcmp(fixture->provision_operation,
                           "alipay.mobile.alideviceinfo.device.active") == 0);
    assert(input->body_len < sizeof(fixture->provision_body));
    memcpy(fixture->provision_body, input->body, input->body_len);
    fixture->provision_body[input->body_len] = '\0';
    if (fixture->network_error) return H2_PAL_ERR_IO;
    size_t length = strlen(fixture->response);
    assert(length <= input->response_buf_cap);
    memcpy(input->response_buf, fixture->response, length);
    output->body = input->response_buf;
    output->body_len = length;
    output->status_code = 200;
    return h2_pal_http_deliver_response_header(input, "Result-Status", 13u,
                                               "1000", 4u);
  }
  if (fixture->union_script != NULL) {
    yyjson_val* step =
        yyjson_arr_get(fixture->union_script, fixture->union_step++);
    assert(step != NULL);
    char operation[160];
    size_t size = decode(step, "operation", operation, sizeof(operation) - 1u);
    operation[size] = '\0';
    bool seen = false;
    for (size_t index = 0u; index < input->header_count; ++index)
      if (input->headers[index].name.len == 14u &&
          memcmp(input->headers[index].name.data, "Operation-Type", 14u) == 0)
        seen = input->headers[index].value.len == size &&
               memcmp(input->headers[index].value.data, operation, size) == 0;
    assert(seen);
    match(step, "query", input->body, input->body_len);
    if (yyjson_is_true(yyjson_obj_get(step, "network_error")))
      return H2_PAL_ERR_IO;
    size =
        decode(step, "response", input->response_buf, input->response_buf_cap);
    output->body = input->response_buf;
    output->body_len = size;
    output->status_code = 200;
    return h2_pal_http_deliver_response_header(input, "Result-Status", 13u,
                                               "1000", 4u);
  }
  if (fixture->scan_mode) {
    bool seen = false;
    for (size_t index = 0u; index < input->header_count; ++index)
      if (input->headers[index].name.len == 14u &&
          memcmp(input->headers[index].name.data, "Operation-Type", 14u) == 0)
        seen = input->headers[index].value.len ==
                   strlen(fixture->scan_operation) &&
               memcmp(input->headers[index].value.data, fixture->scan_operation,
                      input->headers[index].value.len) == 0;
    assert(seen && input->body_len < sizeof(fixture->query));
    memcpy(fixture->query, input->body, input->body_len);
    fixture->query[input->body_len] = '\0';
    ++fixture->scan_requests;
    if (fixture->network_error) return H2_PAL_ERR_IO;
    size_t length = strlen(fixture->response);
    assert(length <= input->response_buf_cap);
    memcpy(input->response_buf, fixture->response, length);
    output->body = input->response_buf;
    output->body_len = length;
    output->status_code = 200;
    return h2_pal_http_deliver_response_header(input, "Result-Status", 13u,
                                               "1000", 4u);
  }
  if (fixture->income_mode) {
    static const char operation[] =
        "alipay.mobile.alideviceinfo.changecode.queryIncomeDetailByCode";
    bool seen = false;
    for (size_t index = 0u; index < input->header_count; ++index)
      if (input->headers[index].name.len == 14u &&
          memcmp(input->headers[index].name.data, "Operation-Type", 14u) == 0)
        seen = input->headers[index].value.len == sizeof(operation) - 1u &&
               memcmp(input->headers[index].value.data, operation,
                      sizeof(operation) - 1u) == 0;
    assert(seen && input->body_len < sizeof(fixture->query));
    memcpy(fixture->query, input->body, input->body_len);
    fixture->query[input->body_len] = '\0';
    if (fixture->network_error) return H2_PAL_ERR_IO;
    size_t length = strlen(fixture->response);
    assert(length <= input->response_buf_cap);
    memcpy(input->response_buf, fixture->response, length);
    output->body = input->response_buf;
    output->body_len = length;
    output->status_code = 200;
    return h2_pal_http_deliver_response_header(input, "Result-Status", 13u,
                                               "1000", 4u);
  }
  if (fixture->remote_mode) {
    static const char operation[] =
        "alipay.mobile.alideviceinfo.changecode.device.queryDeviceInfo";
    bool seen = false;
    for (size_t index = 0u; index < input->header_count; ++index)
      if (input->headers[index].name.len == 14u &&
          memcmp(input->headers[index].name.data, "Operation-Type", 14u) == 0)
        seen = input->headers[index].value.len == sizeof(operation) - 1u &&
               memcmp(input->headers[index].value.data, operation,
                      sizeof(operation) - 1u) == 0;
    if (seen) {
      assert(input->body_len < sizeof(fixture->remote_query));
      memcpy(fixture->remote_query, input->body, input->body_len);
      fixture->remote_query[input->body_len] = '\0';
      ++fixture->remote_requests;
      if (fixture->network_error) return H2_PAL_ERR_IO;
      size_t length = strlen(fixture->response);
      assert(length <= input->response_buf_cap);
      memcpy(input->response_buf, fixture->response, length);
      output->body = input->response_buf;
      output->body_len = length;
      output->status_code = 200;
      return h2_pal_http_deliver_response_header(input, "Result-Status", 13u,
                                                 "1000", 4u);
    }
  }
  if (fixture->unbind_mode) {
    static const char operation[] =
        "alipay.mobile.alideviceinfo.changecode.unbind";
    bool seen = false;
    for (size_t index = 0u; index < input->header_count; ++index)
      if (input->headers[index].name.len == 14u &&
          memcmp(input->headers[index].name.data, "Operation-Type", 14u) == 0)
        seen = input->headers[index].value.len == sizeof(operation) - 1u &&
               memcmp(input->headers[index].value.data, operation,
                      sizeof(operation) - 1u) == 0;
    assert(seen && input->body_len < sizeof(fixture->query));
    memcpy(fixture->query, input->body, input->body_len);
    fixture->query[input->body_len] = '\0';
    ++fixture->unbind_requests;
    if (fixture->network_error) return H2_PAL_ERR_IO;
    static const char response[] = "{\"success\":true}";
    assert(input->response_buf_cap >= sizeof(response));
    memcpy(input->response_buf, response, sizeof(response));
    output->body = input->response_buf;
    output->body_len = sizeof(response) - 1u;
    output->status_code = 200;
    return h2_pal_http_deliver_response_header(input, "Result-Status", 13u,
                                               "1000", 4u);
  }
  bool acknowledged = false;
  bool operation_seen = false;
  char* binding_body = NULL;
  const char* binding_response = NULL;
  size_t binding_capacity = 0u;
  for (size_t index = 0u; index < input->header_count; ++index) {
    const h2_pal_http_header_t* header = &input->headers[index];
    if (header->name.len == 14u &&
        memcmp(header->name.data, "Operation-Type", 14u) == 0) {
      if (fixture->binding_mode) {
        static const char prepare[] =
            "alipay.mobile.alideviceinfo.changecode.prepareBind";
        static const char lookup[] =
            "alipay.mobile.alideviceinfo.changecode.queryBindInfo";
        static const char finish[] =
            "alipay.mobile.alideviceinfo.changecode.finishBind";
        if (header->value.len == sizeof(prepare) - 1u &&
            memcmp(header->value.data, prepare, sizeof(prepare) - 1u) == 0) {
          binding_body = fixture->prepare;
          binding_capacity = sizeof(fixture->prepare);
          binding_response =
              "{\"success\":true,\"token\":\"SYNTHETIC_BINDING_TOKEN\"}";
        } else if (header->value.len == sizeof(lookup) - 1u &&
                   memcmp(header->value.data, lookup, sizeof(lookup) - 1u) ==
                       0) {
          binding_body = fixture->binding_query;
          binding_capacity = sizeof(fixture->binding_query);
          binding_response = fixture->response;
        } else {
          assert(header->value.len == sizeof(finish) - 1u &&
                 memcmp(header->value.data, finish, sizeof(finish) - 1u) == 0);
          binding_body = fixture->finish;
          binding_capacity = sizeof(fixture->finish);
          binding_response = "{\"success\":true}";
        }
        operation_seen = true;
        continue;
      }
      static const char query[] =
          "alipay.livetradeprod.facepay.sdk.inside.queryOrderStatus";
      static const char ack[] =
          "alipay.livetradeprod.facepay.sdk.inside.payResultAck";
      acknowledged = header->value.len == sizeof(ack) - 1u &&
                     memcmp(header->value.data, ack, sizeof(ack) - 1u) == 0;
      assert(acknowledged ||
             (header->value.len == sizeof(query) - 1u &&
              memcmp(header->value.data, query, sizeof(query) - 1u) == 0));
      operation_seen = true;
    }
  }
  assert(operation_seen);
  size_t capacity =
      fixture->binding_mode ? binding_capacity : sizeof(fixture->query);
  assert(input->body_len < capacity);
  char* body = fixture->binding_mode ? binding_body
               : acknowledged        ? fixture->ack
                                     : fixture->query;
  memcpy(body, input->body, input->body_len);
  body[input->body_len] = '\0';
  if (acknowledged) ++fixture->acknowledgements;
  const char* response = fixture->binding_mode ? binding_response
                         : acknowledged        ? "{}"
                                               : fixture->response;
  size_t length = strlen(response);
  assert(length <= input->response_buf_cap);
  memcpy(input->response_buf, response, length);
  output->body = input->response_buf;
  output->body_len = length;
  output->status_code = 200;
  return h2_pal_http_deliver_response_header(input, "Result-Status", 13u,
                                             "1000", 4u);
}

static void response_free(void* user, h2_pal_http_response_t* response) {
  (void)user;
  h2_pal_http_response_reset(response);
}

static uint64_t number(yyjson_val* root, const char* name) {
  yyjson_val* value = yyjson_obj_get(root, name);
  assert(yyjson_is_uint(value));
  return yyjson_get_uint(value);
}

static int integer(yyjson_val* root, const char* name) {
  yyjson_val* value = yyjson_obj_get(root, name);
  assert(yyjson_is_int(value));
  int64_t result = yyjson_get_sint(value);
  assert(result >= INT32_MIN && result <= INT32_MAX);
  return (int)result;
}

static bool boolean(yyjson_val* root, const char* name) {
  yyjson_val* value = yyjson_obj_get(root, name);
  assert(yyjson_is_bool(value));
  return yyjson_get_bool(value);
}

static unsigned hex_digit(char value) {
  assert((value >= '0' && value <= '9') || (value >= 'a' && value <= 'f'));
  return value <= '9' ? (unsigned)(value - '0') : (unsigned)(value - 'a') + 10u;
}

static size_t decode(yyjson_val* root, const char* name, void* output,
                     size_t capacity) {
  yyjson_val* value = yyjson_obj_get(root, name);
  assert(yyjson_is_str(value));
  size_t length = yyjson_get_len(value);
  assert(length % 2u == 0u && length / 2u <= capacity);
  const char* text = yyjson_get_str(value);
  uint8_t* bytes = output;
  for (size_t index = 0u; index < length / 2u; ++index)
    bytes[index] = (uint8_t)((hex_digit(text[index * 2u]) << 4u) |
                             hex_digit(text[index * 2u + 1u]));
  return length / 2u;
}

static void match(yyjson_val* root, const char* name, const void* data,
                  size_t length) {
  uint8_t expected[4096];
  size_t size = decode(root, name, expected, sizeof(expected));
  if (size != length ||
      (length != 0u && (data == NULL || memcmp(expected, data, length) != 0))) {
    fprintf(stderr, "mismatch field=%s expected_bytes=%zu actual_bytes=%zu\n",
            name, size, length);
    const uint8_t* actual = data;
    for (size_t index = 0u; actual != NULL && index < size && index < length;
         ++index) {
      if (expected[index] == actual[index]) continue;
      fprintf(stderr, "first difference at %zu: %02x/%02x\n", index,
              actual[index], expected[index]);
      break;
    }
    assert(false);
  }
}

static void hash(const uint8_t* data, size_t length, uint8_t output[32]) {
  h2_sha256_t state;
  h2_sha256_init(&state);
  h2_sha256_update(&state, data, length);
  h2_sha256_finish(&state, output);
}

static void store(fixture_t* fixture, yyjson_val* root, bool bound) {
  h2_pal_pref_namespace_t* namespace = NULL;
  assert(h2_pal_pref_open(&fixture->pref.api, "compatibility",
                          H2_PAL_PREF_OPEN_READ_WRITE, &namespace) == 0);
  assert(namespace->clear(namespace) == 0);
  uint8_t data[1024] = {0};
  if (bound) {
    uint8_t key[32], seed[128];
    assert(decode(root, "key", key, sizeof(key)) == sizeof(key));
    size_t length = decode(root, "seed", seed, sizeof(seed));
    assert(length >= 16u);
    memcpy(data, "APB1", 4u);
    h2_alipay_client_fingerprint(&fixture->config, data + 4u);
    data[36] = BINDING_COMPLETE;
    const char* uid = yyjson_get_str(yyjson_obj_get(root, "uid"));
    if (uid == NULL) uid = "TEST_UID";
    size_t uid_length = strlen(uid);
    assert(uid_length > 0u && uid_length <= 32u);
    data[39] = 8u;
    data[41] = (uint8_t)uid_length;
    data[47] = (uint8_t)length;
    size_t offset = 49u;
    memcpy(data + offset, "TEST_TID", 8u);
    offset += 8u;
    memcpy(data + offset, uid, uid_length);
    offset += uid_length;
    memcpy(data + offset, seed, length);
    offset += length;
    memcpy(data + offset, key, sizeof(key));
    offset += sizeof(key);
    hash(data, offset, data + offset);
    offset += 32u;
    assert(namespace->set_blob(namespace, "binding", data, offset) == 0);
    h2_alipay_client_wipe(data, sizeof(data));
    h2_alipay_client_wipe(key, sizeof(key));
    h2_alipay_client_wipe(seed, sizeof(seed));
  }
  memcpy(data, "APC1", 4u);
  h2_alipay_client_fingerprint(&fixture->config, data + 4u);
  memcpy(data + 36u, "0123456789ABCDEF", 16u);
  data[52] = 7u;
  memcpy(data + 53u, "DEVICE1", 7u);
  assert(namespace->set_blob(namespace, "context", data, 60u) == 0);
  h2_alipay_client_wipe(data, sizeof(data));
  assert(namespace->commit(namespace) == 0);
  assert(namespace->close(namespace) == 0);
}

static int fixed_random(void* user, uint8_t* output, size_t length) {
  (void)user;
  memset(output, 0x42, length);
  return H2_PAL_OK;
}

static int fixed_keypair(void* user, h2_pal_p256_keypair_t* output) {
  (void)user;
  if (migration_key_mode) {
    assert(migration_key_step < 2u);
    return h2_pal_crypto_p256_keypair_from_private(
        h2_wolfcrypt_crypto_api(),
        &migration_private_keys[migration_key_step++], output);
  }
  int result = h2_pal_crypto_p256_keypair_from_private(
      h2_wolfcrypt_crypto_api(), &controlled_private_key, output);
  return result;
}

static int fixed_sign(void* user, const h2_pal_p256_private_key_t* private_key,
                      const uint8_t* message, size_t length,
                      h2_pal_p256_signature_t* signature) {
  (void)user;
  uint8_t nonce[32];
  memset(nonce, 0x42, sizeof(nonce));
  int rc = h2_wolfcrypt_test_p256_sign(nonce, private_key, message, length,
                                       signature);
  if (rc == 0) {
    h2_pal_p256_keypair_t pair;
    assert(h2_pal_crypto_p256_keypair_from_private(h2_wolfcrypt_crypto_api(),
                                                   private_key, &pair) == 0);
    assert(h2_pal_crypto_ecdsa_p256_sha256_verify(h2_wolfcrypt_crypto_api(),
                                                  &pair.public_key, message,
                                                  length, signature) == 0);
    h2_alipay_client_wipe(&pair, sizeof(pair));
  }
  return rc;
}

static int fixed_secp192k1_sign(
    void* user, const h2_pal_secp192k1_private_key_t* private_key,
    const uint8_t* message, size_t length,
    h2_pal_secp192k1_signature_t* signature) {
  (void)user;
  uint8_t nonce[24];
  memset(nonce, 0x42, sizeof(nonce));
  return h2_wolfcrypt_test_secp192k1_sign(nonce, private_key, message, length,
                                          signature);
}

static int fixed_sm2_sign(void* user,
                          const h2_pal_sm2_private_key_t* private_key,
                          const uint8_t digest[32],
                          h2_pal_sm2_signature_t* signature) {
  (void)user;
  uint8_t nonce[32];
  memset(nonce, 0x42, sizeof(nonce));
  return h2_wolfcrypt_test_sm2_sign(nonce, private_key, digest, signature);
}

static void initialize(fixture_t* fixture) {
  memset(fixture, 0, sizeof(*fixture));
  h2_app_test_mem_init(&fixture->memory, NULL);
  h2_app_test_time_init(&fixture->time, 0u);
  h2_app_test_sync_init(&fixture->sync);
  assert(h2_app_test_pref_init(&fixture->pref, &fixture->memory.api) == 0);
  const h2_wolfcrypt_crypto_config_t crypto = {&fixture->entropy, entropy};
  assert(h2_wolfcrypt_crypto_init(&crypto) == 0);
  fixture->crypto_vtable = *h2_wolfcrypt_crypto_api()->vtable;
  fixture->crypto_vtable.random = fixed_random;
  fixture->crypto_vtable.p256_keypair_generate = fixed_keypair;
  fixture->crypto_vtable.ecdsa_p256_sha256_sign = fixed_sign;
  fixture->crypto_vtable.ecdsa_secp192k1_sha1_sign = fixed_secp192k1_sign;
  fixture->crypto_vtable.sm2_sign_digest = fixed_sm2_sign;
  fixture->crypto = (h2_pal_crypto_api_t){h2_wolfcrypt_crypto_api()->user,
                                          &fixture->crypto_vtable};
  static const h2_pal_http_vtable_t http_vtable = {request, response_free};
  static h2_pal_http_api_t http;
  http = (h2_pal_http_api_t){fixture, &http_vtable};
  fixture->config = (h2_alipay_client_config_t){
      .mem = &fixture->memory.api,
      .time = &fixture->time.api,
      .http = &http,
      .pref = &fixture->pref.api,
      .crypto = &fixture->crypto,
      .identity = {"TEST_SN", "TEST_SN", "12:34:56:78:9A:BC", "H106"},
      .company = "HAIVIVI",
      .pref_namespace = "compatibility",
      .product_type = 3u,
      .feature = 16385u,
      .compile_timestamp_s = 1794449000u};
  assert(h2_alipay_independent_configure(&fixture->config,
                                         &fixture->sync.api) == 0);
  assert(alipay_pre_init() == 0 && alipay_pre_init() == 0);
  uint8_t text[64];
  uint32_t length = sizeof(text);
  assert(alipay_get_companyName(text, &length) == 0 &&
         length == strlen("HAIVIVI") && strcmp((char*)text, "HAIVIVI") == 0);
  length = sizeof(text);
  assert(alipay_get_productModel(text, &length) == 0 &&
         length == strlen("H106") && strcmp((char*)text, "H106") == 0);
  length = sizeof(text);
  assert(alipay_get_sn(text, &length) == 0 && length == strlen("TEST_SN") &&
         strcmp((char*)text, "TEST_SN") == 0);
  length = sizeof(text);
  assert(alipay_get_imei(text, &length) == 0 && length == strlen("TEST_SN") &&
         strcmp((char*)text, "TEST_SN") == 0);
  length = sizeof(text);
  assert(alipay_get_mac(text, &length) == 0 && length == 17u &&
         strcmp((char*)text, "12:34:56:78:9A:BC") == 0);
  assert(alipay_get_productType() == 3u);
}

static void payer(fixture_t* fixture, yyjson_val* root) {
  fixture->binding_mode = false;
  bool bound = boolean(root, "bound");
  store(fixture, root, bound);
  assert(h2_pal_time_set_wall_ms(&fixture->time.api,
                                 number(root, "epoch") * 1000u) == 0);
  uint8_t output[32];
  memset(output, 0xa5, sizeof(output));
  uint32_t length = sizeof(output);
  int result =
      alipay_get_paycode(boolean(root, "null_output") ? NULL : output, &length);
  assert(result == integer(root, "result"));
  assert(length == number(root, "length") && output[31] == 0xa5);
  if (result == 0) match(root, "output", output, length + 1u);
  assert(alipay_get_binding_status() == bound);
}

static void collection(fixture_t* fixture, yyjson_val* root) {
  bool bound = boolean(root, "bound");
  store(fixture, root, bound);
  assert(h2_pal_time_set_wall_ms(&fixture->time.api,
                                 number(root, "epoch") * 1000u) == 0);
  uint8_t output[80];
  memset(output, 0xa5, sizeof(output));
  uint32_t length = (uint32_t)number(root, "capacity");
  int result = alipay_get_collectioncode(
      boolean(root, "null_output") ? NULL : output, &length);
  assert(result == integer(root, "result"));
  assert(length == number(root, "length"));
  match(root, "output", output, sizeof(output));
  assert(alipay_get_binding_status() == bound);
}

static void tap_after_query(bool unbound) {
  uint8_t token[] = "NFC_TOKEN", code[32];
  memset(code, 0xa5, sizeof(code));
  uint32_t length = 0xa5u;
  int rc = alipay_get_plain_paycode_with_nfc_token(token, sizeof(token) - 1u,
                                                   code, &length);
  assert(rc == (unbound ? 99 : 0));
  if (unbound) {
    assert(length == 0xa5u && code[0] == 0xa5u);
  } else {
    assert(length == 18u && code[18] == 0xa5u);
    assert(memcmp(code, "284960766182755224", 18u) == 0);
  }
}

static void query(fixture_t* fixture, yyjson_val* root) {
  fixture->binding_mode = false;
  store(fixture, root, true);
  assert(h2_pal_time_set_wall_ms(&fixture->time.api, 1794449000000u) == 0);
  size_t response_length = decode(root, "response", fixture->response,
                                  sizeof(fixture->response) - 1u);
  fixture->response[response_length] = '\0';
  fixture->query[0] = fixture->ack[0] = '\0';
  fixture->acknowledgements = 0u;
  char code[] = "284960766182755224";
  assert(h2_alipay_independent_set_nfc_context("NFC_TOKEN", "SQUEEZE", code) ==
         0);
  char amount[32] = {0}, ext[128] = {0};
  uint32_t length = sizeof(ext);
  alipay_payment_status_e status = ALIPAY_PAYMENT_STATUS_UNKNOWN;
  int result =
      yyjson_is_true(yyjson_obj_get(root, "nfc"))
          ? alipay_query_last_nfc_pay_result(&status, amount, ext, &length)
          : alipay_query_order_result_string(code, &status, amount, ext,
                                             &length);
  if (length != number(root, "ext_length"))
    fprintf(stderr, "query ext_length actual=%u expected=%llu\n", length,
            (unsigned long long)number(root, "ext_length"));
  assert(result == integer(root, "result"));
  assert((int)status == integer(root, "status"));
  assert(length == number(root, "ext_length"));
  match(root, "amount", amount, strlen(amount));
  match(root, "ext", ext, status == ALIPAY_PAYMENT_STATUS_FAIL ? length : 0u);
  match(root, "query", fixture->query, strlen(fixture->query));
  match(root, "ack", fixture->ack, strlen(fixture->ack));
  assert(fixture->acknowledgements == number(root, "acks"));
  assert(alipay_get_binding_status() == !boolean(root, "unbound"));
  tap_after_query(boolean(root, "unbound"));
}

static void query_bitmap(fixture_t* fixture, yyjson_val* root) {
  fixture->binding_mode = false;
  store(fixture, root, true);
  assert(h2_pal_time_set_wall_ms(&fixture->time.api, 1794449000000u) == 0);
  size_t length = decode(root, "response", fixture->response,
                         sizeof(fixture->response) - 1u);
  fixture->response[length] = '\0';
  fixture->query[0] = fixture->ack[0] = '\0';
  fixture->acknowledgements = 0u;
  struct {
    uint8_t before;
    char bitmap[32][14];
    uint8_t after;
  } output = {.before = 0x12u, .after = 0x34u};
  memset(output.bitmap, (int)number(root, "pattern"), sizeof(output.bitmap));
  char code[] = "284960766182755224";
  assert(h2_alipay_independent_set_nfc_context("NFC_TOKEN", "SQUEEZE", code) ==
         0);
  alipay_payment_status_e status = ALIPAY_PAYMENT_STATUS_UNKNOWN;
  int result = alipay_query_order_result(code, &status, output.bitmap);
  if (result != integer(root, "result") ||
      (int)status != integer(root, "status"))
    fprintf(stderr, "bitmap query result=%d/%d status=%d/%d response=%s\n",
            result, integer(root, "result"), (int)status,
            integer(root, "status"), fixture->response);
  assert(result == integer(root, "result"));
  assert((int)status == integer(root, "status"));
  assert(output.before == 0x12u && output.after == 0x34u);
  match(root, "output", output.bitmap, sizeof(output.bitmap));
  match(root, "query", fixture->query, strlen(fixture->query));
  match(root, "ack", fixture->ack, strlen(fixture->ack));
  assert(fixture->acknowledgements == number(root, "acks"));
  assert(alipay_get_binding_status() == !boolean(root, "unbound"));
  tap_after_query(boolean(root, "unbound"));
}

static void binding_start(fixture_t* fixture, yyjson_val* root) {
  fixture->binding_mode = true;
  store(fixture, root, false);
  assert(h2_pal_time_set_wall_ms(&fixture->time.api, 1794449000000u) == 0);
  assert(alipay_pre_init() == integer(root, "init"));
  assert(alipay_pre_init() == integer(root, "reentry"));
  assert(decode(root, "private", controlled_private_key.bytes,
                sizeof(controlled_private_key.bytes)) ==
         sizeof(controlled_private_key.bytes));
  char code[256] = {0};
  int length = sizeof(code);
  int result = alipay_get_binding_code(16385u, code, &length);
  assert(result == integer(root, "result") &&
         length == integer(root, "length"));
  match(root, "output", code, (size_t)length + 1u);
  match(root, "prepare", fixture->prepare, strlen(fixture->prepare));
  assert(!alipay_get_binding_status());
  h2_alipay_client_wipe(&controlled_private_key,
                        sizeof(controlled_private_key));
}

static void binding_query(fixture_t* fixture, yyjson_val* root) {
  fixture->binding_mode = true;
  size_t length = decode(root, "response", fixture->response,
                         sizeof(fixture->response) - 1u);
  fixture->response[length] = '\0';
  fixture->binding_query[0] = fixture->finish[0] = '\0';
  binding_status_e status = ALIPAY_STATUS_UNKNOWN;
  int result = alipay_query_binding_result(&status);
  assert(result == integer(root, "result") &&
         (int)status == integer(root, "status"));
  assert(alipay_get_binding_status() == boolean(root, "bound"));
  match(root, "query", fixture->binding_query, strlen(fixture->binding_query));
  match(root, "finish", fixture->finish, strlen(fixture->finish));
  if (status == ALIPAY_STATUS_BINDING_OK) {
    binding_record_t binding = {0};
    assert(h2_alipay_client_load_binding(&fixture->config, &binding) == 0);
    match(root, "key", binding.secret, sizeof(binding.secret));
    match(root, "seed", binding.seed, binding.seed_length);
    h2_alipay_client_wipe(&binding, sizeof(binding));
  }
}

static void account(yyjson_val* root) {
  typedef retval_e (*getter_t)(uint8_t*, uint32_t*);
  const getter_t getters[] = {alipay_get_logon_ID, alipay_get_nick_name,
                              alipay_get_privacy_code, alipay_get_setting_code};
  unsigned field = (unsigned)number(root, "field");
  assert(field < sizeof(getters) / sizeof(*getters));
  uint8_t output[144];
  memset(output, 0xa5, sizeof(output));
  uint32_t length = (uint32_t)number(root, "capacity");
  int result = getters[field](output, &length);
  if (result != integer(root, "result") || length != number(root, "length"))
    fprintf(stderr,
            "account field=%u capacity=%llu result=%d/%d length=%u/%llu\n",
            field, (unsigned long long)number(root, "capacity"), result,
            integer(root, "result"), length,
            (unsigned long long)number(root, "length"));
  assert(result == integer(root, "result"));
  assert(length == number(root, "length"));
  match(root, "output", output, sizeof(output));
  assert(alipay_get_binding_status() == boolean(root, "bound"));
}

static void unbind(fixture_t* fixture, yyjson_val* root) {
  store(fixture, root, boolean(root, "initial_bound"));
  fixture->unbind_mode = true;
  fixture->network_error = boolean(root, "network_error");
  fixture->unbind_requests = 0u;
  fixture->query[0] = '\0';
  int result = boolean(root, "reset") ? alipay_reset_all() : alipay_unbinding();
  assert(result == integer(root, "result"));
  assert(alipay_get_binding_status() == boolean(root, "bound"));
  assert(fixture->unbind_requests == number(root, "requests"));
  match(root, "query", fixture->query, strlen(fixture->query));
  h2_pal_pref_namespace_t* namespace = NULL;
  assert(h2_pal_pref_open(&fixture->pref.api, "compatibility",
                          H2_PAL_PREF_OPEN_READ_ONLY, &namespace) == 0);
  void* raw = NULL;
  size_t length = 0u;
  int rc = namespace->get_blob(namespace, &fixture->memory.api, "context", &raw,
                               &length);
  assert((rc == 0) == boolean(root, "registration"));
  h2_pal_mem_free(&fixture->memory.api, raw);
  assert(namespace->close(namespace) == 0);
  fixture->unbind_mode = false;
}

static void remote(fixture_t* fixture, yyjson_val* root) {
  store(fixture, root, boolean(root, "initial_bound"));
  fixture->remote_mode = fixture->unbind_mode = true;
  fixture->network_error = boolean(root, "network_error");
  fixture->remote_requests = fixture->unbind_requests = 0u;
  fixture->query[0] = fixture->remote_query[0] = '\0';
  size_t length = decode(root, "response", fixture->response,
                         sizeof(fixture->response) - 1u);
  fixture->response[length] = '\0';
  alipay_remote_bind_status_t status = REMOTE_STATUS_UNKNOWN;
  int result = alipay_query_remote_bind_status(&status);
  assert(result == integer(root, "result"));
  assert((int)status == integer(root, "status"));
  assert(alipay_get_binding_status() == boolean(root, "bound"));
  assert(fixture->remote_requests == number(root, "queries"));
  assert(fixture->unbind_requests == number(root, "unbinds"));
  match(root, "query", fixture->remote_query, strlen(fixture->remote_query));
  match(root, "unbind", fixture->query, strlen(fixture->query));
  fixture->remote_mode = fixture->unbind_mode = false;
}

static void balance(fixture_t* fixture, yyjson_val* root) {
  bool bound = boolean(root, "initial_bound");
  store(fixture, root, bound);
  fixture->remote_mode = true;
  fixture->network_error = boolean(root, "network_error");
  fixture->remote_requests = 0u;
  fixture->remote_query[0] = '\0';
  size_t length = decode(root, "response", fixture->response,
                         sizeof(fixture->response) - 1u);
  fixture->response[length] = '\0';
  struct {
    uint8_t before;
    char bitmap[32][14];
    uint8_t after;
  } output = {.before = 0x12u, .after = 0x34u};
  memset(output.bitmap, (int)number(root, "pattern"), sizeof(output.bitmap));
  int result = alipay_get_balance_bmp(output.bitmap);
  if (result != integer(root, "result"))
    fprintf(stderr, "balance result=%d/%d response=%s\n", result,
            integer(root, "result"), fixture->response);
  assert(result == integer(root, "result"));
  assert(output.before == 0x12u && output.after == 0x34u);
  match(root, "output", output.bitmap, sizeof(output.bitmap));
  match(root, "query", fixture->remote_query, strlen(fixture->remote_query));
  assert(fixture->remote_requests == number(root, "requests"));
  assert(alipay_get_binding_status() == bound);
  fixture->remote_mode = false;
}

static void income(fixture_t* fixture, yyjson_val* root) {
  bool bound = boolean(root, "initial_bound");
  store(fixture, root, bound);
  fixture->income_mode = true;
  fixture->network_error = boolean(root, "network_error");
  fixture->query[0] = '\0';
  assert(h2_pal_time_set_wall_ms(&fixture->time.api, 1794449000000u) == 0);
  size_t length = decode(root, "response", fixture->response,
                         sizeof(fixture->response) - 1u);
  fixture->response[length] = '\0';
  char code[1025];
  length = decode(root, "code", code, sizeof(code) - 1u);
  code[length] = '\0';
  struct {
    uint8_t before;
    alipay_income_detail_info_t detail;
    uint8_t after;
  } output = {.before = 0x12u, .after = 0x34u};
  memset(&output.detail, (int)number(root, "pattern"), sizeof(output.detail));
  int result = alipay_query_income_details(code, &output.detail);
  if (result != integer(root, "result"))
    fprintf(stderr, "income result=%d/%d response=%s\n", result,
            integer(root, "result"), fixture->response);
  assert(result == integer(root, "result"));
  assert(output.detail.is_success == boolean(root, "success"));
  assert(output.before == 0x12u && output.after == 0x34u);
  match(root, "bitmap", output.detail.bitmap_amount,
        sizeof(output.detail.bitmap_amount));
  match(root, "message", output.detail.message, sizeof(output.detail.message));
  match(root, "query", fixture->query, strlen(fixture->query));
  assert(alipay_get_binding_status() == bound);
  fixture->income_mode = false;
}

static void scan_case(fixture_t* fixture, yyjson_val* root, yyjson_val* kind) {
  bool bound = boolean(root, "initial_bound");
  store(fixture, root, bound);
  assert(h2_pal_time_set_wall_ms(&fixture->time.api, 1794449000000u) == 0);
  fixture->scan_mode = true;
  fixture->network_error = boolean(root, "network_error");
  fixture->scan_requests = 0u;
  fixture->query[0] = '\0';
  size_t length = decode(root, "response", fixture->response,
                         sizeof(fixture->response) - 1u);
  fixture->response[length] = '\0';
  length = decode(root, "operation", fixture->scan_operation,
                  sizeof(fixture->scan_operation) - 1u);
  fixture->scan_operation[length] = '\0';
  int result = 0;
  if (yyjson_equals_str(kind, "dzs_precheck")) {
    uint8_t enabled = 0xa5u;
    result = alipay_dzs_get_precheck_status(&enabled);
    assert(enabled == integer(root, "value"));
  } else if (yyjson_equals_str(kind, "dzs_generate")) {
    struct {
      uint8_t before;
      alipay_dzs_code_info_t code;
      uint8_t after;
    } output = {.before = 0x12u, .after = 0x34u};
    memset(&output.code, 0xa5, sizeof(output.code));
    char amount[32];
    length = decode(root, "amount", amount, sizeof(amount) - 1u);
    amount[length] = '\0';
    result = alipay_dzs_generate_payment_qrcode(amount, &output.code);
    assert(output.code.is_success == boolean(root, "success"));
    assert((int)output.code.err_code == integer(root, "error"));
    match(root, "output", output.code.qr_code, sizeof(output.code.qr_code));
    assert(output.before == 0x12u && output.after == 0x34u);
  } else if (yyjson_equals_str(kind, "dzs_query")) {
    alipay_payment_status_e status = ALIPAY_PAYMENT_STATUS_NOTHING;
    result = alipay_dzs_query_latest_payment_result(&status);
    if ((int)status != integer(root, "value"))
      fprintf(stderr, "DZS status=%d/%d response=%s\n", (int)status,
              integer(root, "value"), fixture->response);
    assert((int)status == integer(root, "value"));
  } else {
    assert(yyjson_equals_str(kind, "dzs_cancel"));
    alipay_dzs_cancel_latest_payment();
  }
  if (result != integer(root, "result"))
    fprintf(stderr, "DZS kind=%s result=%d/%d response=%s\n",
            yyjson_get_str(kind), result, integer(root, "result"),
            fixture->response);
  assert(result == integer(root, "result"));
  match(root, "query", fixture->query, strlen(fixture->query));
  assert(fixture->scan_requests == number(root, "requests"));
  assert(alipay_get_binding_status() == bound);
  fixture->scan_mode = false;
}

static void union_case(fixture_t* fixture, yyjson_val* root, bool parse) {
  store(fixture, root, true);
  assert(h2_pal_time_set_wall_ms(&fixture->time.api, 1794449000000u) == 0);
  fixture->union_script = yyjson_obj_get(root, "rpc");
  fixture->union_step = 0u;
  unsigned index = (unsigned)number(root, "case");
  bool nfc = index == 2u || index == 3u;
  int rc;
  if (parse) {
    char input[4096];
    size_t length = decode(root, "input", input, sizeof(input) - 1u);
    input[length] = '\0';
    memset(&fixture->union_order, 0xa5, sizeof(fixture->union_order));
    rc = nfc ? alipay_nfc_parse_with_content(input, &fixture->union_order)
             : alipay_union_code_parse_with_code(input, &fixture->union_order);
    assert(fixture->union_order.is_success == boolean(root, "success"));
    match(root, "error", fixture->union_order.err_code,
          sizeof(fixture->union_order.err_code));
    match(root, "name", fixture->union_order.payee_name,
          sizeof(fixture->union_order.payee_name));
    match(root, "logo", fixture->union_order.payee_logo,
          sizeof(fixture->union_order.payee_logo));
    match(root, "amount", fixture->union_order.pay_amount,
          sizeof(fixture->union_order.pay_amount));
  } else {
    strcpy(fixture->union_order.pay_amount, "0.01");
    alipay_union_code_pay_result_t result;
    memset(&result, 0xa5, sizeof(result));
    rc =
        nfc ? alipay_nfc_confirm_payment(&fixture->union_order, &result)
            : alipay_union_code_confirm_payment(&fixture->union_order, &result);
    assert((int)result.status == integer(root, "status"));
    assert((int)result.res_fail == integer(root, "failure"));
    assert(alipay_get_binding_status() == boolean(root, "bound"));
  }
  if (rc != integer(root, "result"))
    fprintf(stderr, "union case=%u parse=%d result=%d/%d\n", index, parse, rc,
            integer(root, "result"));
  assert(rc == integer(root, "result"));
  assert(fixture->union_step == yyjson_arr_size(fixture->union_script));
  fixture->union_script = NULL;
}

static void tap_static_case(fixture_t* fixture, yyjson_val* root) {
  assert(h2_alipay_independent_release() == 0);
  assert(h2_alipay_independent_configure(&fixture->config,
                                         &fixture->sync.api) == 0);
  assert(alipay_pre_init() == 0);
  store(fixture, root, true);
  char token[32], squeeze[32], code[32], input[64];
  token[decode(root, "token", token, sizeof(token) - 1u)] = '\0';
  squeeze[decode(root, "squeeze", squeeze, sizeof(squeeze) - 1u)] = '\0';
  code[decode(root, "paycode", code, sizeof(code) - 1u)] = '\0';
  if (token[0] != '\0' && code[0] != '\0')
    assert(h2_alipay_independent_set_nfc_context(token, squeeze, code) == 0);
  if (!boolean(root, "initial_bound")) store(fixture, root, false);
  size_t input_length = decode(root, "input", input, sizeof(input) - 1u);
  input[input_length] = '\0';
  uint8_t output[128];
  memset(output, 0xa5, sizeof(output));
  uint32_t length = (uint32_t)number(root, "initial_length");
  int rc = alipay_get_paycode_with_nfc_token(
      boolean(root, "null_token") ? NULL : (uint8_t*)input,
      (uint32_t)number(root, "token_length"), output, &length);
  if (rc != integer(root, "result"))
    fprintf(stderr, "static NFC case=%llu result=%d/%d\n",
            (unsigned long long)number(root, "case"), rc,
            integer(root, "result"));
  assert(rc == integer(root, "result"));
  assert(length == number(root, "length"));
  match(root, "output", output, sizeof(output));
  assert(alipay_npay_ble_broadcast_init() == integer(root, "init_result"));
  assert(alipay_npay_ble_broadcast_init() == integer(root, "reinit_result"));
}

static void tap_case(fixture_t* fixture, yyjson_val* root, yyjson_val* kind) {
  assert(h2_alipay_independent_release() == 0);
  assert(h2_alipay_independent_configure(&fixture->config,
                                         &fixture->sync.api) == 0);
  assert(alipay_pre_init() == 0);
  store(fixture, root, boolean(root, "initial_bound"));
  char token[32], squeeze[32], paycode[32];
  token[decode(root, "token", token, sizeof(token) - 1u)] = '\0';
  squeeze[decode(root, "squeeze", squeeze, sizeof(squeeze) - 1u)] = '\0';
  paycode[decode(root, "paycode", paycode, sizeof(paycode) - 1u)] = '\0';
  if (squeeze[0] != '\0' && boolean(root, "initial_bound"))
    assert(h2_alipay_independent_set_nfc_context(token, squeeze, paycode) == 0);
  uint8_t input[64];
  size_t size = decode(root, "input", input, sizeof(input) - 1u);
  input[size] = '\0';
  int rc;
  if (yyjson_equals_str(kind, "tap_v2")) {
    alipay_npay_ble_pay_result_t output;
    memset(&output, 0xa5, sizeof(output));
    rc = alipay_npay_ble_scan_pay_res_data_handle_v2(
        input, (uint32_t)number(root, "length"), &output);
    match(root, "output", &output, sizeof(output));
  } else if (yyjson_equals_str(kind, "tap_legacy")) {
    uint32_t amount = 0xa5a5a5a5u;
    rc = alipay_npay_ble_scan_pay_res_data_handle(
        input, (uint32_t)number(root, "length"), &amount);
    match(root, "output", &amount, sizeof(amount));
  } else {
    uint8_t output[256];
    memset(output, 0xa5, sizeof(output));
    uint32_t length = (uint32_t)number(root, "case");
    if (yyjson_equals_str(kind, "tap_plain")) {
      length = 0xa5a5a5a5u;
      rc = alipay_get_plain_paycode_with_nfc_token(
          input, number(root, "case") == 2u ? 0u : (uint32_t)size, output,
          &length);
      match(root, "output", output, 32u);
    } else if (yyjson_equals_str(kind, "tap_debug")) {
      char* generated = alipay_nfc_test_token_generation();
      assert(generated != NULL);
      length = (uint32_t)strlen(generated);
      assert(length < 128u);
      memcpy(output, generated, length + 1u);
      match(root, "output", output, 128u);
      rc = 0;
    } else if (yyjson_equals_str(kind, "tap_aid")) {
      length = (uint32_t)number(root, "case") % 258u;
      assert(h2_pal_time_set_wall_ms(&fixture->time.api, 1794449000000u) == 0);
      rc = alipay_get_aid_code((char*)output, &length,
                               number(root, "case") >= 258u);
      match(root, "output", output, sizeof(output));
    } else {
      rc = yyjson_equals_str(kind, "tap_coupon")
               ? alipay_npay_get_coupon_detail_url(output, &length)
               : alipay_get_nfc_available_nearby_locations_url(output, &length);
      match(root, "output", output, sizeof(output));
    }
    assert(length == number(root, "length"));
  }
  if (rc != integer(root, "result"))
    fprintf(stderr, "tap kind=%s case=%llu result=%d/%d\n",
            yyjson_get_str(kind), (unsigned long long)number(root, "case"), rc,
            integer(root, "result"));
  assert(rc == integer(root, "result"));
}

static int unexpected_nfc_send(const uint8_t* data, uint32_t length) {
  (void)data;
  (void)length;
  assert(false);
  return 0;
}

static int unexpected_nfc_status(alipay_payment_status_t* status) {
  (void)status;
  assert(false);
  return 0;
}

static int quick_changed(bool flag) {
  ++quick_callbacks;
  quick_last_flag = flag;
  assert(alipay_get_binding_status());
  return 42;
}

static int sync_quick_changed(bool flag) {
  ++quick_callbacks;
  quick_last_flag = flag;
  h2_alipay_independent_binding_result_t result;
  assert(h2_alipay_independent_last_binding_result(&result) == 0);
  return 42;
}

static void sync_case(fixture_t* fixture, yyjson_val* root) {
  store(fixture, root, true);
  assert(h2_alipay_independent_set_nfc_context("TOKEN123", "SQUEEZE123",
                                               "123456789012345678") == 0);
  store(fixture, root, boolean(root, "initial_bound"));
  uint8_t factory[2048];
  size_t factory_length = decode(root, "factory", factory, sizeof(factory));
  if (factory_length != 0u) {
    h2_pal_pref_namespace_t* namespace = NULL;
    assert(h2_pal_pref_open(&fixture->pref.api, "compatibility",
                            H2_PAL_PREF_OPEN_READ_WRITE, &namespace) == 0);
    assert(namespace->set_blob(namespace, "device_certificate", factory,
                               factory_length) == 0);
    assert(namespace->commit(namespace) == 0 &&
           namespace->close(namespace) == 0);
  }
  const alipay_nfc_payment_callback_t callbacks = {
      .send_data_handle = unexpected_nfc_send,
      .notify_payment_result_handle = unexpected_nfc_status,
      .quick_pay_flag_change_callback = sync_quick_changed};
  alipay_nfc_init_callbacks(callbacks);
  quick_callbacks = 0u;
  quick_last_flag = false;
  assert(decode(root, "private", controlled_private_key.bytes, 32u) == 32u);
  uint8_t input[4096], sentinel = 0xa5u;
  size_t size = decode(root, "input", input, sizeof(input));
  uint8_t* output = &sentinel;
  uint32_t length = 777u;
  int rc = alipay_biz_process_entry(input, (uint32_t)size, &output, &length);
  if (rc != integer(root, "result")) {
    fprintf(stderr, "sync result=%d/%d input=%.*s\n", rc,
            integer(root, "result"), (int)size, (const char*)input);
    assert(false);
  }
  if (length != number(root, "length"))
    fprintf(stderr, "sync length=%u/%llu input=%.*s output=%.*s\n", length,
            (unsigned long long)number(root, "length"), (int)size,
            (const char*)input, output != &sentinel ? (int)length : 0,
            (const char*)output);
  assert(length == number(root, "length"));
  assert((output != &sentinel) == boolean(root, "output_changed"));
  uint8_t expected_reply[2048];
  size_t expected_length =
      decode(root, "output", expected_reply, sizeof(expected_reply));
  if (output != &sentinel && expected_length == length + 1u &&
      memcmp(output, expected_reply, expected_length) != 0) {
    fprintf(stderr, "Sync byte mismatch input=%.*s\n", (int)size,
            (const char*)input);
    yyjson_doc* actual_document = yyjson_read((const char*)output, length, 0u);
    yyjson_doc* expected_document =
        yyjson_read((const char*)expected_reply, length, 0u);
    assert(actual_document != NULL && expected_document != NULL);
    fprintf(stderr, "Actual proof=%s\nExpected proof=%s\n",
            yyjson_get_str(yyjson_obj_get(yyjson_doc_get_root(actual_document),
                                          "bizSign")),
            yyjson_get_str(yyjson_obj_get(
                yyjson_doc_get_root(expected_document), "bizSign")));
    yyjson_doc_free(actual_document);
    yyjson_doc_free(expected_document);
  }
  match(root, "output", output, output != &sentinel ? length + 1u : 1u);
  assert(quick_callbacks == number(root, "quick_calls"));
  assert(quick_last_flag == boolean(root, "quick_value"));
  uint8_t state[4];
  assert(decode(root, "binding", state, sizeof(state)) == sizeof(state));
  assert(alipay_get_binding_status() == (state[0] == 0xa5u));
  h2_pal_pref_namespace_t* namespace = NULL;
  assert(h2_pal_pref_open(&fixture->pref.api, "compatibility",
                          H2_PAL_PREF_OPEN_READ_ONLY, &namespace) == 0);
  const char* names[] = {"quick", "show"};
  const char* keys[] = {"npay_quick", "npay_show"};
  for (size_t index = 0u; index < 2u; ++index) {
    uint8_t expected[4];
    size_t count = decode(root, names[index], expected, sizeof(expected));
    uint32_t value = 0u;
    int stored = namespace->get_u32(namespace, keys[index], &value);
    if (count == 0u)
      assert(stored == H2_PAL_ERR_NOT_FOUND);
    else {
      assert(count == 4u && stored == 0);
      uint32_t number = (uint32_t)expected[0] | ((uint32_t)expected[1] << 8u) |
                        ((uint32_t)expected[2] << 16u) |
                        ((uint32_t)expected[3] << 24u);
      assert(value == number);
    }
  }
  assert(namespace->close(namespace) == 0);
  if (state[0] == 0xa5u) {
    binding_record_t binding = {0};
    assert(h2_alipay_client_load_binding(&fixture->config, &binding) == 0);
    match(root, "stored_key", binding.secret, sizeof(binding.secret));
    match(root, "stored_seed", binding.seed, binding.seed_length);
    match(root, "stored_nickname", binding.nickname, strlen(binding.nickname));
    match(root, "stored_login", binding.login, strlen(binding.login));
    match(root, "stored_tid", binding.tid, strlen(binding.tid));
    match(root, "stored_uid", binding.uid, strlen(binding.uid));
    h2_alipay_client_wipe(&binding, sizeof(binding));
  }
  if (factory_length != 0u) {
    size_t first = fixture->memory.calls;
    uint8_t* reply = NULL;
    uint32_t reply_length = 0u;
    assert(alipay_biz_process_entry(input, (uint32_t)size, &reply,
                                    &reply_length) == 0);
    size_t allocations = fixture->memory.calls - first;
    size_t baseline = fixture->memory.live_blocks;
    for (size_t failure = 1u; failure <= allocations; ++failure) {
      uint8_t guard = 0xa5u;
      reply = &guard;
      reply_length = 777u;
      fixture->memory.fail_at = fixture->memory.calls + failure;
      int failed = alipay_biz_process_entry(input, (uint32_t)size, &reply,
                                            &reply_length);
      fixture->memory.fail_at = 0u;
      assert(failed == 0 || failed == 29 || failed == 99);
      if (failed != 0)
        assert(reply == &guard && reply_length == 777u && guard == 0xa5u);
      else
        assert(reply != &guard && reply_length < 2048u &&
               reply[reply_length] == '\0');
      assert(fixture->memory.live_blocks == baseline &&
             fixture->pref.active_handles == 0u);
      ++certificate_fault_points;
    }
    assert(alipay_biz_process_entry(input, (uint32_t)size, &reply,
                                    &reply_length) == 0);
    assert(reply_length == number(root, "length"));
    match(root, "output", reply, (size_t)reply_length + 1u);
  }
}

static void migration_case(fixture_t* fixture, yyjson_val* root) {
  assert(h2_alipay_independent_release() == 0);
  assert(h2_pal_time_set_wall_ms(&fixture->time.api,
                                 number(root, "epoch") * 1000u) == 0);
  fixture->config.legacy_pref = &fixture->pref.api;
  fixture->config.legacy_pref_namespace = "compatibility_legacy";
  assert(h2_alipay_independent_configure(&fixture->config,
                                         &fixture->sync.api) == 0);
  assert(alipay_pre_init() == 0);
  bool bound = boolean(root, "initial_bound");
  store(fixture, root, bound);
  if (bound) {
    binding_record_t record = {0};
    assert(h2_alipay_client_load_binding(&fixture->config, &record) == 0);
    strcpy(record.nickname, "INITIAL_NAME");
    strcpy(record.login, "INITIAL_LOGIN");
    record.nickname_length = 12u;
    record.login_length = 13u;
    assert(h2_alipay_client_store_binding(&fixture->config, &record) == 0);
    h2_alipay_client_wipe(&record, sizeof(record));
  }
  h2_pal_pref_namespace_t* namespace = NULL;
  assert(h2_pal_pref_open(&fixture->pref.api, "compatibility_legacy",
                          H2_PAL_PREF_OPEN_READ_WRITE, &namespace) == 0);
  assert(namespace->clear(namespace) == 0 && namespace->commit(namespace) == 0);
  assert(namespace->close(namespace) == 0);
  unsigned scenario = (unsigned)number(root, "scenario");
  unsigned pattern = (unsigned)number(root, "pattern");
  uint8_t key[32], seed[128];
  assert(decode(root, "key", key, sizeof(key)) == sizeof(key));
  size_t seed_length = decode(root, "seed", seed, sizeof(seed));
  assert(ALIPAY_deprecated_storage_write(2u, key, sizeof(key)) == 0);
  assert(ALIPAY_deprecated_storage_write(4u, seed, (uint32_t)seed_length) == 0);
  if (pattern < 2u) {
    assert(ALIPAY_deprecated_storage_write(16u, (const uint8_t*)"LEGACY_NAME",
                                           11u + pattern % 2u) == 0);
    assert(ALIPAY_deprecated_storage_write(32u, (const uint8_t*)"LEGACY_LOGIN",
                                           12u + pattern % 2u) == 0);
  } else {
    uint8_t text[256];
    memset(text, 'N', sizeof(text));
    text[255] = 0u;
    assert(ALIPAY_deprecated_storage_write(16u, text, 255u + pattern % 2u) ==
           0);
    memset(text, 'L', 255u);
    assert(ALIPAY_deprecated_storage_write(32u, text, 255u + pattern % 2u) ==
           0);
  }
  assert(ALIPAY_deprecated_storage_write(65u, (const uint8_t*)"UNUSED_TID",
                                         10u) == 0);
  assert(ALIPAY_deprecated_storage_write(66u, (const uint8_t*)"TEST_UID", 8u) ==
         0);
  uint8_t flag[4] = {scenario == 1u ? 1u : 0xa5u, 0u, 0u, 0u};
  assert(ALIPAY_deprecated_storage_write(64u, flag, sizeof(flag)) == 0);
  const uint32_t removed[] = {64u, 0u, 0u, 4u, 2u, 16u, 32u, 66u};
  if (scenario < sizeof(removed) / sizeof(*removed) && removed[scenario] != 0u)
    assert(ALIPAY_deprecated_storage_delete(removed[scenario]) == 0);
  assert(decode(root, "private_ifaa", migration_private_keys[0].bytes, 32u) ==
         32u);
  assert(decode(root, "private_microecc", migration_private_keys[1].bytes,
                32u) == 32u);
  migration_key_mode = true;
  migration_key_step = 0u;
  int rc = alipay_migrate_data();
  if (rc != integer(root, "result"))
    fprintf(stderr, "migration scenario=%u pattern=%u result=%d/%d\n", scenario,
            pattern, rc, integer(root, "result"));
  assert(rc == integer(root, "result"));
  assert(migration_key_step == number(root, "keypairs"));
  assert(alipay_migrate_data() == integer(root, "repeated"));
  migration_key_mode = false;
  assert(alipay_get_binding_status() == boolean(root, "bound"));
  uint8_t code[32], nickname[64], login[64];
  memset(code, 0xa5, sizeof(code));
  memset(nickname, 0xa5, sizeof(nickname));
  memset(login, 0xa5, sizeof(login));
  uint32_t code_length = sizeof(code), name_length = sizeof(nickname),
           login_length = sizeof(login);
  int payer = alipay_get_paycode(code, &code_length);
  if (payer != integer(root, "payer_result")) {
    h2_alipay_client_payment_t detail;
    assert(h2_alipay_independent_last_payment_result(&detail) == 0);
    fprintf(stderr,
            "migration payer scenario=%u pattern=%u result=%d/%d pal=%d "
            "stage=%d\n",
            scenario, pattern, payer, integer(root, "payer_result"),
            detail.result, detail.stage);
  }
  assert(payer == integer(root, "payer_result"));
  assert(alipay_get_nick_name(nickname, &name_length) ==
         integer(root, "nickname_result"));
  assert(alipay_get_logon_ID(login, &login_length) ==
         integer(root, "login_result"));
  assert(code_length == number(root, "payer_length"));
  assert(name_length == number(root, "nickname_length"));
  assert(login_length == number(root, "login_length"));
  match(root, "payer", code, sizeof(code));
  match(root, "nickname", nickname, sizeof(nickname));
  match(root, "login", login, sizeof(login));
  uint8_t wide_name[256], wide_login[256];
  memset(wide_name, 0xa5, sizeof(wide_name));
  memset(wide_login, 0xa5, sizeof(wide_login));
  uint32_t wide_name_length = sizeof(wide_name),
           wide_login_length = sizeof(wide_login);
  assert(alipay_get_nick_name(wide_name, &wide_name_length) ==
         integer(root, "wide_name_result"));
  assert(alipay_get_logon_ID(wide_login, &wide_login_length) ==
         integer(root, "wide_login_result"));
  assert(wide_name_length == number(root, "wide_name_length"));
  assert(wide_login_length == number(root, "wide_login_length"));
  match(root, "wide_name", wide_name, sizeof(wide_name));
  match(root, "wide_login", wide_login, sizeof(wide_login));
  uint8_t presence[7];
  const uint32_t items[] = {2u, 4u, 16u, 32u, 64u, 65u, 66u};
  for (size_t index = 0u; index < 7u; ++index)
    presence[index] = ALIPAY_deprecated_storage_exists(items[index]) == 0;
  match(root, "presence", presence, sizeof(presence));
  if (boolean(root, "bound")) {
    binding_record_t record = {0};
    assert(h2_alipay_client_load_binding(&fixture->config, &record) == 0);
    match(root, "stored_key", record.secret, sizeof(record.secret));
    match(root, "stored_seed", record.seed, record.seed_length);
    match(root, "stored_tid", record.tid, strlen(record.tid));
    h2_alipay_client_wipe(&record, sizeof(record));
  }
  h2_alipay_client_wipe(migration_private_keys, sizeof(migration_private_keys));
  h2_alipay_client_wipe(key, sizeof(key));
  h2_alipay_client_wipe(seed, sizeof(seed));
}

static void tap_setting_case(fixture_t* fixture, yyjson_val* root) {
  assert(h2_alipay_independent_release() == 0);
  assert(h2_alipay_independent_configure(&fixture->config,
                                         &fixture->sync.api) == 0);
  assert(alipay_pre_init() == 0);
  store(fixture, root, true);
  h2_pal_pref_namespace_t* namespace = NULL;
  assert(h2_pal_pref_open(&fixture->pref.api, "compatibility",
                          H2_PAL_PREF_OPEN_READ_WRITE, &namespace) == 0);
  if (number(root, "present") != 0u) {
    assert(namespace->set_u32(namespace, "npay_quick",
                              (uint32_t)number(root, "initial")) == 0);
    assert(namespace->set_u32(namespace, "npay_show",
                              (uint32_t)number(root, "initial")) == 0);
  }
  assert(namespace->set_u32(namespace, "npay_guide", 0x5a5a5a5au) == 0);
  assert(namespace->commit(namespace) == 0 && namespace->close(namespace) == 0);
  alipay_nfc_payment_callback_t callbacks = {
      .send_data_handle = unexpected_nfc_send,
      .notify_payment_result_handle = unexpected_nfc_status,
      .quick_pay_flag_change_callback = quick_changed};
  alipay_nfc_init_callbacks(callbacks);
  alipay_nfc_init_callbacks((alipay_nfc_payment_callback_t){0});
  assert(alipay_nfc_setup_multi_arch((uint8_t)number(root, "multi")) == 0);
  quick_callbacks = 0u;
  quick_last_flag = false;
  unsigned operation = (unsigned)number(root, "setting_op");
  int rc = operation == 0u   ? (int)alipay_npay_get_quick_pay_show_flag()
           : operation == 1u ? (int)alipay_npay_get_quick_pay_flag()
                             : alipay_npay_set_quick_pay_flag(operation == 2u);
  if (rc != integer(root, "result"))
    fprintf(stderr, "setting op=%u multi=%llu result=%d/%d\n", operation,
            (unsigned long long)number(root, "multi"), rc,
            integer(root, "result"));
  assert(rc == integer(root, "result"));
  assert(quick_callbacks == number(root, "callbacks"));
  assert(quick_last_flag == boolean(root, "last_flag"));
  assert(h2_pal_pref_open(&fixture->pref.api, "compatibility",
                          H2_PAL_PREF_OPEN_READ_ONLY, &namespace) == 0);
  const char* keys[] = {"npay_quick", "npay_show", "npay_guide"};
  const char* presence[] = {"quick_present", "show_present", "guide_present"};
  const char* fields[] = {"quick", "show"};
  for (size_t index = 0u; index < 3u; ++index) {
    uint32_t value = 0u;
    int found = namespace->get_u32(namespace, keys[index], &value);
    assert(found ==
           (boolean(root, presence[index]) ? 0 : H2_PAL_ERR_NOT_FOUND));
    if (index < 2u) assert(value == number(root, fields[index]));
  }
  assert(namespace->close(namespace) == 0);
}

static int provision_call(const char* entry, const char* input) {
  if (strcmp(entry, "direct") == 0) return alipay_direct_provision(input);
  if (strcmp(entry, "ota") == 0) return alipay_provision_ota(input);
  assert(strcmp(entry, "active") == 0);
  return alipay_active_device(input);
}

static void provision_case(fixture_t* fixture, yyjson_val* root) {
  assert(h2_alipay_independent_release() == 0);
  h2_alipay_client_config_t previous = fixture->config;
  uint8_t asset[598];
  size_t asset_length = decode(root, "asset", asset, sizeof(asset));
  fixture->config.activation_asset = asset_length != 0u ? asset : NULL;
  fixture->config.activation_asset_length = asset_length;
  assert(h2_alipay_independent_configure(&fixture->config,
                                         &fixture->sync.api) == 0);
  store(fixture, root, false);
  assert(h2_pal_time_set_wall_ms(&fixture->time.api, 1794449000000u) == 0);
  h2_pal_pref_namespace_t* namespace = NULL;
  uint8_t initial[1024];
  size_t initial_length =
      decode(root, "initial_saved", initial, sizeof(initial));
  assert(h2_pal_pref_open(&fixture->pref.api, "compatibility",
                          H2_PAL_PREF_OPEN_READ_WRITE, &namespace) == 0);
  assert(namespace->set_blob(namespace, "device_certificate", initial,
                             initial_length) == 0);
  assert(namespace->commit(namespace) == 0 && namespace->close(namespace) == 0);
  if (boolean(root, "save_failure"))
    fixture->pref.commit =
        (h2_app_test_fault_t){.result = H2_PAL_ERR_IO, .remaining = 1u};
  fixture->provision_mode = true;
  fixture->provision_requests = 0u;
  fixture->provision_operation[0] = fixture->provision_body[0] = '\0';
  fixture->network_error = boolean(root, "network_error");
  size_t response_length = decode(root, "response", fixture->response,
                                  sizeof(fixture->response) - 1u);
  fixture->response[response_length] = '\0';
  char input[1024];
  input[decode(root, "input", input, sizeof(input) - 1u)] = '\0';
  const char* entry = yyjson_get_str(yyjson_obj_get(root, "entry"));
  assert(entry != NULL);
  const char* argument = boolean(root, "null_input") ? NULL : input;
  int rc = provision_call(entry, argument);
  if (rc != integer(root, "result"))
    fprintf(stderr, "Provision entry=%s case=%llu result=%d/%d\n", entry,
            (unsigned long long)number(root, "case"), rc,
            integer(root, "result"));
  assert(rc == integer(root, "result"));
  assert(fixture->provision_requests == number(root, "requests"));
  if (strcmp(entry, "active") == 0) {
    unsigned requests = fixture->provision_requests;
    assert(provision_call(entry, argument) == integer(root, "repeated"));
    assert(fixture->provision_requests - requests ==
           number(root, "repeat_requests"));
  }
  match(root, "request_operation", fixture->provision_operation,
        strlen(fixture->provision_operation));
  match(root, "request_body", fixture->provision_body,
        strlen(fixture->provision_body));
  fixture->pref.commit = (h2_app_test_fault_t){0};
  assert(h2_pal_pref_open(&fixture->pref.api, "compatibility",
                          H2_PAL_PREF_OPEN_READ_ONLY, &namespace) == 0);
  void* stored = NULL;
  size_t stored_length = 0u;
  assert(namespace->get_blob(namespace, &fixture->memory.api,
                             "device_certificate", &stored,
                             &stored_length) == 0);
  match(root, "saved", stored, stored_length);
  h2_pal_mem_free(&fixture->memory.api, stored);
  assert(namespace->close(namespace) == 0);
  assert(h2_alipay_independent_release() == 0);
  fixture->config = previous;
  fixture->provision_mode = fixture->network_error = false;
}

static yyjson_val* receive_event(const char* type) {
  yyjson_val* event = yyjson_arr_get(receive_events, receive_event_index++);
  assert(event != NULL &&
         yyjson_equals_str(yyjson_obj_get(event, "event"), type));
  h2_alipay_client_payment_t diagnostics;
  assert(h2_alipay_independent_last_payment_result(&diagnostics) == 0);
  return event;
}

static int receive_send(const uint8_t* data, uint32_t length) {
  if (receive_events == NULL) return receive_callback_result;
  yyjson_val* event = receive_event("send");
  match(event, "data", data, length);
  return receive_callback_result;
}

static int receive_direct(int manufacturer, const uint8_t* data,
                          uint32_t length, const uint8_t* uuid,
                          uint32_t uuid_length, uint8_t quick) {
  if (receive_events == NULL) return receive_callback_result;
  yyjson_val* event = receive_event("direct");
  assert(manufacturer == integer(event, "manufacturer"));
  assert(quick == number(event, "quick"));
  match(event, "data", data, length);
  match(event, "uuid", uuid, uuid_length);
  return receive_callback_result;
}

static int receive_status(alipay_payment_status_t* status) {
  if (receive_events == NULL) return receive_callback_result;
  yyjson_val* event = receive_event("status");
  assert((int)status->status == integer(event, "status"));
  assert((int)status->code == integer(event, "error"));
  match(event, "amount", status->amount, sizeof(status->amount));
  return receive_callback_result;
}

static int receive_notify(bool first, bool last, const uint8_t* data,
                          uint32_t length) {
  if (receive_events == NULL) return receive_callback_result;
  yyjson_val* event = receive_event("notify");
  assert(first == boolean(event, "first") && last == boolean(event, "last"));
  match(event, "data", data, length);
  return receive_callback_result;
}

static int synthetic_device_verify(void* user,
                                   const h2_pal_p256_public_key_t* public_key,
                                   const uint8_t* message, size_t length,
                                   const h2_pal_p256_signature_t* signature) {
  (void)user;
  assert(public_key != NULL && message != NULL && length == 64u &&
         signature != NULL);
  return H2_PAL_OK;
}

static int controlled_issuer_verify(void* user,
                                    const h2_pal_p256_public_key_t* public_key,
                                    const uint8_t* message, size_t length,
                                    const h2_pal_p256_signature_t* signature) {
  (void)user;
  assert(public_key != NULL && message != NULL && length == 64u);
  h2_pal_p256_private_key_t private_key = {0};
  memset(private_key.bytes, 0x42, sizeof(private_key.bytes));
  h2_pal_p256_keypair_t issuer;
  int rc = h2_pal_crypto_p256_keypair_from_private(h2_wolfcrypt_crypto_api(),
                                                   &private_key, &issuer);
  if (rc == 0)
    rc = h2_pal_crypto_ecdsa_p256_sha256_verify(h2_wolfcrypt_crypto_api(),
                                                &issuer.public_key, message,
                                                length, signature);
  h2_alipay_client_wipe(&issuer, sizeof(issuer));
  return rc;
}

static int frame_notify(bool first, bool last, const uint8_t* data,
                        uint32_t length) {
  assert(frame_offset < frame_length);
  size_t remaining = frame_length - frame_offset;
  size_t size = remaining < frame_mtu - 2u ? remaining : frame_mtu - 2u;
  assert(length == size + 2u && first == (frame_packet == 0u));
  assert(last == (remaining <= frame_mtu));
  assert(memcmp(data, frame_expected + frame_offset, size) == 0);
  assert(data[size] == frame_packet);
  assert(data[size + 1u] == (frame_length + frame_mtu - 3u) / (frame_mtu - 2u));
  h2_alipay_client_payment_t diagnostics;
  assert(h2_alipay_independent_last_payment_result(&diagnostics) == 0);
  frame_offset += size;
  ++frame_packet;
  return 17;
}

static void tap_ble_case(fixture_t* fixture, yyjson_val* root) {
  assert(h2_alipay_independent_release() == 0);
  bool issuer = yyjson_is_true(yyjson_obj_get(root, "controlled_issuer"));
  if (issuer) {
    fixture->crypto_vtable.ecdsa_p256_sha256_verify = controlled_issuer_verify;
    assert(decode(root, "private_key", controlled_private_key.bytes,
                  sizeof(controlled_private_key.bytes)) == 32u);
  }
  assert(h2_alipay_independent_configure(&fixture->config,
                                         &fixture->sync.api) == 0);
  assert(alipay_pre_init() == 0);
  store(fixture, root, true);
  receive_events = NULL;
  receive_callback_result = 0;
  const alipay_nfc_payment_callback_t callbacks = {
      .send_data_handle = receive_send,
      .notify_payment_result_handle = receive_status,
      .ble_broadcast_direct_pay_data_handle = receive_direct,
      .ble_notify_data_handle = receive_notify};
  alipay_nfc_init_callbacks(callbacks);
  assert(alipay_nfc_setup_multi_arch(0u) == 0);
  const char* url = boolean(root, "vip")
                        ? "x?scheme=x%3Fs%3Dvip%26v%3Da~b~c~"
                          "0123456789abcdef_SIG_a_b_c_d_3~e"
                        : "x?scheme=x%3FpageParams%3D%7B%22k%22%3A%22"
                          "0123456789abcdef_SIG_a_b_c_d_3%22%7D";
  uint8_t amount[16];
  assert(h2_pal_time_set_wall_ms(&fixture->time.api, 1794449000000u) == 0);
  assert(alipay_nfc_recv_data_handle_with_context(
             NFC_PAY_READER_CONTEXT_AWAKE, (const uint8_t*)url,
             (uint32_t)strlen(url), amount) == 0);
  alipay_npay_set_ble_mtu_size((uint32_t)number(root, "mtu"));
  uint8_t input[512];
  size_t length = decode(root, "input", input, sizeof(input));
  size_t chunk = (size_t)number(root, "chunk"), offset = 0u, count = 0u;
  assert(chunk != 0u);
  uint32_t results[64];
  receive_events = yyjson_obj_get(root, "events");
  receive_event_index = 0u;
  while (offset < length) {
    size_t size = length - offset;
    if (size > chunk) size = chunk;
    assert(count < 63u);
    results[count++] = (uint32_t)alipay_npay_ble_recv_data_handle(
        input + offset, (uint32_t)size);
    offset += size;
  }
  results[count++] =
      (uint32_t)alipay_npay_ble_recv_data_handle(input, (uint32_t)length);
  assert(receive_event_index == yyjson_arr_size(receive_events));
  match(root, "output", results, count * sizeof(results[0]));
  receive_events = NULL;
  fixture->crypto_vtable.ecdsa_p256_sha256_verify =
      h2_wolfcrypt_crypto_api()->vtable->ecdsa_p256_sha256_verify;
}

static void tap_cipher_core_case(fixture_t* fixture, yyjson_val* root) {
  assert(h2_alipay_independent_release() == 0);
  store(fixture, root, true);
  assert(decode(root, "private_key", controlled_private_key.bytes,
                sizeof(controlled_private_key.bytes)) == 32u);
  assert(h2_pal_time_set_wall_ms(&fixture->time.api,
                                 number(root, "epoch") * 1000u) == 0);
  char public_key[129] = {0}, token[64] = {0};
  assert(decode(root, "input", public_key, sizeof(public_key) - 1u) == 128u);
  (void)decode(root, "token", token, sizeof(token) - 1u);
  h2_alipay_tap_t* tap = NULL;
  assert(h2_alipay_tap_open(&fixture->config, &tap) == 0);
  uint8_t output[256];
  memset(output, 0xa5, sizeof(output));
  size_t length = 777u;
  h2_alipay_client_payment_t diagnostics;
  int rc = h2_alipay_tap_connected_reply(tap, public_key, token,
                                         boolean(root, "vip"), output, &length,
                                         &diagnostics);
  assert(rc == integer(root, "result"));
  assert(length == number(root, "length"));
  uint8_t expected_output[256];
  assert(decode(root, "output", expected_output, sizeof(expected_output)) ==
         sizeof(expected_output));
  for (size_t index = 0u; index < sizeof(output); ++index) {
    if (expected_output[index] == output[index]) continue;
    fprintf(stderr,
            "Connected cipher case=%llu byte=%zu actual=%02x expected=%02x\n",
            (unsigned long long)number(root, "case"), index, output[index],
            expected_output[index]);
    break;
  }
  match(root, "output", output, sizeof(output));
  h2_alipay_tap_close(tap);
  h2_pal_result_t (*verify)(void*, const h2_pal_p256_public_key_t*,
                            const uint8_t*, size_t,
                            const h2_pal_p256_signature_t*) =
      fixture->crypto_vtable.ecdsa_p256_sha256_verify;
  fixture->crypto_vtable.ecdsa_p256_sha256_verify = synthetic_device_verify;
  assert(h2_alipay_independent_configure(&fixture->config,
                                         &fixture->sync.api) == 0);
  assert(alipay_pre_init() == 0);
  receive_events = NULL;
  const alipay_nfc_payment_callback_t callbacks = {
      .send_data_handle = receive_send,
      .notify_payment_result_handle = receive_status,
      .ble_broadcast_direct_pay_data_handle = receive_direct,
      .ble_notify_data_handle = frame_notify};
  alipay_nfc_init_callbacks(callbacks);
  char url[256];
  int url_length =
      snprintf(url, sizeof(url),
               boolean(root, "vip")
                   ? "x?scheme=x%%3Fs%%3Dvip%%26v%%3Da~b~c~%s_SIG_a_b_c_d_3~e"
                   : "x?scheme=x%%3FpageParams%%3D%%7B%%22k%%22%%3A%%22"
                     "%s_SIG_a_b_c_d_3%%22%%7D",
               token);
  assert(url_length > 0 && (size_t)url_length < sizeof(url));
  uint8_t amount[16];
  assert(alipay_nfc_recv_data_handle_with_context(
             NFC_PAY_READER_CONTEXT_AWAKE, (const uint8_t*)url,
             (uint32_t)url_length, amount) == 0);
  uint8_t frame[512] = {0};
  char first_field[10];
  size_t first_length = 1u + (size_t)number(root, "case") % 8u;
  memset(first_field, 'A', first_length);
  first_field[first_length] = '\0';
  char signature[131];
  memset(signature, '0', 130u);
  signature[1] = ':';
  signature[130] = '\0';
  int payload_length =
      snprintf((char*)frame + 10u, sizeof(frame) - 10u, "%s,B,C,%s,%s",
               first_field, public_key, signature);
  assert(payload_length > 0 && (size_t)payload_length < sizeof(frame) - 10u);
  memcpy(frame, "BT0000", 6u);
  for (size_t index = 0u; index < 4u; ++index)
    frame[6u + index] =
        (uint8_t)((unsigned)payload_length >> (24u - index * 8u));
  size_t prefix_length = first_length + 5u;
  memcpy(frame_expected, frame, prefix_length + 10u);
  memcpy(frame_expected + 10u + prefix_length, expected_output, length);
  size_t expected_payload_length = prefix_length + length;
  for (size_t index = 0u; index < 4u; ++index)
    frame_expected[6u + index] =
        (uint8_t)(expected_payload_length >> (24u - index * 8u));
  frame_length = expected_payload_length + 10u;
  frame_offset = frame_packet = 0u;
  frame_mtu = number(root, "case") % 3u == 0u ? 100u : 20u;
  alipay_npay_set_ble_mtu_size(frame_mtu == 100u ? 101u : frame_mtu);
  size_t baseline = fixture->memory.live_blocks;
  assert(alipay_npay_ble_recv_data_handle(frame, 10u) == 0);
  assert(frame_packet == 0u && fixture->memory.live_blocks == baseline);
  size_t processing_start = fixture->memory.calls;
  assert(alipay_npay_ble_recv_data_handle(frame + 10u,
                                          (uint32_t)payload_length) == 0);
  size_t allocations = fixture->memory.calls - processing_start;
  assert(frame_offset == frame_length && frame_packet > 0u);
  assert(fixture->memory.live_blocks == baseline);
  frame_offset = frame_packet = 0u;
  fixture->memory.fail_at = fixture->memory.calls + 1u;
  assert(alipay_npay_ble_recv_data_handle(frame, 10u) == 30);
  fixture->memory.fail_at = 0u;
  assert(frame_packet == 0u && fixture->memory.live_blocks == baseline);
  assert(alipay_npay_ble_recv_data_handle(frame, 10u) == 0);
  assert(frame_offset == frame_length &&
         fixture->memory.live_blocks == baseline);
  for (size_t failure = 1u; failure <= allocations; ++failure) {
    frame_offset = frame_packet = 0u;
    fixture->memory.fail_at = fixture->memory.calls + failure;
    int failed = alipay_npay_ble_recv_data_handle(frame, 10u);
    fixture->memory.fail_at = 0u;
    assert(failed == 0 || failed == 30 || failed == 52 || failed == 99);
    assert(frame_offset == (failed == 0 ? frame_length : 0u));
    assert(fixture->memory.live_blocks == baseline &&
           fixture->pref.active_handles == 0u);
  }
  frame_offset = frame_packet = 0u;
  assert(alipay_npay_ble_recv_data_handle(frame, 10u) == 0);
  assert(frame_offset == frame_length &&
         fixture->memory.live_blocks == baseline);
  assert(h2_alipay_independent_release() == 0);
  fixture->crypto_vtable.ecdsa_p256_sha256_verify = verify;
}

static void tap_receive_case(fixture_t* fixture, yyjson_val* root) {
  assert(h2_alipay_independent_release() == 0);
  assert(h2_alipay_independent_configure(&fixture->config,
                                         &fixture->sync.api) == 0);
  assert(alipay_pre_init() == 0);
  store(fixture, root, boolean(root, "initial_bound"));
  const alipay_nfc_payment_callback_t callbacks = {
      .send_data_handle = receive_send,
      .notify_payment_result_handle = receive_status,
      .ble_broadcast_direct_pay_data_handle = receive_direct};
  alipay_nfc_init_callbacks(callbacks);
  assert(alipay_npay_ble_broadcast_init() == 0);
  assert(alipay_nfc_setup_multi_arch((uint8_t)number(root, "multi")) == 0);
  h2_pal_pref_namespace_t* namespace = NULL;
  assert(h2_pal_pref_open(&fixture->pref.api, "compatibility",
                          H2_PAL_PREF_OPEN_READ_WRITE, &namespace) == 0);
  assert(namespace->set_u32(namespace, "npay_quick",
                            (uint32_t)number(root, "quick")) == 0);
  assert(namespace->set_u32(namespace, "npay_show", 0u) == 0);
  assert(namespace->commit(namespace) == 0 && namespace->close(namespace) == 0);
  assert(h2_pal_time_set_wall_ms(&fixture->time.api, 1794449000000u) == 0);
  uint8_t input[272], amount[16];
  size_t input_length = decode(root, "input", input, sizeof(input));
  memset(amount, 0xa5, sizeof(amount));
  receive_events = yyjson_obj_get(root, "events");
  receive_event_index = 0u;
  receive_callback_result = integer(root, "callback_result");
  unsigned context = (unsigned)number(root, "context");
  int rc = context == 5u
               ? alipay_nfc_recv_data_handle(input, (uint32_t)input_length)
               : alipay_nfc_recv_data_handle_with_context(
                     (alipay_nfc_pay_reader_context_t)context, input,
                     (uint32_t)input_length, amount);
  assert(rc == integer(root, "result"));
  assert(receive_event_index == yyjson_arr_size(receive_events));
  match(root, "output", amount, sizeof(amount));
  char token[32] = {0}, code[32] = {0};
  size_t token_length = decode(root, "token", token, sizeof(token) - 1u);
  size_t code_length = decode(root, "paycode", code, sizeof(code) - 1u);
  uint8_t plain[32];
  memset(plain, 0xa5, sizeof(plain));
  uint32_t plain_length = UINT32_MAX;
  int plain_result = alipay_get_plain_paycode_with_nfc_token(
      (uint8_t*)token, (uint32_t)token_length, plain, &plain_length);
  if (code_length == 0u) {
    assert(plain_result == 99 && plain_length == UINT32_MAX);
    for (size_t index = 0u; index < sizeof(plain); ++index)
      assert(plain[index] == 0xa5u);
  } else {
    assert(plain_result == 0 && plain_length == code_length);
    assert(memcmp(plain, code, code_length) == 0);
    for (size_t index = code_length; index < sizeof(plain); ++index)
      assert(plain[index] == 0xa5u);
  }
  assert(h2_pal_pref_open(&fixture->pref.api, "compatibility",
                          H2_PAL_PREF_OPEN_READ_ONLY, &namespace) == 0);
  uint32_t show = 0u;
  assert(namespace->get_u32(namespace, "npay_show", &show) == 0);
  assert(show == number(root, "show"));
  assert(namespace->close(namespace) == 0);
  receive_events = NULL;
}

static void tap_write_case(fixture_t* fixture, yyjson_val* root) {
  static const char token[] = "0123456789abcdef";
  static const uint32_t offsets[] = {0u,      1u,      2u,      3u,
                                     604800u, 604801u, 604802u, 604803u};
  assert(h2_alipay_independent_release() == 0);
  assert(h2_alipay_independent_configure(&fixture->config,
                                         &fixture->sync.api) == 0);
  assert(alipay_pre_init() == 0);
  store(fixture, root, true);
  assert(h2_alipay_independent_set_nfc_context(token, "SYNTHETIC_SQUEEZE",
                                               "281234567890123456") == 0);
  h2_pal_pref_namespace_t* namespace = NULL;
  assert(h2_pal_pref_open(&fixture->pref.api, "compatibility",
                          H2_PAL_PREF_OPEN_READ_WRITE, &namespace) == 0);
  assert(namespace->set_u32(namespace, "npay_show",
                            (uint32_t)number(root, "initial_show")) == 0);
  assert(namespace->set_u32(namespace, "npay_quick", 0u) == 0);
  char history[128];
  size_t history_length =
      decode(root, "initial_history", history, sizeof(history));
  if (history_length != 0u)
    assert(namespace->set_blob(namespace, "npay_guide", history,
                               history_length) == 0);
  assert(namespace->commit(namespace) == 0 && namespace->close(namespace) == 0);
  assert(alipay_nfc_setup_multi_arch((uint8_t)number(root, "multi")) == 0);
  uint8_t input[256];
  size_t length = decode(root, "input", input, sizeof(input));
  assert(h2_pal_time_set_wall_ms(&fixture->time.api, 1794449000000u) == 0);
  int rc = alipay_set_nfc_write_result_with_nfc_data(
      input, (uint32_t)length, (uint8_t)number(root, "write_value"));
  if (rc != integer(root, "result"))
    fprintf(stderr, "write case=%llu result=%d/%d\n",
            (unsigned long long)number(root, "case"), rc,
            integer(root, "result"));
  assert(rc == integer(root, "result"));
  uint8_t output[12];
  memset(output, 0xa5, sizeof(output));
  assert(alipay_get_nfc_write_result_with_nfc_token(
             (uint8_t*)token, sizeof(token) - 1u, &output[0]) ==
         integer(root, "length"));
  assert(alipay_get_nfc_write_result_with_nfc_token((uint8_t*)"OTHER_TOKEN",
                                                    11u, &output[1]) == 99);
  output[2] = alipay_npay_get_quick_pay_show_flag();
  for (size_t index = 0u; index < 8u; ++index) {
    assert(h2_pal_time_set_wall_ms(
               &fixture->time.api,
               (UINT64_C(1794449000) + offsets[index]) * 1000u) == 0);
    output[3u + index] = alipay_npay_need_show_quick_open_guide();
  }
  output[11] = alipay_npay_get_quick_pay_flag();
  uint8_t expected[sizeof(output)];
  assert(decode(root, "output", expected, sizeof(expected)) ==
         sizeof(expected));
  if (memcmp(output, expected, sizeof(output)) != 0) {
    fprintf(stderr, "write case=%llu output:",
            (unsigned long long)number(root, "case"));
    for (size_t index = 0u; index < sizeof(output); ++index)
      fprintf(stderr, " %u/%u", output[index], expected[index]);
    fputc('\n', stderr);
  }
  match(root, "output", output, sizeof(output));
  assert(h2_pal_pref_open(&fixture->pref.api, "compatibility",
                          H2_PAL_PREF_OPEN_READ_ONLY, &namespace) == 0);
  uint32_t show = 0u;
  assert(namespace->get_u32(namespace, "npay_show", &show) == 0);
  assert(show == number(root, "final_show"));
  void* saved = NULL;
  size_t size = 0u;
  int found = namespace->get_blob(namespace, &fixture->memory.api, "npay_guide",
                                  &saved, &size);
  assert(found == 0 || found == H2_PAL_ERR_NOT_FOUND);
  match(root, "final_history", saved, size);
  h2_pal_mem_free(&fixture->memory.api, saved);
  assert(namespace->close(namespace) == 0);
}

static void transit_match_result(yyjson_val* root, int result) {
  if (result != integer(root, "result")) {
    fprintf(stderr, "transit entry=%s case=%d result=%d/%d\n",
            yyjson_get_str(yyjson_obj_get(root, "entry")),
            integer(root, "case"), result, integer(root, "result"));
    assert(false);
  }
}

static void retail_mac_case(fixture_t* fixture, yyjson_val* root) {
  uint8_t key[16], input[32], output[32];
  assert(decode(root, "mac_key", key, sizeof(key)) == sizeof(key));
  size_t length = decode(root, "input", input, sizeof(input));
  memset(output, 0xa5, sizeof(output));
  assert(integer(root, "result") == 0 && number(root, "sdk_length") == 4u);
  assert(h2_crypto_iso9797_alg3_mac(fixture->config.crypto, key, input, length,
                                    output) == 0);
  match(root, "output", output, sizeof(output));
}

static void transit_match_details(yyjson_val* root) {
  transit_card_list_result_e card = -9;
  transit_transit_code_result_e code = -9;
  alipay_get_last_card_list_result(&card);
  alipay_get_last_transit_code_result(&code);
  if ((int)card != integer(root, "card_result") ||
      (int)code != integer(root, "code_result")) {
    fprintf(stderr, "transit entry=%s case=%d detail=%d/%d code=%d/%d\n",
            yyjson_get_str(yyjson_obj_get(root, "entry")),
            integer(root, "case"), (int)card, integer(root, "card_result"),
            (int)code, integer(root, "code_result"));
    assert(false);
  }
}

static void transit_case(fixture_t* fixture, yyjson_val* root) {
  store(fixture, root, true);
  assert(h2_pal_time_set_wall_ms(&fixture->time.api, 1794449000000u) == 0);
  assert(alipay_init_transit_module() == 0);
  assert(alipay_init_transit_module() == 0);
  fixture->union_script = yyjson_obj_get(root, "rpc");
  assert(yyjson_is_arr(fixture->union_script));
  fixture->union_step = 0u;
  const char* entry = yyjson_get_str(yyjson_obj_get(root, "entry"));
  if (strcmp(entry, "cards") == 0) {
    uint8_t output[496];
    uint32_t length = 480u, count = 0u;
    if (boolean(root, "cached")) {
      memset(output, 0xa5, sizeof(output));
      assert(alipay_transit_get_card_list_online((void*)output, &length,
                                                 &count) == 0);
    }
    length = (uint32_t)number(root, "capacity");
    count = 0x42424242u;
    memset(output, 0xa5, sizeof(output));
    int result =
        alipay_transit_get_card_list_online((void*)output, &length, &count);
    transit_match_result(root, result);
    assert(length == number(root, "length") && count == number(root, "count"));
    match(root, "output", output, sizeof(output));
    transit_match_details(root);
    length = (uint32_t)number(root, "capacity");
    count = 0x42424242u;
    memset(output, 0xa5, sizeof(output));
    result =
        alipay_transit_get_card_list_offline((void*)output, &length, &count);
    if (result != integer(root, "offline_result"))
      fprintf(stderr, "transit offline case=%d result=%d/%d\n",
              integer(root, "case"), result, integer(root, "offline_result"));
    assert(result == integer(root, "offline_result"));
    assert(length == number(root, "offline_length") &&
           count == number(root, "offline_count"));
    match(root, "offline", output, sizeof(output));
    transit_card_list_result_e detail;
    alipay_get_last_card_list_result(&detail);
    assert((int)detail == integer(root, "offline_detail"));
  } else if (strcmp(entry, "activity") == 0) {
    unsigned cached = (unsigned)number(root, "count");
    bool active = false;
    if (cached != 0u)
      assert(alipay_transit_get_activity_status_online(&active) == 0);
    active = number(root, "case") % 2u != 0u;
    int result = alipay_transit_get_activity_status_online(&active);
    transit_match_result(root, result);
    match(root, "output", &active, sizeof(active));
    transit_match_details(root);
    active = true;
    result = alipay_transit_get_activity_status_offline(&active);
    if (result != integer(root, "offline_result"))
      fprintf(stderr, "transit activity offline case=%d result=%d/%d\n",
              integer(root, "case"), result, integer(root, "offline_result"));
    assert(result == integer(root, "offline_result"));
    assert(active == boolean(root, "offline_active"));
  } else if (strcmp(entry, "update") == 0) {
    uint8_t error[272];
    memset(error, 0xa5, sizeof(error));
    int result =
        alipay_transit_update_card_data("CARD_NO", "TYPE", (void*)error, 256u);
    transit_match_result(root, result);
    match(root, "output", error, sizeof(error));
    transit_match_details(root);
    alipay_transit_card_status_t status;
    memset(&status, 0xa5, sizeof(status));
    result = alipay_transit_check_card_status("CARD_NO", "TYPE", &status);
    assert(result == integer(root, "status_result"));
    assert(status.is_exists == boolean(root, "exists") &&
           status.expire_timestamp == integer(root, "expiry") &&
           status.remain_use_count == integer(root, "remaining"));
    memset(&status, 0xa5, sizeof(status));
    result = alipay_transit_check_card_status("CARD_NO", "type", &status);
    assert(result == integer(root, "other_case_result"));
    assert(status.is_exists == boolean(root, "other_case_exists") &&
           status.expire_timestamp == integer(root, "other_case_expiry") &&
           status.remain_use_count == integer(root, "other_case_remaining"));
  } else if (strcmp(entry, "native") == 0 || strcmp(entry, "native_sm2") == 0 ||
             strcmp(entry, "native_yct") == 0 ||
             strcmp(entry, "native_rct") == 0) {
    assert(h2_pal_time_set_wall_ms(&fixture->time.api,
                                   number(root, "now") * 1000u) == 0);
    uint8_t cards[240];
    memset(cards, 0xa5, sizeof(cards));
    uint32_t card_length = sizeof(cards), count = 0u;
    assert(alipay_transit_get_card_list_online((void*)cards, &card_length,
                                               &count) == 0);
    uint8_t output[528], error[272];
    memset(output, 0xa5, sizeof(output));
    memset(error, 0xa5, sizeof(error));
    uint32_t length = 512u;
    int result = alipay_transit_get_TransitCode("CARD_NO", "TYPE", output,
                                                &length, (void*)error, 256u);
    transit_match_result(root, result);
    if (length != number(root, "length"))
      fprintf(stderr, "transit native entry=%s case=%d length=%u/%u\n", entry,
              integer(root, "case"), length, (unsigned)number(root, "length"));
    assert(length == number(root, "length"));
    match(root, "output", output, sizeof(output));
    match(root, "error", error, sizeof(error));
    transit_match_details(root);
    alipay_tansit_CardBaseVO_t info;
    memset(&info, 0xa5, sizeof(info));
    assert(alipay_transit_get_the_last_transcode_info(&info) ==
           integer(root, "last_info_result"));
    match(root, "info", &info, sizeof(info));
    char title[56];
    memset(title, 0xa5, sizeof(title));
    memset(output, 0xa5, sizeof(output));
    length = 512u;
    assert(alipay_transit_get_the_last_transitCode(title, output, &length,
                                                   (void*)error, 256u) ==
           integer(root, "last_code_result"));
    assert(length == number(root, "last_length"));
    match(root, "title", title, sizeof(title));
    match(root, "last_output", output, sizeof(output));
    alipay_transit_card_status_t status;
    memset(&status, 0xa5, sizeof(status));
    assert(alipay_transit_check_card_status("CARD_NO", "TYPE", &status) ==
           integer(root, "status_result"));
    assert(status.is_exists == boolean(root, "exists") &&
           status.expire_timestamp == integer(root, "expiry") &&
           status.remain_use_count == integer(root, "remaining"));
  } else {
    assert(strcmp(entry, "code") == 0);
    uint8_t output[528], error[272];
    memset(output, 0xa5, sizeof(output));
    memset(error, 0xa5, sizeof(error));
    uint32_t length = (uint32_t)number(root, "capacity");
    char* type = number(root, "case") >= 8u ? "S0310001" : "TYPE";
    int result = alipay_transit_get_TransitCode("CARD_NO", type, output,
                                                &length, (void*)error, 256u);
    transit_match_result(root, result);
    assert(length == number(root, "length"));
    match(root, "output", output, sizeof(output));
    match(root, "error", error, sizeof(error));
    transit_match_details(root);
    alipay_tansit_CardBaseVO_t info;
    memset(&info, 0xa5, sizeof(info));
    assert(alipay_transit_get_the_last_transcode_info(&info) ==
           integer(root, "last_info_result"));
    match(root, "info", &info, sizeof(info));
    char title[56];
    memset(title, 0xa5, sizeof(title));
    assert(alipay_transit_get_the_last_transitCode(title, output, &length,
                                                   (void*)error, 256u) ==
           integer(root, "last_code_result"));
    match(root, "title", title, sizeof(title));
  }
  assert(fixture->union_step == yyjson_arr_size(fixture->union_script));
  fixture->union_script = NULL;
}

int main(int argc, char** argv) {
  assert(argc == 2);
  FILE* input = fopen(argv[1], "r");
  assert(input != NULL);
  fixture_t fixture;
  initialize(&fixture);
  char line[32768];
  unsigned payer_cases = 0u, query_cases = 0u, binding_cases = 0u;
  unsigned account_cases = 0u;
  unsigned unbind_cases = 0u;
  unsigned collection_cases = 0u;
  unsigned remote_cases = 0u;
  unsigned balance_cases = 0u;
  unsigned bitmap_query_cases = 0u;
  unsigned income_cases = 0u;
  unsigned scan_cases = 0u;
  unsigned tap_cipher_core_cases = 0u;
  unsigned controlled_issuer_cases = 0u;
  unsigned sync_cases = 0u;
  unsigned migration_cases = 0u;
  unsigned provision_cases = 0u;
  unsigned transit_cases = 0u;
  unsigned retail_mac_cases = 0u;
  while (fgets(line, sizeof(line), input) != NULL) {
    assert(strchr(line, '\n') != NULL);
    yyjson_doc* document = yyjson_read(line, strlen(line), 0u);
    assert(document != NULL);
    yyjson_val* root = yyjson_doc_get_root(document);
    yyjson_val* kind = yyjson_obj_get(root, "kind");
    if (yyjson_equals_str(kind, "crypto_retail_mac")) {
      retail_mac_case(&fixture, root);
      ++retail_mac_cases;
    } else if (yyjson_equals_str(kind, "transit")) {
      transit_case(&fixture, root);
      ++transit_cases;
    } else if (yyjson_equals_str(kind, "migration")) {
      migration_case(&fixture, root);
      ++migration_cases;
    } else if (yyjson_equals_str(kind, "sync")) {
      sync_case(&fixture, root);
      ++sync_cases;
    } else if (yyjson_equals_str(kind, "payer")) {
      payer(&fixture, root);
      ++payer_cases;
    } else if (yyjson_equals_str(kind, "query")) {
      query(&fixture, root);
      ++query_cases;
    } else if (yyjson_equals_str(kind, "collection")) {
      collection(&fixture, root);
      ++collection_cases;
    } else if (yyjson_equals_str(kind, "query_bitmap")) {
      query_bitmap(&fixture, root);
      ++bitmap_query_cases;
    } else if (yyjson_equals_str(kind, "income")) {
      income(&fixture, root);
      ++income_cases;
    } else if (yyjson_is_str(kind) &&
               strncmp(yyjson_get_str(kind), "dzs_", 4u) == 0) {
      scan_case(&fixture, root, kind);
      ++scan_cases;
    } else if (yyjson_equals_str(kind, "union_parse") ||
               yyjson_equals_str(kind, "union_confirm")) {
      union_case(&fixture, root, yyjson_equals_str(kind, "union_parse"));
      ++scan_cases;
    } else if (yyjson_equals_str(kind, "tap_setting")) {
      tap_setting_case(&fixture, root);
      ++scan_cases;
    } else if (yyjson_equals_str(kind, "tap_write")) {
      tap_write_case(&fixture, root);
      ++scan_cases;
    } else if (yyjson_equals_str(kind, "tap_static")) {
      tap_static_case(&fixture, root);
      ++scan_cases;
    } else if (yyjson_equals_str(kind, "tap_receive")) {
      tap_receive_case(&fixture, root);
      ++scan_cases;
    } else if (yyjson_equals_str(kind, "tap_ble")) {
      tap_ble_case(&fixture, root);
      ++scan_cases;
    } else if (yyjson_equals_str(kind, "tap_ble_issuer")) {
      tap_ble_case(&fixture, root);
      ++controlled_issuer_cases;
    } else if (yyjson_equals_str(kind, "tap_cipher_core")) {
      tap_cipher_core_case(&fixture, root);
      ++tap_cipher_core_cases;
    } else if (yyjson_equals_str(kind, "provision")) {
      provision_case(&fixture, root);
      ++provision_cases;
    } else if (yyjson_is_str(kind) &&
               strncmp(yyjson_get_str(kind), "tap_", 4u) == 0) {
      tap_case(&fixture, root, kind);
      ++scan_cases;
    } else if (yyjson_equals_str(kind, "binding_start")) {
      binding_start(&fixture, root);
      ++binding_cases;
    } else if (yyjson_equals_str(kind, "account")) {
      account(root);
      ++account_cases;
    } else if (yyjson_equals_str(kind, "unbind")) {
      unbind(&fixture, root);
      ++unbind_cases;
    } else if (yyjson_equals_str(kind, "remote")) {
      remote(&fixture, root);
      ++remote_cases;
    } else if (yyjson_equals_str(kind, "balance")) {
      balance(&fixture, root);
      ++balance_cases;
    } else {
      assert(yyjson_equals_str(kind, "binding_query"));
      binding_query(&fixture, root);
      ++binding_cases;
    }
    yyjson_doc_free(document);
    assert(fixture.pref.active_handles == 0u);
  }
  assert(!ferror(input) && fclose(input) == 0);
  assert((payer_cases >= 1685u && query_cases >= 9u) || binding_cases >= 3u ||
         scan_cases >= 29u || sync_cases >= 70u || migration_cases >= 36u ||
         provision_cases >= 124u || transit_cases >= 500u);
  assert(h2_alipay_independent_release() == 0);
  assert(h2_app_test_pref_deinit(&fixture.pref) == 0);
  assert(fixture.memory.live_blocks == 0u);
  h2_wolfcrypt_crypto_deinit();
  printf(
      "{\"public_paycode_comparisons\":%u,\"public_query_comparisons\":%u,"
      "\"public_binding_comparisons\":%u,\"public_account_comparisons\":%u,"
      "\"public_unbind_comparisons\":%u,\"public_collection_comparisons\":%u,"
      "\"public_remote_comparisons\":%u,\"public_balance_comparisons\":%u,"
      "\"public_bitmap_query_comparisons\":%u,"
      "\"public_income_comparisons\":%u,"
      "\"public_scan_comparisons\":%u,"
      "\"public_sync_comparisons\":%u,"
      "\"public_migration_comparisons\":%u,"
      "\"tap_cipher_core_comparisons\":%u,"
      "\"public_provision_comparisons\":%u,"
      "\"public_transit_comparisons\":%u,"
      "\"retail_mac_comparisons\":%u,"
      "\"certificate_profile_fault_points\":%u,"
      "\"image_decode_comparisons\":0,"
      "\"controlled_issuer_ble_comparisons\":%u,"
      "\"failed\":0}\n",
      payer_cases, query_cases, binding_cases, account_cases, unbind_cases,
      collection_cases, remote_cases, balance_cases, bitmap_query_cases,
      income_cases, scan_cases, sync_cases, migration_cases,
      tap_cipher_core_cases, provision_cases, transit_cases, retail_mac_cases,
      certificate_fault_points, controlled_issuer_cases);
  return 0;
}
