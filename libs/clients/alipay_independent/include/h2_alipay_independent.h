#ifndef H2_ALIPAY_INDEPENDENT_H
#define H2_ALIPAY_INDEPENDENT_H

#include "h2_alipay_client.h"
#include "os/h2_pal_sync.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Configure the self-implemented original-API facade before alipay_pre_init.
 * Copies config, borrows its pointers and sync until release succeeds.
 * Callers serialize configure/release with every operation. The facade owns
 * one pending binding session; business calls serialize through Sync PAL.
 * Headers come unchanged from the pinned header-only Alipay API delivery.
 * No vendor archive, vendor HAL callback or vendor implementation is used.
 * Currently implements initialization and network binding-code generation.
 * Binding-result polling returns a nonzero SDK-compatible error;
 * bound status is false until completion is implemented.
 * This target and the original vendor entry points are mutually exclusive. */
h2_pal_result_t h2_alipay_independent_configure(
    const h2_alipay_client_config_t* config, const h2_pal_sync_api_t* sync);

/** Release after all callers have joined. Wipes the pending session without
 * a server request or persistent deletion. A mutex-destroy failure retains
 * ownership and all borrowed dependencies for retry. Repeated release is OK. */
h2_pal_result_t h2_alipay_independent_release(void);

#ifdef __cplusplus
}
#endif
#endif
