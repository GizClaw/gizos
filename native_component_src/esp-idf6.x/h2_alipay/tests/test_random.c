#include "h2_esp_alipay_internal.h"
#include "esp_random.h"
#include <assert.h>
#include <limits.h>
#include <string.h>

int32_t get_Rng_number(uint8_t* data, unsigned size);
int alipay_rand(void);
int ALIPAY_mbedtls_hardware_poll(void* user, unsigned char* output,
                                 size_t length, size_t* written);
static unsigned fills, rand_calls;
void esp_fill_random(void* buffer, size_t size) {
  ++fills;
  memset(buffer, 0xa5, size);
}
uint32_t esp_random(void) { return rand_calls++ == 0 ? 0u : UINT32_MAX; }
uint64_t h2_esp_alipay_now(void) { return 0; }
void h2_esp_alipay_trace(const char* operation, const char* boundary,
                         int result, uint32_t item, uint32_t connection,
                         uint64_t started, const char* detail,
                         const void* bytes, size_t length) {
  (void)boundary;
  (void)item;
  (void)connection;
  (void)started;
  (void)detail;
  assert(strcmp(operation, "random.fill") == 0);
  assert(result == 0 || result == 1);
  assert(bytes == NULL && length == 0);
}
int main(void) {
  uint8_t output[32] = {0};
  /* The real callback return controls whether uECC accepts entropy. */
  assert(get_Rng_number(output, sizeof(output)) == 1);
  assert(fills == 1);
  for (size_t i = 0; i < sizeof(output); ++i) assert(output[i] == 0xa5);
  assert(get_Rng_number(NULL, 32) == 0);
  assert(get_Rng_number(output, 0) == 0);
  assert(fills == 1);
  size_t written = 0;
  /* mbedTLS uses the separate zero-is-success convention. */
  assert(ALIPAY_mbedtls_hardware_poll(NULL, output, sizeof(output), &written) ==
         0);
  assert(written == sizeof(output) && fills == 2);
  assert(ALIPAY_mbedtls_hardware_poll(NULL, NULL, 32, &written) == -1);
  assert(alipay_rand() == INT_MAX && rand_calls == 2);
  return 0;
}
