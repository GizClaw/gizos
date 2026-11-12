#include "h2_alipay_protocol.h"
#include "h2_sm3.h"
#include <assert.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern unsigned char rpc_init_cfg[232];
extern int alipay_iothub_sign(const uint8_t*, uint32_t, uint8_t*, uint32_t*);
extern void ALIPAY_sm3_hmac(const uint8_t*, int, const uint8_t*, int, uint8_t*);
extern void register_signature_func(int (*)(const uint8_t*, uint32_t, char**,
                                            uint32_t*),
                                    void (*)(void*));
typedef struct oracle_bytes {
  const uint8_t* data;
  uint32_t size;
} oracle_bytes_t;
extern int iot_get_sign_data(void*, const char*, const oracle_bytes_t*,
                             const char*, char**, int, int*);
extern int alipay_inner_get_binding_code(uint32_t, char*, int*);
extern int profile_get_factor(void*);
extern const char* BOX_device_id(void*);
extern char tmp_device_name_buf[256];
extern void rpc_time_to_string(int32_t, char*);
extern void alipay_ByteToHexStr(const uint8_t*, char*, uint32_t);
typedef struct oracle_registration {
  const char* product;
  const char* sn;
  const char* vendor;
  uint32_t count;
  const char** info;
  int sign_algorithm;
  int negotiate_algorithm;
  int timeout;
} oracle_registration_t;
extern int ant_iot_device_name_req(const oracle_registration_t*, void*);

static h2_alipay_protocol_identity_t identity;
static const char* device_name;
static char captured[2048];
static unsigned comparisons;
static char request_body[4096];
static uint8_t packed_registration[512];
static size_t packed_length;
static bool storage_present = true;

static void* allocate(void* user, size_t size) {
  (void)user;
  return malloc(size);
}
static void* reallocate(void* user, void* data, size_t size) {
  (void)user;
  return realloc(data, size);
}
static void deallocate(void* user, void* data) {
  (void)user;
  free(data);
}
static const h2_pal_mem_vtable_t mem_vtable = {
    .alloc = allocate, .realloc = reallocate, .free = deallocate};
static const h2_pal_mem_api_t mem = {.vtable = &mem_vtable};

static int semihost(int operation, int first, int second, int third) {
  register int result_register __asm__("a2") = operation;
  register int first_register __asm__("a3") = first;
  register int second_register __asm__("a4") = second;
  register int third_register __asm__("a5") = third;
  __asm__ volatile("simcall"
                   : "+r"(result_register), "+r"(first_register),
                     "+r"(second_register), "+r"(third_register)
                   :
                   : "memory");
  return result_register;
}
static void emit(const char* text) {
  (void)semihost(4, 1, (int)(uintptr_t)text, (int)strlen(text));
}
void __assert_func(const char* file, int line, const char* function,
                   const char* expression) __attribute__((noreturn));
void __assert_func(const char* file, int line, const char* function,
                   const char* expression) {
  (void)file;
  (void)function;
  char text[320];
  snprintf(text, sizeof(text), "ASSERT line=%d expression=%s\n", line,
           expression);
  emit(text);
  (void)semihost(1, 1, 0, 0);
  for (;;) {
  }
}

void* alipay_calloc(uint32_t count, uint32_t size) {
  return calloc(count, size);
}
void* alipay_malloc(uint32_t size) { return malloc(size); }
void alipay_free(void* data) { free(data); }

