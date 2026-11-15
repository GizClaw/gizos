#include "h2_alipay_payment_model.h"
#include <assert.h>
#include <string.h>
int main(void) {
  h2_alipay_payment_model_t model = {0};
  /* These arrays intentionally lack a NUL byte: the model owns termination. */
  uint8_t first[18];
  memcpy(first, "123456789012345678", sizeof(first));
  uint8_t second[19];
  memcpy(second, "9876543210987654321", sizeof(second));
  assert(h2_alipay_payment_model_code(&model, first, sizeof(first), 1000u) ==
         0);
  assert(model.length == 18u && model.refresh_ms == 61000u);
  h2_alipay_payment_attempt_t old;
  assert(h2_alipay_payment_model_query(&model, 1000u, &old) == 0);
  assert(strcmp(old.code, "123456789012345678") == 0);
  assert(h2_alipay_payment_model_code(&model, second, sizeof(second), 61000u) ==
         0);
  assert(strcmp(model.visible, "9876543210987654321") == 0);
  assert(h2_alipay_payment_model_has(&model, old.id));
  assert(strcmp(old.code, "123456789012345678") == 0);
  h2_alipay_payment_attempt_t selected;
  assert(h2_alipay_payment_model_query(&model, 61000u, &selected) == 0);
  assert(strcmp(selected.code, "9876543210987654321") == 0);
  assert(h2_alipay_payment_model_query(&model, 61001u, &selected) ==
         H2_PAL_ERR_WOULD_BLOCK);
  assert(h2_alipay_payment_model_query(&model, 63000u, &selected) == 0);
  assert(strcmp(selected.code, "123456789012345678") == 0);
  assert(h2_alipay_payment_model_query(&model, 121001u, &selected) == 0);
  assert(!h2_alipay_payment_model_has(&model, old.id));
  assert(strcmp(selected.code, "9876543210987654321") == 0);
  assert(h2_alipay_payment_model_code(&model, (const uint8_t*)"123x", 4u,
                                      122000u) == H2_PAL_ERR_FORMAT);
  h2_alipay_payment_model_clear(&model);
  assert(memcmp(&model, &(h2_alipay_payment_model_t){0}, sizeof(model)) == 0);
  uint8_t adv[32] = {30u, 0x16u, 0x38u, 0xfau};
  for (unsigned i = 0u; i < 27u; ++i) adv[i + 4u] = (uint8_t)i;
  uint8_t result[27];
  assert(h2_alipay_payment_ble_result(adv, 31u, result) == 0);
  for (unsigned i = 0u; i < 27u; ++i) assert(result[i] == i);
  assert(h2_alipay_payment_ble_result(adv, 30u, result) == H2_PAL_ERR_FORMAT);
  adv[2] = 0x39u;
  assert(h2_alipay_payment_ble_result(adv, 31u, result) ==
         H2_PAL_ERR_NOT_FOUND);
  adv[2] = 0x38u;
  adv[0] = 29u;
  assert(h2_alipay_payment_ble_result(adv, 30u, result) == H2_PAL_ERR_FORMAT);
  return 0;
}
