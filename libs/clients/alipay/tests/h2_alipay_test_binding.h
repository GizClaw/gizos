#ifndef H2_ALIPAY_TEST_BINDING_H
#define H2_ALIPAY_TEST_BINDING_H

#include "h2_alipay_client.h"

/** Test-only synthetic TEST_TID/TEST_UID binding and encrypted seed. Uses the
 * production serializer and injected Crypto/Pref. No network or hardware. */
h2_pal_result_t h2_alipay_test_binding_seed(
    const h2_alipay_client_config_t* config);

/** Test-only terminal result for the supplied synthetic NFC token. */
h2_pal_result_t h2_alipay_test_binding_result(
    const h2_alipay_client_config_t* config, const char* token,
    uint32_t real_amount_fen, uint8_t out[27]);

#endif
