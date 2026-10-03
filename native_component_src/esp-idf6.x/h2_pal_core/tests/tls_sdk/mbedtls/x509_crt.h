#ifndef H2_TEST_MBEDTLS_X509_CRT_H
#define H2_TEST_MBEDTLS_X509_CRT_H
#include <stddef.h>
#include <stdint.h>

/* Only the SDK values the production verification policy consumes. Signature,
 * issuer lookup and hostname verification stay with the SDK delegate. */
typedef struct mbedtls_x509_time {
    int year, mon, day, hour, min, sec;
} mbedtls_x509_time;
typedef struct mbedtls_x509_buf {
    int tag;
    size_t len;
    unsigned char *p;
} mbedtls_x509_buf;
typedef struct mbedtls_x509_crt {
    mbedtls_x509_buf raw;
    mbedtls_x509_time valid_from, valid_to;
} mbedtls_x509_crt;
#define MBEDTLS_X509_BADCERT_EXPIRED 0x01u
#define MBEDTLS_X509_BADCERT_NOT_TRUSTED 0x08u
#define MBEDTLS_X509_BADCERT_OTHER 0x0100u
#define MBEDTLS_X509_BADCERT_FUTURE 0x0200u
#define MBEDTLS_ERR_X509_FATAL_ERROR (-0x3000)
int mbedtls_x509_time_cmp(const mbedtls_x509_time *a,
                          const mbedtls_x509_time *b);
#endif
