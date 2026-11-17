#include "h2_alipay_test_binding.h"
#include "h2_alipay_client_internal.h"
#include "h2_alipay_tap_internal.h"
#include <string.h>

h2_pal_result_t h2_alipay_test_binding_seed(
    const h2_alipay_client_config_t* config) {
  binding_record_t binding = {
      .phase = BINDING_COMPLETE, .tid = "TEST_TID", .uid = "TEST_UID"};
  for (size_t index = 0u; index < sizeof(binding.secret); ++index)
    binding.secret[index] = (uint8_t)index;
  static const char plain[] =
      "28###3132333435363738393031323334353637383930###60###";
  uint8_t iv[16], middle[128];
  size_t length = 0u;
  int rc = h2_alipay_paycode_iv(config->identity.mac, iv);
  if (rc == 0)
    rc = h2_crypto_aes_cbc_pkcs7_encrypt(
        config->crypto, config->mem, binding.secret, 16u, iv,
        (const uint8_t*)plain, sizeof(plain) - 1u, middle, sizeof(middle),
        &length);
  if (rc == 0)
    rc = h2_crypto_aes_cbc_pkcs7_encrypt(
        config->crypto, config->mem, binding.secret + 16u, 16u, iv, middle,
        length, binding.seed, sizeof(binding.seed), &binding.seed_length);
  if (rc == 0) rc = h2_alipay_client_store_binding(config, &binding);
  uint8_t context[60] = {0};
  memcpy(context, "APC1", 4u);
  h2_alipay_client_fingerprint(config, context + 4u);
  memcpy(context + 36u, "0123456789ABCDEF", 16u);
  context[52] = 7u;
  memcpy(context + 53u, "DEVICE1", 7u);
  h2_pal_pref_namespace_t* space = NULL;
  if (rc == 0)
    rc = h2_pal_pref_open(config->pref, config->pref_namespace,
                          H2_PAL_PREF_OPEN_READ_WRITE, &space);
  if (rc == 0) rc = space->set_blob(space, "context", context, sizeof(context));
  if (rc == 0) rc = space->commit(space);
  if (space != NULL) {
    int closed = space->close(space);
    if (rc == 0) rc = closed;
  }
  h2_alipay_client_wipe(&binding, sizeof(binding));
  h2_alipay_client_wipe(iv, sizeof(iv));
  h2_alipay_client_wipe(middle, sizeof(middle));
  h2_alipay_client_wipe(context, sizeof(context));
  return rc;
}

h2_pal_result_t h2_alipay_test_binding_result(
    const h2_alipay_client_config_t* config, const char* token,
    uint32_t real_amount_fen, uint8_t out[27]) {
  char squeeze[16] = {0}, twice[30];
  uint8_t plain[19] = {0};
  int rc = h2_alipay_tap_squeeze(token, squeeze);
  if (rc == 0) {
    memcpy(twice, squeeze, 15u);
    memcpy(twice + 15u, squeeze, 15u);
    rc = h2_crypto_md5(config->crypto, (const uint8_t*)twice, sizeof(twice),
                       plain);
  }
  if (rc == 0) {
    memset(out, 0, 27u);
    out[0] = out[3] = 1u;
    plain[18] = 1u;
    rc = h2_crypto_xor_mask(plain, sizeof(plain), (const uint8_t*)"T_UID", 5u,
                            false, out + 4u);
    out[23] = (uint8_t)(real_amount_fen >> 16u);
    out[24] = (uint8_t)(real_amount_fen >> 8u);
    out[25] = (uint8_t)real_amount_fen;
  }
  h2_alipay_client_wipe(squeeze, sizeof(squeeze));
  h2_alipay_client_wipe(twice, sizeof(twice));
  h2_alipay_client_wipe(plain, sizeof(plain));
  return rc;
}
