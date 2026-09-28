/* Keep SDK DTLS/cookie/certificate code, with the same SRTP feature as the
 * firmware. Upstream default self-tests contain SDK-only RNG symbols. */
#undef MBEDTLS_SELF_TEST
#define MBEDTLS_SSL_DTLS_SRTP
/* Match the BK minimal profile: certificate bytes may be released once the
 * handshake completes, so authentication must capture them in its callback. */
#undef MBEDTLS_SSL_KEEP_PEER_CERTIFICATE
