#include "h2_esp_platform_net_tls_verify.h"

int h2_esp_net_tls_verify_certificate(const h2_esp_net_tls_verify_t *verify,
                                    mbedtls_x509_crt *cert, int depth,
                                    uint32_t *flags,
                                    const mbedtls_x509_time *now) {
    if (flags == NULL) return MBEDTLS_ERR_X509_FATAL_ERROR;
    if (verify == NULL || cert == NULL) {
        *flags |= MBEDTLS_X509_BADCERT_OTHER;
        return 0;
    }
    if (verify->delegate != NULL) {
        const int rc = verify->delegate(verify->delegate_user, cert, depth, flags);
        if (rc != 0) return rc;
    }
    if (now == NULL) {
        *flags |= MBEDTLS_X509_BADCERT_OTHER;
        return 0;
    }
    /* ESP-IDF's cross-signed bundle lookup constructs a trust anchor from
     * subject/SPKI only. Its SDK callback checks bundle membership and handles
     * that anchor's missing dates. It is not a wire certificate: never add
     * EXPIRED back to it after the SDK callback. No existing flag is cleared. */
    const bool bundle_anchor = verify->certificate_bundle &&
        verify->delegate != NULL && depth > 0 && cert->raw.p == NULL &&
        cert->raw.len == 0u && cert->valid_from.year == 0 &&
        cert->valid_to.year == 0;
    if (!bundle_anchor) {
        if (mbedtls_x509_time_cmp(&cert->valid_from, now) > 0)
            *flags |= MBEDTLS_X509_BADCERT_FUTURE;
        if (mbedtls_x509_time_cmp(&cert->valid_to, now) < 0)
            *flags |= MBEDTLS_X509_BADCERT_EXPIRED;
    }
    return 0;
}
