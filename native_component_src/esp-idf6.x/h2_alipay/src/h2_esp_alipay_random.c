#include "h2_esp_alipay_internal.h"
#include "esp_random.h"
#include <limits.h>

int32_t get_Rng_number(uint8_t* data, unsigned size) {
  /* Original vendor_os sample and uECC expect 1 on success, 0 on failure. */
  int32_t rc = data != NULL && size != 0u ? 1 : 0;
  if (rc == 1) esp_fill_random(data, size);
  h2_esp_alipay_trace("random.fill", "hal_entropy", rc, size, 0,
                      h2_esp_alipay_now(), "uECC_success_is_one", NULL, 0);
  return rc;
}
int alipay_rand(void) {
  uint32_t value;
  do {
    value = esp_random() & INT_MAX;
  } while (value == 0u);
  return (int)value;
}
void alipay_srand(unsigned int seed) { (void)seed; }
int ALIPAY_mbedtls_hardware_poll(void* user, unsigned char* output,
                                 size_t length, size_t* written) {
  (void)user;
  if (output == NULL || written == NULL) return -1;
  esp_fill_random(output, length);
  *written = length;
  return 0;
}
