#include "h2_alipay_client.h"
#include "h2_app_test_mem.h"
#include "h2_app_test_pref.h"
#include "h2_app_test_time.h"
#include "h2_wolfcrypt_crypto.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Entirely synthetic test identities, tokens and factors. No live service. */
static const char point_hex[] =
    "6b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c296"
    "4fe342e2fe1a7f9b8ee7eb4a7c0f9e162bce33576b315ececbb6406837bf51f5";

typedef struct fixture {
  h2_app_test_mem_t mem;
  h2_app_test_pref_t pref;
  h2_app_test_time_t time;
  h2_pal_http_api_t http;
  h2_alipay_client_config_t config;
  h2_alipay_client_result_t session;
  unsigned registrations, preparations, queries, finishes, frees;
  uint32_t entropy;
  bool pending;
  int query_rc, finish_rc;
  const char* query_body;
  const char* finish_body;
  char expected_token[129];
  char completed[2048];
} fixture_t;

static bool equal(h2_pal_http_str_t value, const char* text) {
  return value.len == strlen(text) && memcmp(value.data, text, value.len) == 0;
}

static int request(void* user, const h2_pal_http_request_t* request,
                   h2_pal_http_response_t* response) {
  fixture_t* f = user;
  const char* operation = NULL;
  size_t operation_length = 0u;
  for (size_t i = 0u; i < request->header_count; ++i)
    if (equal(request->headers[i].name, "Operation-Type")) {
      operation = request->headers[i].value.data;
      operation_length = request->headers[i].value.len;
    }
  assert(operation != NULL);
  const char* body = NULL;
  static const uint8_t name[] = {10, 7, 'D', 'E', 'V', 'I', 'C', 'E', '1'};
  size_t length = 0u;
  if (operation_length == strlen("iothub.internal.device.active")) {
    ++f->registrations;
    body = (const char*)name;
    length = sizeof(name);
  } else if (memcmp(operation + operation_length - 11u, "prepareBind", 11u) ==
             0) {
    ++f->preparations;
    body = "{\"success\":true,\"token\":\"SYNTHETIC_TOKEN\"}";
  } else if (memcmp(operation + operation_length - 13u, "queryBindInfo", 13u) ==
             0) {
    ++f->queries;
    char expected[256];
    int size = snprintf(expected, sizeof(expected),
                        "[{\"token\":\"%s\",\"operationMode\":\"NETWORK\"}]",
                        f->expected_token);
    assert(size > 0 && (size_t)size < sizeof(expected));
    assert(request->body_len == (size_t)size &&
           memcmp(request->body, expected, request->body_len) == 0);
    if (f->query_rc != 0) return f->query_rc;
    body = f->query_body != NULL ? f->query_body
           : f->pending          ? "{\"needQuery\":true}"
                                 : f->completed;
  } else {
    assert(memcmp(operation + operation_length - 10u, "finishBind", 10u) == 0);
    ++f->finishes;
    char expected[256];
    int size = snprintf(expected, sizeof(expected),
                        "[{\"token\":\"%s\",\"operationMode\":\"NETWORK\","
                        "\"seRes\":\"\",\"operateRes\":\"success\"}]",
                        f->expected_token);
    assert(size > 0 && (size_t)size < sizeof(expected));
    assert(request->body_len == (size_t)size &&
           memcmp(request->body, expected, request->body_len) == 0);
    if (f->finish_rc != 0) return f->finish_rc;
    body = f->finish_body != NULL ? f->finish_body : "{\"success\":true}";
  }
  if (length == 0u) length = strlen(body);
  assert(length <= request->response_buf_cap);
  memcpy(request->response_buf, body, length);
  response->status_code = 200;
  response->body = request->response_buf;
  response->body_len = length;
  return h2_pal_http_deliver_response_header(request, "Result-Status", 13u,
                                             "1000", 4u);
}

static void response_free(void* user, h2_pal_http_response_t* response) {
  ++((fixture_t*)user)->frees;
  h2_pal_http_response_reset(response);
}

static int entropy(void* user, uint8_t* out, size_t length) {
  uint32_t* state = user;
  for (size_t i = 0u; i < length; ++i) {
    *state = *state * 1664525u + 1013904223u;
    out[i] = (uint8_t)(*state >> 24u);
  }
  return H2_PAL_OK;
}

