/* Exercise the SDK engine with the production software-profile override.
 * Keys/certificates are ephemeral fixtures; no service credentials are used. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <mbedtls/ecp.h>
#include <mbedtls/md.h>
#include <mbedtls/pk.h>
#include <mbedtls/x509_crt.h>

static int random_bytes(void *user, unsigned char *bytes, size_t size) {
    (void)user;
    FILE *source = fopen("/dev/urandom", "rb");
    if (source == NULL) return -1;
    size_t read = fread(bytes, 1u, size, source);
    int closed = fclose(source);
    return read == size && closed == 0 ? 0 : -1;
}

static void make_key(mbedtls_pk_context *key) {
    mbedtls_pk_init(key);
    assert(mbedtls_pk_setup(key, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)) == 0);
    assert(mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP384R1, mbedtls_pk_ec(*key),
                             random_bytes, NULL) == 0);
}

static void make_certificate(mbedtls_x509_crt *certificate,
                             mbedtls_pk_context *subject,
                             mbedtls_pk_context *issuer, int ca) {
    mbedtls_x509write_cert writer;
    mbedtls_mpi serial;
    mbedtls_x509write_crt_init(&writer);
    mbedtls_mpi_init(&serial);
    assert(mbedtls_mpi_lset(&serial, ca ? 1 : 2) == 0);
    mbedtls_x509write_crt_set_version(&writer, MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_md_alg(&writer, MBEDTLS_MD_SHA384);
    mbedtls_x509write_crt_set_subject_key(&writer, subject);
    mbedtls_x509write_crt_set_issuer_key(&writer, issuer);
    assert(mbedtls_x509write_crt_set_serial(&writer, &serial) == 0);
    assert(mbedtls_x509write_crt_set_subject_name(&writer,
        ca ? "CN=BK test root" : "CN=bk-test.invalid") == 0);
    assert(mbedtls_x509write_crt_set_issuer_name(&writer, "CN=BK test root") == 0);
    assert(mbedtls_x509write_crt_set_validity(&writer,
        "20000101000000", "20991231235959") == 0);
    assert(mbedtls_x509write_crt_set_basic_constraints(&writer, ca, ca ? 1 : -1) == 0);
    assert(mbedtls_x509write_crt_set_key_usage(&writer,
        ca ? MBEDTLS_X509_KU_KEY_CERT_SIGN : MBEDTLS_X509_KU_DIGITAL_SIGNATURE) == 0);
    unsigned char der[2048];
    int length = mbedtls_x509write_crt_der(&writer, der, sizeof(der), random_bytes, NULL);
    assert(length > 0 && (size_t)length <= sizeof(der));
    mbedtls_x509_crt_init(certificate);
    assert(mbedtls_x509_crt_parse_der(certificate,
        der + sizeof(der) - (size_t)length, (size_t)length) == 0);
    mbedtls_x509write_crt_free(&writer);
    mbedtls_mpi_free(&serial);
}

int main(void) {
    assert(mbedtls_md_info_from_type(MBEDTLS_MD_SHA384) != NULL);
    mbedtls_pk_context root_key, leaf_key, wrong_key;
    mbedtls_x509_crt root, leaf, wrong_root;
    make_key(&root_key);
    make_key(&leaf_key);
    make_key(&wrong_key);
    make_certificate(&root, &root_key, &root_key, 1);
    make_certificate(&leaf, &leaf_key, &root_key, 0);
    make_certificate(&wrong_root, &wrong_key, &wrong_key, 1);
    uint32_t flags = 0u;
    assert(mbedtls_x509_crt_verify(&leaf, &root, NULL, "bk-test.invalid",
                                   &flags, NULL, NULL) == 0);
    assert(flags == 0u);
    assert(mbedtls_x509_crt_verify(&leaf, &wrong_root, NULL, "bk-test.invalid",
                                   &flags, NULL, NULL) == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED);
    assert((flags & MBEDTLS_X509_BADCERT_NOT_TRUSTED) != 0u);
    assert(mbedtls_x509_crt_verify(&leaf, &root, NULL, "wrong.invalid",
                                   &flags, NULL, NULL) == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED);
    assert((flags & MBEDTLS_X509_BADCERT_CN_MISMATCH) != 0u);
    mbedtls_x509_crt_free(&leaf);
    mbedtls_x509_crt_free(&root);
    mbedtls_x509_crt_free(&wrong_root);
    mbedtls_pk_free(&leaf_key);
    mbedtls_pk_free(&root_key);
    mbedtls_pk_free(&wrong_key);
    puts("SHA-384 chain accepted; wrong root and hostname rejected");
    return 0;
}
