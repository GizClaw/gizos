#ifndef H2_COREHTTP_H
#define H2_COREHTTP_H

#include "h2/pal/application/h2_pal_http.h"
#include "h2/pal/os/h2_pal_log.h"
#include "h2/pal/os/h2_pal_mem.h"
#include "h2/pal/net/h2_pal_net.h"
#include "h2/pal/os/h2_pal_time.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Opaque portable HTTP provider instance. */
typedef struct h2_corehttp h2_corehttp_t;

/** Immutable provider configuration copied by h2_corehttp_create(). */
typedef struct h2_corehttp_config {
    /** Borrowed allocator used for provider and per-request storage. */
    const h2_pal_mem_api_t *allocator;
    /** Borrowed Net PAL API used for DNS, TCP, TLS, and close. */
    const h2_pal_net_api_t *net;
    /** Borrowed monotonic Time PAL API used for total request deadlines. */
    const h2_pal_time_api_t *time;
    /** Optional borrowed logger. */
    const h2_pal_log_api_t *log;
    /** Default HTTPS verification policy. DEFAULT is normalized to REQUIRED. */
    h2_pal_net_tls_verify_t tls_verify;
    /** Optional root CA copied by create. */
    const uint8_t *root_ca_pem;
    size_t root_ca_pem_len;
    /** Maximum serialized request headers and parsed response headers. */
    size_t max_header_bytes;
    /** Maximum redirects followed by one request. */
    uint32_t max_redirects;
    /** Finite default used when request.timeout_ms is not positive. */
    uint32_t default_timeout_ms;
    /** Maximum bounded I/O slice used to observe cancellation. */
    uint32_t io_slice_ms;
} h2_corehttp_config_t;

/**
 * @brief Create a portable coreHTTP-backed HTTP PAL provider.
 *
 * The provider copies the config and optional root CA. PAL API objects remain
 * borrowed and must outlive the provider. The returned API remains valid until
 * h2_corehttp_destroy(). Calls use independent request state and may run
 * concurrently when the injected PAL backends support concurrent sockets.
 *
 * @param config Required provider dependencies and bounded defaults.
 * @param out_http Receives the owned provider instance on success.
 * @param out_api Receives the borrowed HTTP PAL API backed by out_http.
 * @return H2_PAL_OK on success, or a PAL argument/allocation error. Outputs are
 * reset on failure.
 */
h2_pal_result_t h2_corehttp_create(
    const h2_corehttp_config_t *config,
    h2_corehttp_t **out_http,
    h2_pal_http_api_t *out_api);

/**
 * @brief Destroy a provider after all synchronous request calls have returned.
 * @param http Owned provider, or NULL. The HTTP PAL API returned by create is
 * invalid after this call.
 */
void h2_corehttp_destroy(h2_corehttp_t *http);

#ifdef __cplusplus
}
#endif

#endif
