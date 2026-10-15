/* Keep SDK DTLS/cookie/certificate code, with the same SRTP feature as the
 * firmware. Upstream default self-tests contain SDK-only RNG symbols. */
#undef MBEDTLS_SELF_TEST
#define MBEDTLS_SSL_DTLS_SRTP
/* Match the BK minimal profile: certificate bytes may be released once the
 * handshake completes, so authentication must capture them in its callback. */
#undef MBEDTLS_SSL_KEEP_PEER_CERTIFICATE
/* The SDK minimal profile omits both SHA-384 and SHA-512. Exercise the
 * production override instead of inheriting upstream full-profile support. */
#undef MBEDTLS_SHA384_C
#undef MBEDTLS_SHA512_C
#include "../../include/h2_bk_mbedtls_config.h"
