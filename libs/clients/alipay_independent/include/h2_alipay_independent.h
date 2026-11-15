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
 * No vendor archive or implementation is used. The original
 * alipay_get_companyName/productModel getters return injected fields after
 * configure, including before init. They make no network request; successful
 * lengths exclude the NUL terminator and input capacities must include it.
 * Implements initialization, binding-code generation, query/finish, durable
 * bound status, payer-code generation, string payment-result queries and the
 * six original device-info getters. Only product type 3
 * and feature 0x4001 currently have a qualified protocol implementation.
 * alipay_get_productType returns UINT32_MAX while unconfigured/lock failure.
 * Original query ABI: a valid initialized query returns OK; UNKNOWN indicates
 * pending/transport failure, BINDING_OK verified completion, FAIL other errors.
 * Use the H2 diagnostics below to distinguish PAL/storage errors.
 * This target and the original vendor entry points are mutually exclusive. */
h2_pal_result_t h2_alipay_independent_configure(
    const h2_alipay_client_config_t* config, const h2_pal_sync_api_t* sync);

typedef struct h2_alipay_independent_binding_result {
  h2_alipay_client_binding_t binding;
  int result;
  h2_alipay_client_stage_t stage;
  int http_status, rpc_status;
  unsigned requests;
} h2_alipay_independent_binding_result_t;

/** Copy non-secret diagnostics of the last query under the facade lock.
 * Does not issue a request. Call after alipay_query_binding_result with callers
 * serialized to associate both results. */
h2_pal_result_t h2_alipay_independent_last_binding_result(
    h2_alipay_independent_binding_result_t* out);

/** Local injected-store snapshot after init, under the facade lock. No RPC. */
h2_pal_result_t h2_alipay_independent_binding_snapshot(
    h2_alipay_client_binding_t* out);

/** Last original payer-code/result call's non-secret diagnostics, copied
 * under the facade lock. No SDK or network operation is performed. */
h2_pal_result_t h2_alipay_independent_last_payment_result(
    h2_alipay_client_payment_t* out);

/** Release after all callers have joined. Wipes the pending session without
 * a server request or persistent deletion. A mutex-destroy failure retains
 * ownership and all borrowed dependencies for retry. Repeated release is OK. */
h2_pal_result_t h2_alipay_independent_release(void);

#ifdef __cplusplus
}
#endif
#endif
