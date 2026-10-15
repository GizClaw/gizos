#ifndef H2_BK_MBEDTLS_CONFIG_H
#define H2_BK_MBEDTLS_CONFIG_H

/* The SDK minimal profile disables upstream's fixed-base precomputation.
 * Restore the normal software-ECP setting: the SDK's read-only base-point
 * tables avoid repeated scalar setup during certificate/signature work. */
#undef MBEDTLS_ECP_FIXED_POINT_OPTIM
#define MBEDTLS_ECP_FIXED_POINT_OPTIM 1

/* Public TLS chains can use SHA-384 signatures even with SHA-256 TLS
 * cipher suites. The minimal SDK profile already enables P-384, but omits
 * its certificate digest. Preserve normal chain and hostname verification. */
#ifndef MBEDTLS_SHA384_C
#define MBEDTLS_SHA384_C
#endif

#endif
