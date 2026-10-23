#include "h2_carrier_setup_failure.h"
#include "h2_yyjson_json.h"
#include "freertos/task.h"

#include <assert.h>
#include <setjmp.h>
#include <stdlib.h>
#include <string.h>

static h2_carrier_device_inputs_t inputs;
static jmp_buf parked;
static unsigned park_count;

void vTaskDelay(TickType_t ticks) {
  (void)ticks;
  assert(inputs.lingxi_key[0] == '\0');
  assert(inputs.onenet_device_key[0] == '\0');
  assert(inputs.onenet_token[0] == '\0');
  const unsigned char* bytes = (const unsigned char*)&inputs;
  for (size_t i = 0u; i < sizeof(inputs); ++i) assert(bytes[i] == 0u);
  ++park_count;
  longjmp(parked, 1);
}

static void* allocate(void* user, size_t n) {
  (void)user;
  return malloc(n);
}
static void* resize(void* user, void* ptr, size_t n) {
  (void)user;
  return realloc(ptr, n);
}
static void release(void* user, void* ptr) {
  (void)user;
  free(ptr);
}

int main(void) {
  const h2_pal_mem_vtable_t vtable = {allocate, resize, release};
  const h2_pal_mem_api_t mem = {NULL, &vtable};
  h2_yyjson_json_t* json = NULL;
  assert(h2_yyjson_json_create(&mem, &json) == H2_PAL_OK);
  const char* configs[] = {
      "{\"epoch_ms\":123,\"lingxi\":{\"base_url\":\"https://fixture.example\",\"product_id\":\"p\",\"product_key\":\"fixture-product-key\",\"device_no_type\":\"SN\",\"device_no\":\"n\"},\"onenet\":{\"host\":\"fixture.example\",\"port\":8883,\"product_id\":\"p\",\"device_name\":\"n\",\"device_key\":\"Zml4dHVyZQ==\",\"params_json\":\"{}\"}}",
      "{\"epoch_ms\":123,\"lingxi\":{\"base_url\":\"https://fixture.example\",\"product_id\":\"p\",\"product_key\":\"fixture-product-key\",\"device_no_type\":\"SN\",\"device_no\":\"n\"},\"onenet\":{\"host\":\"fixture.example\",\"port\":8883,\"product_id\":\"p\",\"device_name\":\"n\",\"access_token\":\"fixture-token\",\"params_json\":\"{}\"}}",
  };
  for (size_t i = 0u; i < sizeof(configs) / sizeof(configs[0]); ++i) {
    assert(h2_carrier_device_inputs_parse(
               h2_yyjson_json_api(json), (const uint8_t*)configs[i],
               strlen(configs[i]), &inputs) == H2_PAL_OK);
    assert(inputs.lingxi_key[0] != '\0');
    assert(inputs.onenet_device_key[0] != '\0' ||
           inputs.onenet_token[0] != '\0');
    if (setjmp(parked) == 0)
      h2_carrier_setup_fail(&inputs, "network", H2_PAL_ERR_UNAVAILABLE);
    assert(park_count == i + 1u);
  }
  assert(h2_yyjson_json_destroy(&json) == H2_PAL_OK);
  return 0;
}
