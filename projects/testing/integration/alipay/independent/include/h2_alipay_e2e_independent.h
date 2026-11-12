#ifndef H2_ALIPAY_E2E_INDEPENDENT_H
#define H2_ALIPAY_E2E_INDEPENDENT_H

#include "h2_alipay.h"
#include "h2_alipay_client.h"
#include "os/h2_pal_sync.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Test-only resident adapter around the independent original API. Copies
 * configuration and borrows its providers and strings until release succeeds.
 * Configuration/release require all workers to be joined. Business calls use
 * alipay_pre_init/alipay_get_binding_code, with no vendor implementation. */
h2_pal_result_t h2_alipay_e2e_independent_configure(
    const h2_alipay_client_config_t* config, const h2_pal_sync_api_t* sync);
/** NULL until configured. Caller borrows the API and quiescent trace. */
const h2_alipay_api_t* h2_alipay_e2e_independent_api(void);
/** Release after workers join; a failure retains ownership for retry. */
h2_pal_result_t h2_alipay_e2e_independent_release(void);

#ifdef __cplusplus
}
#endif
#endif
