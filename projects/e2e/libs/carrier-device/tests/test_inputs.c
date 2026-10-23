#include "h2_carrier_device_inputs.h"
#include "h2_yyjson_json.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void* alloc(void* user, size_t n) {
  (void)user;
  return malloc(n);
}
static void* resize(void* user, void* p, size_t n) {
  (void)user;
  return realloc(p, n);
}
static void release(void* user, void* p) {
  (void)user;
  free(p);
}
static const h2_pal_mem_vtable_t vtable = {alloc, resize, release};

static void assert_cleared(const h2_carrier_device_inputs_t* inputs) {
  const unsigned char* bytes = (const unsigned char*)inputs;
  for (size_t j = 0u; j < sizeof(*inputs); ++j) assert(bytes[j] == 0u);
}

static void credential_inputs(const char* credential, unsigned port, char* json,
                              size_t capacity) {
  const int n = snprintf(
      json, capacity,
      "{\"epoch_ms\":1792746000000,\"lingxi\":{\"base_url\":\"https://fixture.example\",\"product_id\":\"p\",\"product_key\":\"fixture-key\",\"device_no\":\"n\",\"device_no_type\":\"CMEI\"},\"onenet\":{\"host\":\"fixture.example\",\"port\":%u,\"product_id\":\"p\",\"device_name\":\"n\",%s\"params_json\":\"{}\"}}",
      port, credential);
  assert(n > 0 && (size_t)n < capacity);
}

int main(void) {
  const h2_pal_mem_api_t mem = {NULL, &vtable};
  h2_yyjson_json_t* provider = NULL;
  assert(h2_yyjson_json_create(&mem, &provider) == H2_PAL_OK);
  const char* good =
      "{\"epoch_ms\":1792746000000,\"lingxi\":{\"base_url\":\"https://fixture.example\",\"product_id\":\"p\",\"product_key\":\"fixture-key\",\"device_no\":\"n\",\"device_no_type\":\"CMEI\"},\"onenet\":{\"host\":\"fixture.example\",\"port\":8883,\"product_id\":\"p\",\"device_name\":\"n\",\"access_token\":\"fixture-token\",\"params_json\":\"{}\"}}";
  h2_carrier_device_inputs_t inputs;
  assert(h2_carrier_device_inputs_parse(h2_yyjson_json_api(provider),
                                        (const uint8_t*)good, strlen(good),
                                        &inputs) == H2_PAL_OK);
  assert(inputs.onenet_port == 8883u &&
         inputs.device_no_type == H2_LINGXI_DEVICE_NO_CMEI);
  assert(strcmp(inputs.lingxi_key, "fixture-key") == 0);
  assert(inputs.has_lingxi && inputs.has_onenet);
  const char* independent[] = {
      "{\"epoch_ms\":1792746000000,\"onenet\":{\"host\":\"fixture.example\",\"port\":8883,\"product_id\":\"p\",\"device_name\":\"n\",\"access_token\":\"fixture-token\",\"params_json\":\"{}\"}}",
      "{\"epoch_ms\":1792746000000,\"lingxi\":{\"base_url\":\"https://fixture.example\",\"product_id\":\"p\",\"product_key\":\"fixture-key\",\"device_no\":\"n\",\"device_no_type\":\"CMEI\"}}",
  };
  for (size_t i = 0u; i < sizeof(independent) / sizeof(independent[0]); ++i) {
    assert(h2_carrier_device_inputs_parse(
               h2_yyjson_json_api(provider), (const uint8_t*)independent[i],
               strlen(independent[i]), &inputs) == H2_PAL_OK);
    assert(inputs.has_lingxi == (i == 1u));
    assert(inputs.has_onenet == (i == 0u));
  }
  char json[4096];
  credential_inputs("\"device_key\":\"Zml4dHVyZQ==\",", 8883u, json,
                    sizeof(json));
  assert(h2_carrier_device_inputs_parse(h2_yyjson_json_api(provider),
                                        (const uint8_t*)json, strlen(json),
                                        &inputs) == H2_PAL_OK);
  assert(strcmp(inputs.onenet_device_key, "Zml4dHVyZQ==") == 0);
  assert(inputs.onenet_token[0] == '\0');
  credential_inputs(
      "\"device_key\":\"Zml4dHVyZQ==\",\"ca_pem\":\"fixture-ca\",\"tls_server_name\":\"fixture-identity\",",
      8883u, json, sizeof(json));
  assert(h2_carrier_device_inputs_parse(h2_yyjson_json_api(provider),
                                        (const uint8_t*)json, strlen(json),
                                        &inputs) == H2_PAL_OK);
  assert(strcmp(inputs.onenet_ca_pem, "fixture-ca") == 0);
  assert(strcmp(inputs.onenet_tls_server_name, "fixture-identity") == 0);
  h2_carrier_device_inputs_clear(&inputs);
  assert_cleared(&inputs);
  const char* credentials[] = {
      "", "\"access_token\":\"\",", "\"device_key\":null,",
      "\"device_key\":\"abc\\u0000def\",",
      "\"device_key\":\"fixture\",\"tls_server_name\":\"fixture-identity\","};
  for (size_t i = 0u; i < sizeof(credentials) / sizeof(credentials[0]); ++i) {
    credential_inputs(credentials[i], 8883u, json, sizeof(json));
    memset(&inputs, 0xa5, sizeof(inputs));
    assert(h2_carrier_device_inputs_parse(h2_yyjson_json_api(provider),
                                          (const uint8_t*)json, strlen(json),
                                          &inputs) == H2_PAL_ERR_FORMAT);
    assert_cleared(&inputs);
  }
  const unsigned invalid_ports[] = {0u, 65536u};
  for (size_t i = 0u; i < sizeof(invalid_ports) / sizeof(invalid_ports[0]);
       ++i) {
    credential_inputs("\"access_token\":\"fixture-token\",", invalid_ports[i],
                      json, sizeof(json));
    assert(h2_carrier_device_inputs_parse(h2_yyjson_json_api(provider),
                                          (const uint8_t*)json, strlen(json),
                                          &inputs) == H2_PAL_ERR_FORMAT);
    assert_cleared(&inputs);
  }
  const char* bad[] = {"{}",
                       "{",
                       "{\"epoch_ms\":-1}",
                       "{\"epoch_ms\":true}",
                       "{\"epoch_ms\":1.5}",
                       "{\"epoch_ms\":123}",
                       "{\"epoch_ms\":123,\"onenet\":null}"};
  for (size_t i = 0u; i < sizeof(bad) / sizeof(bad[0]); ++i) {
    assert(h2_carrier_device_inputs_parse(
               h2_yyjson_json_api(provider), (const uint8_t*)bad[i],
               strlen(bad[i]), &inputs) != H2_PAL_OK);
    assert_cleared(&inputs);
  }
  assert(h2_yyjson_json_destroy(&provider) == H2_PAL_OK);
  return 0;
}
