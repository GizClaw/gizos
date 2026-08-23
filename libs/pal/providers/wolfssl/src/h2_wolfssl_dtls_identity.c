#include "h2_wolfssl_internal.h"

#include <string.h>

#include <wolfssl/wolfcrypt/asn_public.h>
#include <wolfssl/wolfcrypt/ecc.h>
#include <wolfssl/wolfcrypt/random.h>
#include <wolfssl/wolfcrypt/sha256.h>

#define H2_WOLFSSL_DTLS_CERT_CAPACITY 4096u
#define H2_WOLFSSL_DTLS_KEY_CAPACITY 512u

h2_pal_result_t h2_wolfssl_dtls_generate_identity(
    WOLFSSL_CTX *context,
    uint8_t out_fingerprint[H2_PAL_DTLS_SHA256_FINGERPRINT_SIZE]) {
    uint8_t certificate[H2_WOLFSSL_DTLS_CERT_CAPACITY];
    uint8_t private_key[H2_WOLFSSL_DTLS_KEY_CAPACITY];
    WC_RNG random;
    ecc_key key;
    Cert cert;
    int random_ready = 0;
    int key_ready = 0;
    int certificate_len;
    int private_key_len;
    int result;

    memset(&random, 0, sizeof(random));
    memset(&key, 0, sizeof(key));
    memset(&cert, 0, sizeof(cert));
    result = wc_InitRng(&random);
    if (result == 0) {
        random_ready = 1;
        result = wc_ecc_init(&key);
    }
    if (result == 0) {
        key_ready = 1;
        result = wc_ecc_make_key(&random, 32, &key);
    }
    if (result == 0) {
        result = wc_InitCert(&cert);
    }
    if (result == 0) {
        (void)strncpy(
            cert.subject.commonName, "h2", sizeof(cert.subject.commonName) - 1u);
        cert.selfSigned = 1;
        cert.sigType = CTC_SHA256wECDSA;
        result = wc_MakeCert(
            &cert, certificate, sizeof(certificate), NULL, &key, &random);
    }
    if (result >= 0) {
        result = wc_SignCert(
            cert.bodySz, cert.sigType, certificate, sizeof(certificate),
            NULL, &key, &random);
    }
    certificate_len = result;
    if (result >= 0) {
        result = wc_EccPrivateKeyToDer(
            &key, private_key, sizeof(private_key));
    }
    private_key_len = result;
    if (result >= 0 && wolfSSL_CTX_use_certificate_buffer(
            context, certificate, certificate_len,
            WOLFSSL_FILETYPE_ASN1) != WOLFSSL_SUCCESS) {
        result = -1;
    }
    if (result >= 0 && wolfSSL_CTX_use_PrivateKey_buffer(
            context, private_key, private_key_len,
            WOLFSSL_FILETYPE_ASN1) != WOLFSSL_SUCCESS) {
        result = -1;
    }
    if (result >= 0) {
        result = wc_Sha256Hash(
            certificate, (word32)certificate_len, out_fingerprint);
    }
    if (key_ready) {
        wc_ecc_free(&key);
    }
    if (random_ready) {
        (void)wc_FreeRng(&random);
    }
    memset(certificate, 0, sizeof(certificate));
    memset(private_key, 0, sizeof(private_key));
    return result == 0 ? H2_PAL_OK : H2_PAL_ERR_IO;
}

h2_pal_result_t h2_wolfssl_dtls_verify_peer(
    h2_pal_dtls_session_t *session) {
    WOLFSSL_X509 *certificate;
    const uint8_t *der;
    uint8_t fingerprint[H2_PAL_DTLS_SHA256_FINGERPRINT_SIZE];
    int der_len = 0;
    int result;

    certificate = wolfSSL_get_peer_certificate(session->ssl);
    if (certificate == NULL) {
        return H2_PAL_ERR_TLS_VERIFY;
    }
    der = wolfSSL_X509_get_der(certificate, &der_len);
    result = der == NULL || der_len <= 0
                 ? -1
                 : wc_Sha256Hash(der, (word32)der_len, fingerprint);
    wolfSSL_X509_free(certificate);
    if (result != 0 || memcmp(
            fingerprint, session->remote_fingerprint,
            sizeof(fingerprint)) != 0) {
        memset(fingerprint, 0, sizeof(fingerprint));
        return H2_PAL_ERR_TLS_VERIFY;
    }
    memset(fingerprint, 0, sizeof(fingerprint));
    return H2_PAL_OK;
}
