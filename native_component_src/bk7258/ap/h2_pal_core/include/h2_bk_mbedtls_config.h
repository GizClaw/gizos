#ifndef H2_BK_MBEDTLS_CONFIG_H
#define H2_BK_MBEDTLS_CONFIG_H

/* The SDK minimal profile disables upstream's fixed-base precomputation.
 * Restore the normal software-ECP setting: the SDK's read-only base-point
 * tables avoid repeated scalar setup during certificate/signature work. */
#undef MBEDTLS_ECP_FIXED_POINT_OPTIM
#define MBEDTLS_ECP_FIXED_POINT_OPTIM 1

#endif
