#ifndef H2_WOLFSSL_H
#define H2_WOLFSSL_H

#include "h2/pal/os/h2_pal_crypto.h"
#include "h2/pal/net/h2_pal_dtls.h"
#include "h2/pal/os/h2_pal_mem.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int (*h2_wolfssl_entropy_fn)(
    void *user,
    uint8_t *out,
    size_t len);

typedef struct h2_wolfssl_config {
    /** Memory provider copied by value for the complete WolfSSL lifecycle. */
    h2_pal_mem_api_t mem;
    /** Opaque argument passed to entropy. */
    void *entropy_user;
    /** Entropy source required by Crypto and DTLS operations. */
    h2_wolfssl_entropy_fn entropy;
} h2_wolfssl_config_t;

/**
 * Initializes the process-wide complete WolfSSL provider.
 *
 * Repeated calls with the same Memory and entropy identity add owner
 * references. A different configuration is rejected until final deinit.
 */
h2_pal_result_t h2_wolfssl_init(const h2_wolfssl_config_t *config);

/**
 * Deinitializes the provider.
 *
 * This operation is idempotent. Non-final owners release their reference.
 * Releasing the final owner returns H2_PAL_ERR_INVALID_STATE while any DTLS
 * session is alive and leaves that owner reference intact.
 */
h2_pal_result_t h2_wolfssl_deinit(void);

/** Returns the ready Crypto PAL provider or the unsupported provider. */
const h2_pal_crypto_api_t *h2_wolfssl_crypto_api(void);

/** Returns the ready DTLS PAL provider or the unsupported provider. */
const h2_pal_dtls_api_t *h2_wolfssl_dtls_api(void);

#ifdef __cplusplus
}
#endif

#endif
