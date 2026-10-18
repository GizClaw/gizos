#include "mbedtls/md.h"
#include "mbedtls/entropy.h"
#include "mbedtls/x509_crt.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Host substitute for the board TRNG, retaining real OS entropy. */
int mbedtls_hardware_poll(void *user, unsigned char *output, size_t len, size_t *olen) {
    (void)user;
    *olen = 0;
    FILE *source = fopen("/dev/urandom", "rb");
    if (source == NULL) return MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;
    size_t got = fread(output, 1, len, source);
    fclose(source);
    if (got != len) return MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;
    *olen = got;
    return 0;
}

/* Public certificate fixtures only. Private generation keys are discarded. */
static const unsigned char root_pem[] =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIBdDCCARugAwIBAgIBATAKBggqhkjOPQQDAzAhMR8wHQYDVQQDDBZHaXpPUyBT\n"
    "SEEzODQgcm9vdCB0ZXN0MCAXDTI2MTAxODAwMjU0MloYDzIxMjYwOTI0MDAyNTQy\n"
    "WjAhMR8wHQYDVQQDDBZHaXpPUyBTSEEzODQgcm9vdCB0ZXN0MFkwEwYHKoZIzj0C\n"
    "AQYIKoZIzj0DAQcDQgAEEv8M5CoLcut1dnPdGWMMI0ba8/jIbhgk5nd0AVPHBNVd\n"
    "rGjb6YGzZwxdzBZAxut+/X9WJkPhJj0j/GHf/I65QqNCMEAwDwYDVR0TAQH/BAUw\n"
    "AwEB/zAOBgNVHQ8BAf8EBAMCAQYwHQYDVR0OBBYEFNynOS3RdIamgv12lawQUsFz\n"
    "Tq4/MAoGCCqGSM49BAMDA0cAMEQCIEpftDhcPXm5py1m2wSGiTfAY6yG4R1uK8qP\n"
    "gTLllHrIAiAxTRTT5ysBsR/Tbyq7kI1UWTUkpAgCirMvnxTdhtDwFA==\n"
    "-----END CERTIFICATE-----\n"
;

static const unsigned char leaf_pem[] =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIBizCCATGgAwIBAgIBAzAKBggqhkjOPQQDAzAhMR8wHQYDVQQDDBZHaXpPUyBT\n"
    "SEEzODQgcm9vdCB0ZXN0MCAXDTI2MTAxODAwMjU0MloYDzIxMjYwOTI0MDAyNTQy\n"
    "WjAhMR8wHQYDVQQDDBZzaGEzODQuZXhhbXBsZS5pbnZhbGlkMFkwEwYHKoZIzj0C\n"
    "AQYIKoZIzj0DAQcDQgAETHFoIm+UVoJTHKzqAt++djxgMw3Jvxb7r0QP0RcVI7sE\n"
    "TNwbf6t8JAVJv/koFaFIbXsaNvDA+3XR3xOZzzg5+KNYMFYwDAYDVR0TAQH/BAIw\n"
    "ADAOBgNVHQ8BAf8EBAMCB4AwEwYDVR0lBAwwCgYIKwYBBQUHAwEwIQYDVR0RBBow\n"
    "GIIWc2hhMzg0LmV4YW1wbGUuaW52YWxpZDAKBggqhkjOPQQDAwNIADBFAiBnOPgh\n"
    "vXH3KFe1OpFgln+V4PC5qq1MybdCz5cgwtCQ0wIhAMulwgks+8ojKB+QmZDKa19X\n"
    "4c5A8D6S4vQWExIgsl13\n"
    "-----END CERTIFICATE-----\n"
;

static const unsigned char other_pem[] =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIBeDCCAR2gAwIBAgIBAjAKBggqhkjOPQQDAzAiMSAwHgYDVQQDDBdHaXpPUyBT\n"
    "SEEzODQgb3RoZXIgdGVzdDAgFw0yNjEwMTgwMDI1NDJaGA8yMTI2MDkyNDAwMjU0\n"
    "MlowIjEgMB4GA1UEAwwXR2l6T1MgU0hBMzg0IG90aGVyIHRlc3QwWTATBgcqhkjO\n"
    "PQIBBggqhkjOPQMBBwNCAAQ8HYHbcWpF78sO8qWBwRbgFOlgmbt1ctbjBUmHsG76\n"
    "G37B2SbNtmYwnJKkG9AvdWt7X17EzbjPVCnttk9Uc9tNo0IwQDAPBgNVHRMBAf8E\n"
    "BTADAQH/MA4GA1UdDwEB/wQEAwIBBjAdBgNVHQ4EFgQUxfH6F6S+VSHCZdcGFUjM\n"
    "Ln6J9A0wCgYIKoZIzj0EAwMDSQAwRgIhAOSrERuav41wRUk59TMJQSrVBu4UA49l\n"
    "FEMQN5GjlZJBAiEA8rwppi1QBK0VlmsI6Zgeg61K+XKZ5Te9vWJASEakpMA=\n"
    "-----END CERTIFICATE-----\n"
