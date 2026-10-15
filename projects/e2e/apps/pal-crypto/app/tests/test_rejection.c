#include "h2_pal_crypto_e2e.h"
#include <assert.h>
#include <string.h>
static h2_pal_result_t forbidden_random(void *user, uint8_t *out,
                                        size_t length) {
  (void)user;
  (void)out;
  (void)length;
  assert(!"partial provider was dispatched");
  return H2_PAL_OK;
}
int main(void) {
  h2_pal_crypto_e2e_result_t result;
  assert(h2_pal_crypto_e2e_run(NULL, NULL) == H2_PAL_ERR_INVALID_ARG);
  assert(h2_pal_crypto_e2e_run(NULL, &result) == H2_PAL_ERR_INVALID_STATE);
  assert(result.blocked == H2_PAL_CRYPTO_E2E_CASE_COUNT && !result.qualified);
  const h2_pal_crypto_vtable_t partial = {.random = forbidden_random};
  const h2_pal_crypto_api_t api = {.vtable = &partial};
  h2_runtime_t runtime = {.crypto = &api};
  assert(h2_pal_crypto_e2e_run(&runtime, &result) == H2_PAL_ERR_INVALID_STATE);
  assert(result.complete && !result.qualified &&
         result.blocked == H2_PAL_CRYPTO_E2E_CASE_COUNT && !result.passed);
  for (size_t i = 0; i < H2_PAL_CRYPTO_E2E_CASE_COUNT; ++i) {
    assert(result.cases[i].id &&
           result.cases[i].status == H2_PAL_CRYPTO_E2E_BLOCKED);
    for (size_t j = 0; j < i; ++j)
      assert(strcmp(result.cases[i].id, result.cases[j].id));
  }
  return 0;
}