static void* memory(void* user, void* previous, size_t old_size, size_t size) {
  (void)user;
  (void)old_size;
  if (size == 0u) {
    free(previous);
    return NULL;
  }
  return realloc(previous, size);
}
static void* memory_set(void* user, void* out, int value, size_t length) {
  (void)user;
  return memset(out, value, length);
}
static int format(char* out, size_t capacity, const char* pattern, void* stack,
                  void* registers, unsigned index) {
  uintptr_t values[3] = {(uintptr_t)stack, (uintptr_t)registers, index};
  va_list args;
  _Static_assert(sizeof(args) == sizeof(values), "Xtensa va_list ABI");
  memcpy(&args, values, sizeof(args));
  return vsnprintf(out, capacity, pattern, args);
}
void alipay_inner_log_ext(const char* pattern, ...) { (void)pattern; }
static int text(const char* value, uint8_t* out, uint32_t* size) {
  size_t length = strlen(value);
  assert(length < *size);
  memcpy(out, value, length + 1u);
  *size = length;
  return 0;
}
int alipay_get_imei(uint8_t* out, uint32_t* size) {
  return text(identity.imei, out, size);
}
int alipay_get_sn(uint8_t* out, uint32_t* size) {
  return text(identity.sn, out, size);
}
int alipay_get_mac(uint8_t* out, uint32_t* size) {
  return text(identity.mac, out, size);
}
int alipay_get_productModel(uint8_t* out, uint32_t* size) {
  return text(identity.model, out, size);
}
int alipay_get_companyName(uint8_t* out, uint32_t* size) {
  return text("HAIVIVI", out, size);
}
uint32_t alipay_get_productType(void) { return 3u; }
uint32_t alipay_get_timestamp(void) { return 1794449000u; }
uint32_t alipay_get_compile_timestamp(void) { return 1794449000u; }
void alipay_binding_process_notify(int status) { (void)status; }
int ALIPAY_storage_read(uint32_t item, uint8_t* out, uint32_t* size) {
  assert(item == 8u || item == 9u);
  return text(item == 8u ? device_name : "SYNTHETIC_BIZ_001", out, size);
}
int ALIPAY_storage_exists(uint32_t item) {
  return item == 8u && storage_present ? 0 : -8;
}
int ALIPAY_storage_write(uint32_t item, const uint8_t* data, uint32_t size) {
  (void)item;
  (void)data;
  (void)size;
  return 0;
}
int get_Rng_number(uint8_t* out, unsigned size) {
  memset(out, 0x42, size);
  return 1;
}
void oracle_set_rng(void* function) { (void)function; }
int oracle_generate(uint8_t* public_key, uint8_t* private_key) {
  for (size_t index = 0u; index < 64u; ++index)
    public_key[index] = (uint8_t)index;
  memset(private_key, 0x42, 32u);
  return 0;
}
int alipay_inner_get_secure_bind_data(void* input, void* device, int type) {
  (void)input;
  (void)type;
  return profile_get_factor(device);
}
typedef struct oracle_request {
  const char* operation;
  int version;
  const char* body;
  uint32_t length;
  uint32_t timeout;
} oracle_request_t;
int alipay_send_rpc_simple(const oracle_request_t* request, char* out,
                           uint32_t* size) {
  assert(strcmp(request->operation,
                "alipay.mobile.alideviceinfo.changecode.prepareBind") == 0);
  assert(request->length == strlen(request->body));
  assert(request->length < sizeof(request_body));
  memcpy(request_body, request->body, request->length + 1u);
  static const char response[] =
      "{\"success\":true,\"token\":\"SYNTHETIC_BINDING_TOKEN\"}";
  assert(*size >= sizeof(response));
  memcpy(out, response, sizeof(response));
  *size = sizeof(response) - 1u;
  return 0;
}
int oracle_send_registration(void* context, const void* descriptor,
                             void* ticket, const oracle_bytes_t* body,
                             void* callback, void* pending) {
  (void)context;
  (void)descriptor;
  (void)ticket;
  (void)callback;
  (void)pending;
  assert(body->size <= sizeof(packed_registration));
  packed_length = body->size;
  memcpy(packed_registration, body->data, packed_length);
  return 1;
}
static int capture(const uint8_t* data, uint32_t size, char** out,
                   uint32_t* length) {
  assert(size < sizeof(captured));
  memcpy(captured, data, size);
  captured[size] = '\0';
  *out = malloc(3u);
  assert(*out != NULL);
  memcpy(*out, "00", 3u);
  *length = 2u;
  return 0;
}
int main(void) {
  void* callbacks[58] = {0};
  callbacks[2] = memory;
  callbacks[3] = memory_set;
  callbacks[55] = format;
  memcpy(rpc_init_cfg, callbacks, sizeof(callbacks));
  const uint32_t seconds[] = {1u,   9u,    10u,         99u,
                              100u, 1000u, 1794449000u, INT32_MAX};
  const unsigned fractions[] = {0u, 1u, 999u};
  for (size_t index = 0u; index < sizeof(seconds) / sizeof(*seconds); ++index) {
    for (size_t fraction = 0u;
         fraction < sizeof(fractions) / sizeof(*fractions); ++fraction) {
      char original_time[24], independent_time[24];
      rpc_time_to_string((int32_t)seconds[index], original_time);
      assert(h2_alipay_protocol_timestamp(
                 (uint64_t)seconds[index] * 1000u + fractions[fraction],
                 independent_time) == 0);
      assert(strcmp(original_time, independent_time) == 0);
      ++comparisons;
    }
  }
  uint8_t input[257], key[129], expected[32], actual[32];
  for (size_t index = 0u; index < sizeof(input); ++index)
    input[index] = (uint8_t)(index * 29u + 7u);
  for (size_t index = 0u; index < sizeof(key); ++index)
    key[index] = (uint8_t)(index * 11u);
  for (size_t key_size = 0u; key_size <= sizeof(key); key_size += 3u) {
    for (size_t size = 0u; size <= sizeof(input); size += 7u) {
      ALIPAY_sm3_hmac(input, size, key, key_size, expected);
      h2_sm3_hmac(key, key_size, input, size, actual);
      assert(memcmp(actual, expected, sizeof(actual)) == 0);
      ++comparisons;
    }
  }
  register_signature_func(capture, free);
  for (size_t size = 0u; size <= sizeof(input); ++size) {
    for (unsigned binary = 0u; binary < 2u; ++binary) {
      oracle_bytes_t bytes = {.data = input, .size = size};
      char* signature = NULL;
      int sign_type = -1;
      assert(iot_get_sign_data(NULL, "test.alipay.synthetic", &bytes,
                               "1794449000000", &signature, !binary,
                               &sign_type) == 0);
      assert(signature != NULL);
      free(signature);
      char canonical[2048];
      size_t length = 0u;
      assert(h2_alipay_protocol_canonical(
                 "test.alipay.synthetic", "1794449000000", binary, input, size,
                 canonical, sizeof(canonical), &length) == 0);
      assert(length == strlen(captured) && strcmp(canonical, captured) == 0);
      ++comparisons;
    }
  }
  const h2_alipay_protocol_identity_t identities[] = {
      {"FACTORY_SN_001", "FACTORY_SN_001", "12:34:56:78:9A:BC", "H-106TG"},
      {"SN_DISTINCT", "IMEI_DISTINCT", "AB:CD:EF:01:23:45", "TEST_MODEL"},
      {"123456789012347", "123456789012347", "00:00:00:00:00:01", "X"},
  };
  for (size_t index = 0u; index < sizeof(identities) / sizeof(*identities);
       ++index) {
    identity = identities[index];
    device_name = "SYNTHETIC_DEVICE_NAME_001";
    uint8_t signing_key[32];
    assert(h2_alipay_protocol_signing_key(&identity, device_name,
                                          signing_key) == 0);
    for (size_t size = 0u; size <= sizeof(input); size += 13u) {
      uint32_t expected_size = sizeof(expected);
      assert(alipay_iothub_sign(input, size, expected, &expected_size) == 0);
      h2_sm3_hmac(signing_key, sizeof(signing_key), input, size, actual);
      assert(expected_size == 32u && memcmp(expected, actual, 32u) == 0);
      ++comparisons;
      char expected_hex[65] = {0}, actual_hex[65];
      alipay_ByteToHexStr(expected, expected_hex, 32u);
      assert(h2_alipay_protocol_signature(signing_key, input, size,
                                          actual_hex) == 0);
      assert(strcmp(actual_hex, expected_hex) == 0);
      ++comparisons;
    }
    char vendor[64];
    snprintf(vendor, sizeof(vendor), "ALIPAY_BRAND_NAME_%s", identity.model);
    const char* info[] = {identity.imei, identity.mac, identity.model,
                          identity.imei, identity.sn};
    oracle_registration_t registration = {
        "AlipayInside_IoTPay_NET", identity.sn, vendor, 5u, info, 6, 1, 25};
    assert(ant_iot_device_name_req(&registration, (void*)1u) == 0);
    uint8_t packet[512];
    size_t packet_length = 0u;
    assert(h2_alipay_protocol_registration(&identity, packet, sizeof(packet),
                                           &packet_length) == 0);
    assert(packet_length == packed_length &&
           memcmp(packet, packed_registration, packet_length) == 0);
    ++comparisons;
  }
  identity = identities[0];
  tmp_device_name_buf[0] = '\0';
  storage_present = false;
  assert(strcmp(BOX_device_id(NULL), "") == 0);
  ++comparisons;
  storage_present = true;
  assert(strcmp(BOX_device_id(NULL), device_name) == 0);
  ++comparisons;
  char binding_code[256];
  int binding_length = sizeof(binding_code);
  assert(alipay_inner_get_binding_code(16385u, binding_code, &binding_length) ==
         0);
  uint8_t point[64];
  for (size_t index = 0u; index < sizeof(point); ++index)
    point[index] = (uint8_t)index;
  char* independent_body = NULL;
  size_t independent_length = 0u;
  assert(h2_alipay_protocol_prepare_bind(
             &mem, &identity, "HAIVIVI", "SYNTHETIC_BIZ_001", 16385u,
             1794449000u, point, &independent_body, &independent_length) == 0);
  assert(independent_length == strlen(request_body) &&
         strcmp(independent_body, request_body) == 0);
  h2_pal_mem_free(&mem, independent_body);
  ++comparisons;
  char independent_code[256];
  assert(h2_alipay_protocol_binding_code(
             &identity, "HAIVIVI", "SYNTHETIC_BINDING_TOKEN", 16385u,
             1794449000u, independent_code, sizeof(independent_code),
             &independent_length) == 0);
  assert(independent_length == (size_t)binding_length &&
         strcmp(independent_code, binding_code) == 0);
  ++comparisons;
  char result[160];
  snprintf(result, sizeof(result),
           "{\"original_binary_comparisons\":%u,\"failed\":0}\n", comparisons);
  emit(result);
  (void)semihost(1, 0, 0, 0);
  for (;;) {
  }
}