static void initialize(fixture_t* f) {
  memset(f, 0, sizeof(*f));
  h2_app_test_mem_init(&f->mem, NULL);
  assert(h2_app_test_pref_init(&f->pref, &f->mem.api) == 0);
  h2_app_test_time_init(&f->time, 0u);
  assert(h2_pal_time_set_wall_ms(&f->time.api, 1794449000000u) == 0);
  f->entropy = 1u;
  const h2_wolfcrypt_crypto_config_t crypto = {&f->entropy, entropy};
  assert(h2_wolfcrypt_crypto_init(&crypto) == 0);
  static const h2_pal_http_vtable_t http = {request, response_free};
  f->http = (h2_pal_http_api_t){f, &http};
  f->config = (h2_alipay_client_config_t){
      .mem = &f->mem.api,
      .time = &f->time.api,
      .http = &f->http,
      .gateway_url = "https://iotapi.alipay.com/mgw.htm",
      .pref = &f->pref.api,
      .crypto = h2_wolfcrypt_crypto_api(),
      .identity = {"SYNTHETIC_SN", "SYNTHETIC_IMEI", "12:34:56:78:9A:BC",
                   "TEST"},
      .company = "TEST",
      .pref_namespace = "independent_test",
      .product_type = 3u,
      .feature = 16385u,
      .compile_timestamp_s = 1794449000u,
  };
  memcpy(f->expected_token, "SYNTHETIC_TOKEN", sizeof("SYNTHETIC_TOKEN"));
  (void)snprintf(
      f->completed, sizeof(f->completed),
      "{\"needQuery\":false,\"success\":true,\"tid\":\"TEST_TID\","
      "\"userId\":\"TEST_UID\",\"nickName\":\"TEST_NAME\",\"loginId\":\"TEST_LOGIN\","
      "\"serverTime\":\"1794449000000\",\"encryptedFactors\":\"%s&"
      "000102030405060708090a0b0c0d0e0f\"}",
      point_hex);
  assert(h2_alipay_client_get_binding_code(&f->config, &f->session) == 0);
  assert(f->registrations == 1u && f->preparations == 1u);
}

static void destroy(fixture_t* f) {
  h2_alipay_client_result_clear(&f->session);
  assert(f->pref.active_handles == 0u);
  assert(h2_app_test_pref_deinit(&f->pref) == 0);
  assert(f->mem.live_blocks == 0u);
  h2_wolfcrypt_crypto_deinit();
}

static void complete(fixture_t* f) {
  h2_alipay_client_binding_t binding;
  assert(h2_alipay_client_query_binding_result(&f->config, &f->session,
                                               &binding) == 0);
  assert(binding.state == H2_ALIPAY_CLIENT_COMPLETE);
  assert(binding.tid_length == 8u && binding.uid_length == 8u &&
         binding.seed_length == 16u && binding.shared_key_length == 32u);
  assert(f->session.token[0] == '\0' && f->session.length == 0u);
  assert(memcmp(f->session.private_key.bytes, (uint8_t[32]){0}, 32u) == 0);
  assert(h2_alipay_client_binding_snapshot(&f->config, &binding) == 0);
  assert(binding.state == H2_ALIPAY_CLIENT_COMPLETE);
}

static void sized_response(fixture_t* f, size_t seed_length, bool max_text) {
  assert(seed_length <= 129u);
  char seed[259], tid[65], uid[65], nickname[129], login[129];
  static const char hex[] = "0123456789abcdef";
  for (size_t i = 0u; i < seed_length; ++i) {
    seed[i * 2u] = hex[(i >> 4u) & 15u];
    seed[i * 2u + 1u] = hex[i & 15u];
  }
  seed[seed_length * 2u] = '\0';
  memset(tid, 'T', max_text ? 64u : 8u);
  tid[max_text ? 64u : 8u] = '\0';
  memset(uid, 'U', max_text ? 64u : 8u);
  uid[max_text ? 64u : 8u] = '\0';
  memset(nickname, 'N', max_text ? 128u : 8u);
  nickname[max_text ? 128u : 8u] = '\0';
  memset(login, 'L', max_text ? 128u : 8u);
  login[max_text ? 128u : 8u] = '\0';
  int size = snprintf(
      f->completed, sizeof(f->completed),
      "{\"needQuery\":false,\"success\":true,\"tid\":\"%s\","
      "\"userId\":\"%s\",\"nickName\":\"%s\",\"loginId\":\"%s\","
      "\"serverTime\":\"1794449000000\",\"encryptedFactors\":\"%s&%s\"}",
      tid, uid, nickname, login, point_hex, seed);
  assert(size > 0 && (size_t)size < sizeof(f->completed));
}

