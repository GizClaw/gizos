#ifndef H2_ALIPAY_E2E_INDEPENDENT_H
#define H2_ALIPAY_E2E_INDEPENDENT_H

#include "h2_alipay.h"
#include "h2_alipay_client.h"
#include "h2_alipay_payment.h"
#include "os/h2_pal_sync.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Test-only resident adapter around the independent original API. Copies
 * configuration and borrows its providers and strings until release succeeds.
 * Configuration/release require all workers to be joined. Business calls use
 * the original initialization, code, query and durable binding snapshot
 * interfaces, with no vendor implementation. */
h2_pal_result_t h2_alipay_e2e_independent_configure(
    const h2_alipay_client_config_t* config, const h2_pal_sync_api_t* sync);
/** Same provider bridge with explicit storage retention metadata. */
h2_pal_result_t h2_alipay_e2e_independent_configure_with_storage(
    const h2_alipay_client_config_t* config, const h2_pal_sync_api_t* sync,
    h2_alipay_storage_scope_t scope);
/** NULL until configured. Caller borrows the API and quiescent trace. */
const h2_alipay_api_t* h2_alipay_e2e_independent_api(void);
/**
 * Static payer-code and NFC/BLE API; available before configure for target
 * input assembly. Calls require configured initialization and complete binding.
 * The integration owns physical transport, user confirmation and result
 * presentation. All original calls use unchanged vendor declarations with
 * the independent implementation. Payment operations require caller
 * serialization, including configure/release.
 */
const h2_alipay_payment_api_t* h2_alipay_e2e_independent_payment_api(void);
/** Release after workers join; a failure retains ownership for retry. */
h2_pal_result_t h2_alipay_e2e_independent_release(void);

#ifdef __cplusplus
}
#endif
#endif
