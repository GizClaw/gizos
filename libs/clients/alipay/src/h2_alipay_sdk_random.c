#include "h2_alipay_sdk_internal.h"
#include <limits.h>

int32_t get_Rng_number(uint8_t* data, unsigned size) {
  /* Original vendor_os sample and uECC expect 1 on success, 0 on failure. */
  int32_t rc = data != NULL && size != 0u &&
                       h2_pal_crypto_random(h2_alipay_sdk_config()->crypto,
                                            data, size) == H2_PAL_OK
                   ? 1
                   : 0;
  h2_alipay_sdk_trace("random.fill", "hal_entropy", rc, size, 0,
                      h2_alipay_sdk_now(), "uECC_success_is_one", NULL, 0);
  return rc;
}
int alipay_rand(void) {
  uint32_t value;
  do {
    if (h2_pal_crypto_random(h2_alipay_sdk_config()->crypto, (uint8_t*)&value,
                             sizeof(value)) != H2_PAL_OK)
      return 0;
    value &= INT_MAX;
  } while (value == 0u);
  return (int)value;
}
void alipay_srand(unsigned int seed) { (void)seed; }
int ALIPAY_mbedtls_hardware_poll(void* user, unsigned char* output,
                                 size_t length, size_t* written) {
  (void)user;
  if (written == NULL) return -1;
  *written = 0;
  if (output == NULL || h2_pal_crypto_random(h2_alipay_sdk_config()->crypto,
                                             output, length) != H2_PAL_OK)
    return -1;
  *written = length;
  return 0;
}
