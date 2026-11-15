/* Network-disabled original-machine-code oracle with synthetic materials. */
#include "h2_alipay_client_internal.h"
#include "alipay_pay.h"
#include "alipay_pay_result.h"
#include "vendor_device_info.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern int alipay_pay_AES_cbc_encrypt(const uint8_t*, uint32_t, uint8_t*,
                                      uint32_t*, const uint8_t*, uint32_t);
extern int IFAA_AES_set_decrypt_key(const uint8_t*, int, void*);
extern void IFAA_cbc_decrypt(const uint8_t*, uint8_t*, uint8_t*, uint32_t,
                             uint64_t, const void*);
extern const unsigned char ALIPAY_mbedtls_sha1_info[];
extern int ALIPAY_mbedtls_md_hmac(const void*, const uint8_t*, size_t,
                                  const uint8_t*, size_t, uint8_t*);

static binding_record_t binding;
static uint32_t epoch;
static unsigned comparisons, query_comparisons, allocations;
static bool bound = true, unbound;
static const char* mac = "12:34:56:78:9A:BC";
static const char* query_response;
static char query_body[512], ack_body[512];
static unsigned acknowledgements;
static const char code[] = "284960766182755224";
static int semihost(int op, int a, int b, int c) {
  register int r2 __asm__("a2") = op;
  register int r3 __asm__("a3") = a;
  register int r4 __asm__("a4") = b;
  register int r5 __asm__("a5") = c;
  __asm__ volatile("simcall"
                   : "+r"(r2), "+r"(r3), "+r"(r4), "+r"(r5)::"memory");
  return r2;
}
static void emit(const char* text) {
  (void)semihost(4, 1, (int)(uintptr_t)text, (int)strlen(text));
}
void __assert_func(const char* file, int line, const char* fn,
                   const char* expression) __attribute__((noreturn));
