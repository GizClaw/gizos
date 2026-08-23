#ifndef H2_WOLFCRYPT_CRYPTO_H
#define H2_WOLFCRYPT_CRYPTO_H

#include "h2/pal/os/h2_pal_crypto.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Supplies cryptographically secure entropy to the wolfCrypt provider.
 *
 * The callback must fill all `len` bytes before returning `H2_PAL_OK`.
 * Production implementations must use a target TRNG or an equivalent
 * cryptographically secure source. `out` is caller-owned storage valid only
 * for the synchronous callback. A zero `len` must return `H2_PAL_OK`.
 *
 * @param user Borrowed callback context supplied in the provider config.
 * @param out Caller-owned output storage, or NULL only when `len` is zero.
 * @param len Number of bytes to fill.
 * @return `H2_PAL_OK` after filling all bytes, or a stable H2 PAL error.
 */
typedef int (*h2_wolfcrypt_entropy_fn)(
    void *user,
    uint8_t *out,
    size_t len);

/**
 * Configures the process-wide wolfCrypt Crypto PAL provider.
 *
 * The provider copies both fields during initialization. The config object
 * itself may be released after `h2_wolfcrypt_crypto_init()` returns.
 */
typedef struct h2_wolfcrypt_crypto_config {
    /** Borrowed context passed unchanged to `entropy`. */
    void *entropy_user;
    /** Required entropy callback; the function pointer is copied by init. */
    h2_wolfcrypt_entropy_fn entropy;
} h2_wolfcrypt_crypto_config_t;

/**
 * Initializes the wolfCrypt provider.
 *
 * The config object is borrowed only during this synchronous call. The
 * callback context must remain valid until deinitialization and must support
 * concurrent calls when Crypto PAL operations run concurrently.
 *
 * @param config Required provider configuration.
 * @return `H2_PAL_OK`, `H2_PAL_ERR_INVALID_ARG`, or
 * `H2_PAL_ERR_INVALID_STATE` when already initialized.
 */
int h2_wolfcrypt_crypto_init(
    const h2_wolfcrypt_crypto_config_t *config);

/**
 * Deinitializes the provider and clears its private state.
 *
 * This operation is idempotent. The caller must serialize lifecycle changes
 * against all Crypto PAL operations.
 */
void h2_wolfcrypt_crypto_deinit(void);

/**
 * Returns the ready provider, or the canonical unsupported Crypto PAL API
 * before initialization and after deinitialization.
 *
 * @return A borrowed, non-NULL Crypto PAL API object.
 */
const h2_pal_crypto_api_t *h2_wolfcrypt_crypto_api(void);

#ifdef __cplusplus
}
#endif

#endif