;

static const unsigned char abc_sha384[48] = {
    0xcb, 0x00, 0x75, 0x3f, 0x45, 0xa3, 0x5e, 0x8b, 0xb5, 0xa0, 0x3d, 0x69, 0x9a, 0xc6, 0x50, 0x07, 0x27, 0x2c, 0x32, 0xab, 0x0e, 0xde, 0xd1, 0x63, 0x1a, 0x8b, 0x60, 0x5a, 0x43, 0xff, 0x5b, 0xed, 0x80, 0x86, 0x07, 0x2b, 0xa1, 0xe7, 0xcc, 0x23, 0x58, 0xba, 0xec, 0xa1, 0x34, 0xc8, 0x25, 0xa7
};

int main(int argc, char **argv) {
    const int expect_unavailable = argc == 2 && strcmp(argv[1], "--expect-unavailable") == 0;
    unsigned char digest[48] = {0};
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA384);
    const int digest_result = mbedtls_md(info, (const unsigned char *)"abc", 3, digest);

    mbedtls_x509_crt root, leaf, other, corrupted;
    mbedtls_x509_crt_init(&root);
    mbedtls_x509_crt_init(&leaf);
    mbedtls_x509_crt_init(&other);
    mbedtls_x509_crt_init(&corrupted);
    const int root_result = mbedtls_x509_crt_parse(&root, root_pem, sizeof(root_pem));
    const int leaf_result = mbedtls_x509_crt_parse(&leaf, leaf_pem, sizeof(leaf_pem));
    const int other_result = mbedtls_x509_crt_parse(&other, other_pem, sizeof(other_pem));
    uint32_t flags = 0;
    int chain_result = MBEDTLS_ERR_X509_CERT_VERIFY_FAILED;
    if (root_result == 0 && leaf_result == 0)
        chain_result = mbedtls_x509_crt_verify(&leaf, &root, NULL,
            "sha384.example.invalid", &flags, NULL, NULL);
    printf("BK_SHA384_PROBE digest=%d root_parse=%d leaf_parse=%d verify=%d\n",
           digest_result, root_result, leaf_result, chain_result);
    if (expect_unavailable) {
        /* Both configurations execute the same real digest/chain operations.
         * The negative control must fail those operations, not a macro check. */
        assert(digest_result != 0 && chain_result != 0);
        mbedtls_x509_crt_free(&corrupted);
        mbedtls_x509_crt_free(&other);
        mbedtls_x509_crt_free(&leaf);
        mbedtls_x509_crt_free(&root);
        puts("BK_SHA384_REAL_OPERATIONS_REJECT_WITHOUT_FEATURE PASS");
        return 0;
    }
    if (digest_result != 0 || root_result != 0 || leaf_result != 0 ||
        other_result != 0 || chain_result != 0) {
        mbedtls_x509_crt_free(&corrupted);
        mbedtls_x509_crt_free(&other);
        mbedtls_x509_crt_free(&leaf);
        mbedtls_x509_crt_free(&root);
        return 1;
    }
    assert(memcmp(digest, abc_sha384, sizeof(digest)) == 0);
    assert(leaf.MBEDTLS_PRIVATE(sig_md) == MBEDTLS_MD_SHA384 && flags == 0);

    flags = 0;
    assert(mbedtls_x509_crt_verify(&leaf, &other, NULL,
        "sha384.example.invalid", &flags, NULL, NULL) != 0);
    assert(flags & MBEDTLS_X509_BADCERT_NOT_TRUSTED);

    unsigned char *bad_der = malloc(leaf.raw.len);
    assert(bad_der != NULL);
    memcpy(bad_der, leaf.raw.p, leaf.raw.len);
    /* Change a signature scalar byte, keeping the outer DER shape valid. */
    bad_der[leaf.raw.len - 1] ^= 1;
    assert(mbedtls_x509_crt_parse_der(&corrupted, bad_der, leaf.raw.len) == 0);
    flags = 0;
    assert(mbedtls_x509_crt_verify(&corrupted, &root, NULL,
        "sha384.example.invalid", &flags, NULL, NULL) != 0);
    assert(flags & MBEDTLS_X509_BADCERT_NOT_TRUSTED);
    free(bad_der);
    mbedtls_x509_crt_free(&corrupted);
    mbedtls_x509_crt_free(&other);
    mbedtls_x509_crt_free(&leaf);
    mbedtls_x509_crt_free(&root);
    puts("BK_SHA384_DIGEST_CHAIN_AND_REJECTIONS PASS");
    return 0;
}