void __assert_func(const char* file, int line, const char* fn,
                   const char* expression) {
  (void)file;
  (void)fn;
  char text[320];
  snprintf(text, sizeof(text), "ASSERT line=%d expression=%s\n", line,
           expression);
  emit(text);
  (void)semihost(1, 1, 0, 0);
  for (;;) {
  }
}
void alipay_inner_log_ext(const char* pattern, ...) { (void)pattern; }
int32_t alipay_inner_get_binding_status(void) { return bound ? 165 : 1; }
extern int get_paycode(uint8_t*, uint32_t*);
int alipay_inner_get_paycode(uint8_t* out, uint32_t* length) {
  return get_paycode(out, length);
}
void save_last_show_pay_code(const char* value) { (void)value; }
uint32_t alipay_get_timestamp(void) { return epoch; }
retval_e alipay_get_mac(uint8_t* out, uint32_t* length) {
  assert(*length >= 18u);
  memcpy(out, mac, 18u);
  *length = 17u;
  return 0;
}
int ALIPAY_storage_read(uint32_t index, uint8_t* out, uint32_t* length) {
  const void* data;
  size_t size;
  if (index == 1u) {
    data = binding.secret;
    size = 32u;
  } else if (index == 2u) {
    data = binding.seed;
    size = binding.seed_length;
  } else if (index == 6u) {
    data = "TEST_TID";
    size = 8u;
  } else if (index == 7u) {
    data = "TEST_UID";
    size = 8u;
  } else
    return 2;
  assert(*length >= size);
  memcpy(out, data, size);
  *length = (uint32_t)size;
  return 0;
}
void* alipay_malloc(uint32_t size) { return malloc(size); }
void* alipay_calloc(uint32_t count, uint32_t size) {
  return calloc(count, size);
}
void alipay_free(void* p) { free(p); }
int alipay_get_dev_productModel(uint8_t* out, uint32_t* length) {
  assert(*length > 4u);
  memcpy(out, "H106", 5u);
  *length = 4u;
  return 0;
}
void reset_rpc_status(const char* operation) { (void)operation; }
int alipay_inner_unbinding(void) {
  unbound = true;
  return 0;
}
typedef struct rpc_request {
  const char* operation;
  int version;
  const char* body;
  uint32_t length, type;
} rpc_request_t;
int alipay_send_rpc_simple(const rpc_request_t* request, char* out,
                           uint32_t* size) {
  bool ack = strstr(request->operation, "payResultAck") != NULL;
  assert(request->length == strlen(request->body) && request->length < 512u);
  if (ack) {
    ++acknowledgements;
    memcpy(ack_body, request->body, request->length + 1u);
  } else {
    assert(strstr(request->operation, "queryOrderStatus") != NULL);
    memcpy(query_body, request->body, request->length + 1u);
  }
  const char* response = ack ? "{}" : query_response;
  assert(*size > strlen(response));
  strcpy(out, response);
  *size = (uint32_t)strlen(response);
  return 0;
}
void h2_alipay_client_wipe(void* data, size_t size) {
  volatile uint8_t* p = data;
  while (size-- != 0u) *p++ = 0u;
}
static void* allocate(void* user, size_t size) {
  (void)user;
  size_t* p = calloc(1u, sizeof(size_t) + size);
  assert(p != NULL);
  *p = size;
  ++allocations;
  return p + 1u;
}
static void deallocate(void* user, void* data) {
  (void)user;
  if (data == NULL) return;
  size_t* p = (size_t*)data - 1u;
  for (size_t i = 0u; i < *p; ++i) assert(((const uint8_t*)data)[i] == 0u);
  --allocations;
  free(p);
}
static int wall(void* user, uint64_t* out) {
  (void)user;
  *out = (uint64_t)epoch * 1000u;
  return 0;
}
static int wall_status(void* user, h2_pal_time_wall_status_t* out) {
  (void)user;
  out->valid = true;
  out->source = H2_PAL_TIME_WALL_SOURCE_UNKNOWN;
  return 0;
}
static int cbc(void* user, h2_pal_crypto_cipher_direction_t direction,
               const uint8_t* key, size_t key_size, const uint8_t iv[16],
               const uint8_t* input, size_t size, h2_pal_crypto_buf_t* out) {
  (void)user;
  assert(direction == H2_PAL_CRYPTO_CIPHER_DECRYPT && key_size == 16u);
  assert(out->cap >= size && size % 16u == 0u);
  struct work {
    uint32_t schedule[61];
    uint8_t iv[16];
  };
  struct work* w = allocate(NULL, sizeof(*w));
  memcpy(w->iv, iv, 16u);
  assert(IFAA_AES_set_decrypt_key(key, 128, w->schedule) == 0);
  IFAA_cbc_decrypt(input, out->data, w->iv, 16u, (uint64_t)(size / 16u),
                   w->schedule);
  out->len = size;
  h2_alipay_client_wipe(w, sizeof(*w));
  deallocate(NULL, w);
  return 0;
}
static int hmac(void* user, const uint8_t* key, size_t key_size,
                const uint8_t* input, size_t size, uint8_t out[20]) {
  (void)user;
  return ALIPAY_mbedtls_md_hmac(ALIPAY_mbedtls_sha1_info, key, key_size, input,
                                size, out) == 0
             ? 0
             : H2_PAL_ERR_IO;
}
static void paycodes(void) {
  static const h2_pal_mem_vtable_t mv = {.alloc = allocate, .free = deallocate};
  static const h2_pal_mem_api_t mem = {.vtable = &mv};
  static const h2_pal_time_vtable_t tv = {.get_wall_ms = wall,
                                          .get_wall_status = wall_status};
  static const h2_pal_time_api_t time = {.vtable = &tv};
  static const h2_pal_crypto_vtable_t cv = {.aes_cbc = cbc, .hmac_sha1 = hmac};
  static const h2_pal_crypto_api_t crypto = {.vtable = &cv};
  const h2_alipay_client_config_t config = {
      .mem = &mem, .time = &time, .crypto = &crypto, .identity = {.mac = mac}};
  const char* indexes[] = {"1", "1234567890", "12345678901"};
  const unsigned intervals[] = {1u, 30u, 60u, 300u, 3600u};
  const uint32_t times[] = {0u, 1u, 59u, 60u, 61u, 1794449000u, UINT32_MAX};
  for (unsigned pattern = 0u; pattern < 8u; ++pattern)
    for (unsigned secret_size = 10u; secret_size <= 20u; secret_size += 10u)
      for (unsigned id = 0u; id < 3u; ++id)
        for (unsigned interval = 0u; interval < 5u; ++interval) {
          char secret[41], plain[128];
          uint8_t middle[144];
          for (unsigned i = 0u; i < 32u; ++i)
            binding.secret[i] = (uint8_t)(pattern * 17u + i);
          for (unsigned i = 0u; i < secret_size; ++i)
            snprintf(secret + i * 2u, 3u, "%02x", (pattern * 13u + i) % 256u);
          int size = snprintf(plain, sizeof(plain), "%s###%s###%u###",
                              indexes[id], secret, intervals[interval]);
          assert(size > 0 && (size_t)size < sizeof(plain));
          uint32_t length = sizeof(middle), seed_length = sizeof(binding.seed);
          assert(alipay_pay_AES_cbc_encrypt((uint8_t*)plain, (uint32_t)size,
                                            middle, &length, binding.secret,
                                            16u) == 0);
          assert(alipay_pay_AES_cbc_encrypt(middle, length, binding.seed,
                                            &seed_length, binding.secret + 16u,
                                            16u) == 0);
          binding.seed_length = seed_length;
          for (unsigned ts = 0u; ts < 7u; ++ts) {
            epoch = times[ts];
            uint8_t original[32], independent[32];
            memset(original, 0xa5, sizeof(original));
            memset(independent, 0xa5, sizeof(independent));
            uint32_t n = sizeof(original);
            size_t own = sizeof(independent);
            assert(alipay_get_paycode(original, &n) == 0);
            h2_alipay_client_stage_t stage;
            int own_rc =
                h2_alipay_paycode_generate(&config, &binding, independent,
                                           sizeof(independent), &own, &stage);
            if (own_rc != 0) {
              char diagnostic[160];
              snprintf(
                  diagnostic, sizeof(diagnostic),
                  "candidate rc=%d stage=%d pattern=%u secret_size=%u index_case=%u interval=%u time_case=%u\n",
                  own_rc, (int)stage, pattern, secret_size, id, interval, ts);
              emit(diagnostic);
            }
            assert(own_rc == 0);
            assert(own == n && (n == 18u || n == 19u));
            assert(memcmp(original, independent, n + 1u) == 0);
            assert(original[31] == 0xa5 && independent[31] == 0xa5 &&
                   allocations == 0u);
            ++comparisons;
          }
        }
}
static void paycode_edges(void) {
  static const h2_pal_mem_vtable_t mv = {.alloc = allocate, .free = deallocate};
  static const h2_pal_mem_api_t mem = {.vtable = &mv};
  static const h2_pal_time_vtable_t tv = {.get_wall_ms = wall,
                                          .get_wall_status = wall_status};
  static const h2_pal_time_api_t time = {.vtable = &tv};
  static const h2_pal_crypto_vtable_t cv = {.aes_cbc = cbc, .hmac_sha1 = hmac};
  static const h2_pal_crypto_api_t crypto = {.vtable = &cv};
  const h2_alipay_client_config_t config = {
      .mem = &mem, .time = &time, .crypto = &crypto, .identity = {.mac = mac}};
  const char* plains[] = {
      "1234567890###3132333435363738393031323334353637383930###60###",
      "1234567890###3132333435363738393031323334353637383930###60tail"};
  for (unsigned i = 0u; i < 2u; ++i) {
    uint8_t middle[144], original[32], independent[32];
    uint32_t n = sizeof(middle), seed_length = sizeof(binding.seed);
    size_t size = strlen(plains[i]) + (i == 0u ? 1u : 0u);
    assert(alipay_pay_AES_cbc_encrypt((const uint8_t*)plains[i], (uint32_t)size,
                                      middle, &n, binding.secret, 16u) == 0);
    assert(alipay_pay_AES_cbc_encrypt(middle, n, binding.seed, &seed_length,
                                      binding.secret + 16u, 16u) == 0);
    binding.seed_length = seed_length;
    n = sizeof(original);
    size_t own = sizeof(independent);
    assert(alipay_get_paycode(original, &n) == 0);
    h2_alipay_client_stage_t stage;
    assert(h2_alipay_paycode_generate(&config, &binding, independent,
                                      sizeof(independent), &own, &stage) == 0);
    assert(own == n && memcmp(original, independent, n + 1u) == 0);
    ++comparisons;
  }
  binding.seed[binding.seed_length - 1u] ^= 1u;
  uint8_t original[32];
  uint32_t n = sizeof(original);
  assert(alipay_get_paycode(original, &n) == 99);
  ++comparisons;
}
static void queries(void) {
  const char* bodies[] = {
      "{}",
      "{\"action\":\"pay_success\",\"sdkBizData\":{\"realAmount\":\"0.00\",\"originAmount\":\"1.00\"}}",
      "{\"entireData\":\"{\\\"action\\\":\\\"pay_success\\\",\\\"sdkBizData\\\":\\\"{\\\\\\\"originAmount\\\\\\\":\\\\\\\"2.34\\\\\\\"}\\\"}\"}",
      "{\"action\":\"CLOSECODE\"}",
      "{\"action\":\"failure_info\",\"sdkBizData\":\"limited\"}",
      "{\"action\":\"soundWavePaySuccessPush\",\"realAmount\":\"0\"}",
      "{\"action\":\"unsupported\"}",
      "{\"action\":\"DELSEED\"}",
      "{\"action\":\"DELSEEDANDINDEX\"}"};
  const alipay_payment_status_e expected[] = {2, 0, 0, 4, 1, 0, 2, 3, 3};
  const char expected_query[] =
      "[{\"dynamicId\":\"284960766182755224\",\"userId\":\"TEST_UID\",\"tid\":\"TEST_TID\","
      "\"extInfo\":{\"sdkType\":\"INSIDE_WATCH\",\"clientVersion\":\"4.61\",\"sdkHost\":\"H106\"}}]";
  const char expected_ack[] =
      "[{\"dynamicId\":\"284960766182755224\",\"userId\":\"TEST_UID\",\"tid\":\"TEST_TID\","
      "\"extInfo\":{\"sdkType\":\"INSIDE_WATCH\",\"ackType\":\"RPC\",\"clientVersion\":\"4.61\",\"sdkHost\":\"H106\"}}]";
  for (unsigned i = 0u; i < 9u; ++i) {
    query_response = bodies[i];
    acknowledgements = 0u;
    unbound = false;
    char amount[32] = {0}, ext[128] = {0};
    uint32_t length = sizeof(ext);
    alipay_payment_status_e status = ALIPAY_PAYMENT_STATUS_UNKNOWN;
    assert(alipay_query_order_result_string((char*)code, &status, amount, ext,
                                            &length) == 0);
    assert(status == expected[i] && strcmp(query_body, expected_query) == 0);
    if (status == 0)
      assert(acknowledgements == 1u && strcmp(ack_body, expected_ack) == 0);
    else
      assert(acknowledgements == 0u);
    if (i == 1u) assert(strcmp(amount, "0.00") == 0);
    if (i == 2u) assert(strcmp(amount, "2.34") == 0);
    if (i == 4u) assert(length == 7u && memcmp(ext, "limited", 7u) == 0);
    if (i >= 7u) assert(unbound);
    ++query_comparisons;
  }
}
int main(void) {
  paycodes();
  paycode_edges();
  queries();
  bound = false;
  uint8_t out[32];
  uint32_t n = sizeof(out);
  assert(alipay_get_paycode(out, &n) == 4);
  ++comparisons;
  assert(alipay_get_paycode(NULL, &n) == 19);
  ++comparisons;
  char summary[160];
  snprintf(
      summary, sizeof(summary),
      "{\"original_binary_comparisons\":%u,\"query_comparisons\":%u,\"failed\":0}\n",
      comparisons, query_comparisons);
  h2_alipay_client_wipe(&binding, sizeof(binding));
  emit(summary);
  (void)semihost(1, 0, 0, 0);
  for (;;) {
  }
}