int main(void) {
  fixture_t f;
  h2_alipay_client_binding_t binding;
  initialize(&f);
  assert(h2_alipay_client_binding_snapshot(&f.config, &binding) == 0 &&
         binding.state == H2_ALIPAY_CLIENT_UNBOUND);
  f.pending = true;
  assert(h2_alipay_client_query_binding_result(&f.config, &f.session,
                                               &binding) == 0);
  assert(binding.state == H2_ALIPAY_CLIENT_PENDING && f.finishes == 0u);
  f.query_rc = H2_PAL_ERR_TIMEOUT;
  assert(h2_alipay_client_query_binding_result(&f.config, &f.session,
                                               &binding) == H2_PAL_ERR_TIMEOUT);
  assert(binding.state == H2_ALIPAY_CLIENT_PENDING &&
         f.session.token[0] != '\0');
  f.query_rc = 0;
  f.pending = false;
  uint8_t expected[65];
  h2_pal_p256_keypair_t local;
  assert(h2_pal_crypto_p256_keypair_from_private(
             f.config.crypto, &f.session.private_key, &local) == 0);
  assert(h2_pal_crypto_p256_public_key_uncompress(
             f.config.crypto, &local.public_key, expected) == 0);
  complete(&f);
  h2_pal_pref_namespace_t* ns;
  assert(h2_pal_pref_open(f.config.pref, f.config.pref_namespace,
                          H2_PAL_PREF_OPEN_READ_ONLY, &ns) == 0);
  void* blob = NULL;
  size_t size = 0u;
  assert(ns->get_blob(ns, f.config.mem, "binding", &blob, &size) == 0);
  assert(memcmp((uint8_t*)blob + size - 64u, expected + 1u, 32u) == 0);
  memset(blob, 0, size);
  h2_pal_mem_free(f.config.mem, blob);
  assert(ns->close(ns) == 0);
  unsigned requests = f.queries + f.finishes;
  h2_alipay_client_result_clear(&f.session); /* restart */
  complete(&f);
  assert(requests == f.queries + f.finishes);
  f.config.identity.sn = "OTHER_SN";
  assert(h2_alipay_client_binding_snapshot(&f.config, &binding) ==
         H2_PAL_ERR_INVALID_STATE);
  assert(binding.state == H2_ALIPAY_CLIENT_NEEDS_VERIFICATION);
  destroy(&f);

  /* Real SDK storage observed an 80-byte decoded seed. Exercise that size,
   * the original 128-byte buffer limit, and the full staged-record boundary. */
  const size_t accepted_seeds[] = {16u, 64u, 80u, 128u};
  for (size_t i = 0u; i < sizeof(accepted_seeds) / sizeof(*accepted_seeds);
       ++i) {
    initialize(&f);
    sized_response(&f, accepted_seeds[i], true);
    memset(f.expected_token, 'S', 128u);
    f.expected_token[128] = '\0';
    memcpy(f.session.token, f.expected_token, sizeof(f.expected_token));
    f.finish_rc = H2_PAL_ERR_TIMEOUT;
    assert(h2_alipay_client_query_binding_result(
               &f.config, &f.session, &binding) == H2_PAL_ERR_TIMEOUT);
    assert(f.finishes == 1u && binding.state == H2_ALIPAY_CLIENT_PENDING);
    assert(h2_alipay_client_binding_snapshot(&f.config, &binding) == 0);
    assert(binding.state == H2_ALIPAY_CLIENT_NEEDS_VERIFICATION &&
           binding.seed_length == accepted_seeds[i]);
    assert(h2_pal_pref_open(f.config.pref, f.config.pref_namespace,
                            H2_PAL_PREF_OPEN_READ_ONLY, &ns) == 0);
    blob = NULL;
    size = 0u;
    assert(ns->get_blob(ns, f.config.mem, "binding", &blob, &size) == 0);
    assert(size == 625u + accepted_seeds[i]);
    memset(blob, 0, size);
    h2_pal_mem_free(f.config.mem, blob);
    assert(ns->close(ns) == 0);
    h2_alipay_client_result_clear(&f.session); /* resume staged finish */
    f.finish_rc = 0;
    assert(h2_alipay_client_query_binding_result(&f.config, &f.session,
                                                 &binding) == 0);
    assert(binding.state == H2_ALIPAY_CLIENT_COMPLETE &&
           binding.seed_length == accepted_seeds[i] &&
           binding.tid_length == 64u && binding.uid_length == 64u &&
           binding.shared_key_length == 32u);
    assert(h2_alipay_client_binding_snapshot(&f.config, &binding) == 0 &&
           binding.state == H2_ALIPAY_CLIENT_COMPLETE &&
           binding.seed_length == accepted_seeds[i]);
    assert(f.queries == 1u && f.finishes == 2u);
    destroy(&f);
  }

  const size_t rejected_seeds[] = {15u, 129u};
  for (size_t i = 0u; i < sizeof(rejected_seeds) / sizeof(*rejected_seeds);
       ++i) {
    initialize(&f);
    sized_response(&f, rejected_seeds[i], false);
    assert(h2_alipay_client_query_binding_result(
               &f.config, &f.session, &binding) == H2_PAL_ERR_FORMAT);
    assert(binding.state == H2_ALIPAY_CLIENT_FAILED && f.finishes == 0u);
    assert(h2_alipay_client_binding_snapshot(&f.config, &binding) == 0 &&
           binding.state == H2_ALIPAY_CLIENT_UNBOUND);
    destroy(&f);
  }

  initialize(&f);
  sized_response(&f, 80u, false);
  char* seed_end = strchr(strstr(f.completed, point_hex) + 129u, '"');
  assert(seed_end != NULL);
  memmove(seed_end - 1u, seed_end, strlen(seed_end) + 1u);
  assert(h2_alipay_client_query_binding_result(&f.config, &f.session,
                                               &binding) == H2_PAL_ERR_FORMAT);
  assert(f.finishes == 0u);
  destroy(&f);

  initialize(&f);
  f.finish_rc = H2_PAL_ERR_TIMEOUT;
  assert(h2_alipay_client_query_binding_result(&f.config, &f.session,
                                               &binding) == H2_PAL_ERR_TIMEOUT);
  assert(binding.state == H2_ALIPAY_CLIENT_PENDING && f.queries == 1u &&
         f.finishes == 1u);
  assert(h2_alipay_client_binding_snapshot(&f.config, &binding) == 0 &&
         binding.state == H2_ALIPAY_CLIENT_NEEDS_VERIFICATION);
  assert(memcmp(f.session.private_key.bytes, (uint8_t[32]){0}, 32u) == 0);
  h2_alipay_client_result_clear(
      &f.session); /* staged finish survives restart */
  f.finish_rc = 0;
  complete(&f);
  assert(f.queries == 1u && f.finishes == 2u);
  destroy(&f);

  initialize(&f);
  f.pref.commit = (h2_app_test_fault_t){
      .result = H2_PAL_ERR_WRITE, .remaining = 1u, .skip = 1u};
  assert(h2_alipay_client_query_binding_result(&f.config, &f.session,
                                               &binding) == H2_PAL_ERR_WRITE);
  assert(binding.state == H2_ALIPAY_CLIENT_NEEDS_VERIFICATION &&
         binding.storage_rc == H2_PAL_ERR_WRITE &&
         f.session.finish_acknowledged);
  complete(&f);
  assert(f.queries == 1u && f.finishes == 1u); /* local commit retry only */
  destroy(&f);

  initialize(&f);
  f.pref.commit =
      (h2_app_test_fault_t){.result = H2_PAL_ERR_WRITE, .remaining = 1u};
  assert(h2_alipay_client_query_binding_result(&f.config, &f.session,
                                               &binding) == H2_PAL_ERR_WRITE);
  assert(f.finishes == 0u);
  assert(h2_alipay_client_binding_snapshot(&f.config, &binding) == 0 &&
         binding.state == H2_ALIPAY_CLIENT_UNBOUND);
  complete(&f);
  assert(f.queries == 2u && f.finishes == 1u);
  destroy(&f);

  initialize(&f);
  f.pref.read = (h2_app_test_fault_t){
      .result = H2_PAL_ERR_IO, .remaining = 1u, .skip = 2u};
  assert(h2_alipay_client_query_binding_result(&f.config, &f.session,
                                               &binding) == H2_PAL_ERR_IO);
  assert(binding.storage_rc == H2_PAL_ERR_IO && f.finishes == 0u);
  complete(&f);
  assert(f.queries == 1u && f.finishes == 1u);
  destroy(&f);

  initialize(&f);
  f.pref.read = (h2_app_test_fault_t){
      .result = H2_PAL_ERR_IO, .remaining = 1u, .skip = 3u};
  assert(h2_alipay_client_query_binding_result(&f.config, &f.session,
                                               &binding) == H2_PAL_ERR_IO);
  assert(binding.state == H2_ALIPAY_CLIENT_NEEDS_VERIFICATION &&
         f.finishes == 1u);
  complete(&f); /* completed record readback, no repeat RPC */
  assert(f.queries == 1u && f.finishes == 1u);
  destroy(&f);

  initialize(&f);
  complete(&f);
  assert(h2_pal_pref_open(f.config.pref, f.config.pref_namespace,
                          H2_PAL_PREF_OPEN_READ_WRITE, &ns) == 0);
  blob = NULL;
  size = 0u;
  assert(ns->get_blob(ns, f.config.mem, "binding", &blob, &size) == 0);
  ((uint8_t*)blob)[size - 1u] ^= 1u;
  assert(ns->set_blob(ns, "binding", blob, size) == 0 && ns->commit(ns) == 0);
  memset(blob, 0, size);
  h2_pal_mem_free(f.config.mem, blob);
  assert(ns->close(ns) == 0);
  assert(h2_alipay_client_binding_snapshot(&f.config, &binding) ==
         H2_PAL_ERR_FORMAT);
  assert(binding.state == H2_ALIPAY_CLIENT_NEEDS_VERIFICATION);
  destroy(&f);

  static const char* malformed[] = {
      "{}",
      "{\"needQuery\":false,\"success\":false}",
      "{\"needQuery\":true,\"needQuery\":false}",
      "{\"needQuery\":1}",
      "{\"needQuery\":false,\"success\":true}",
      "{\"needQuery\":true}garbage",
  };
  for (size_t i = 0u; i < sizeof(malformed) / sizeof(*malformed); ++i) {
    initialize(&f);
    f.query_body = malformed[i];
    assert(h2_alipay_client_query_binding_result(
               &f.config, &f.session, &binding) == H2_PAL_ERR_FORMAT);
    assert(binding.state == H2_ALIPAY_CLIENT_FAILED && f.finishes == 0u);
    destroy(&f);
  }
  for (size_t i = 0u; i < 3u; ++i) {
    initialize(&f);
    char* encoded = strstr(f.completed, point_hex);
    assert(encoded != NULL);
    if (i == 0u) encoded[0] = 'g';
    if (i == 1u) encoded[128] = '#';
    if (i == 2u) encoded[129] = 'g';
    assert(h2_alipay_client_query_binding_result(
               &f.config, &f.session, &binding) == H2_PAL_ERR_FORMAT);
    assert(f.finishes == 0u);
    destroy(&f);
  }
  /* Wrong Y with the same parity must not be silently accepted. */
  initialize(&f);
  char* factors = strstr(f.completed, point_hex);
  assert(factors != NULL);
  factors[64] = factors[64] == '4' ? '5' : '4';
  assert(h2_alipay_client_query_binding_result(&f.config, &f.session,
                                               &binding) == H2_PAL_ERR_FORMAT);
  assert(f.finishes == 0u);
  destroy(&f);

  initialize(&f);
  f.finish_body = "{\"success\":false}";
  assert(h2_alipay_client_query_binding_result(&f.config, &f.session,
                                               &binding) == H2_PAL_ERR_FORMAT);
  assert(binding.state == H2_ALIPAY_CLIENT_FAILED);
  assert(h2_alipay_client_binding_snapshot(&f.config, &binding) == 0 &&
         binding.state == H2_ALIPAY_CLIENT_NEEDS_VERIFICATION);
  destroy(&f);

  /* Every allocation failure must preserve ownership and avoid false success.
   */
  initialize(&f);
  size_t before = f.mem.calls;
  complete(&f);
  size_t allocations = f.mem.calls - before;
  destroy(&f);
  for (size_t i = 1u; i <= allocations; ++i) {
    initialize(&f);
    f.mem.fail_at = f.mem.calls + i;
    int rc =
        h2_alipay_client_query_binding_result(&f.config, &f.session, &binding);
    if (rc != 0) assert(binding.state != H2_ALIPAY_CLIENT_COMPLETE);
    destroy(&f);
  }
  return 0;
}
