#ifndef H2_BK_MBEDTLS_RECORD_CONFIG_H
#define H2_BK_MBEDTLS_RECORD_CONFIG_H

/* The pinned SDK minimal profile bypasses the sdkconfig record limits. Apply
 * explicit Board values after that profile, retaining upstream/host defaults
 * when these SDK options are absent. AP and CP share only this mapping. */
#if defined(CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN)
#undef MBEDTLS_SSL_IN_CONTENT_LEN
#define MBEDTLS_SSL_IN_CONTENT_LEN CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN
#endif

#if defined(CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN)
#undef MBEDTLS_SSL_OUT_CONTENT_LEN
#define MBEDTLS_SSL_OUT_CONTENT_LEN CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN
#endif

#endif
