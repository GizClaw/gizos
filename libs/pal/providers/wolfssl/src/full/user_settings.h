#ifndef H2_FULL_WOLFSSL_USER_SETTINGS_H
#define H2_FULL_WOLFSSL_USER_SETTINGS_H

#ifndef WOLFSSL_USER_SETTINGS
#define WOLFSSL_USER_SETTINGS
#endif

#define NO_MAIN_DRIVER
#define WOLFSSL_TLS13
#define WOLFSSL_DTLS
#ifndef WOLFSSL_DTLS_MTU
#define WOLFSSL_DTLS_MTU
#endif
#define WOLFSSL_SRTP
#define HAVE_KEYING_MATERIAL
#define WOLFSSL_CERT_GEN
#define KEEP_PEER_CERT
#define NO_OLD_TLS
#define NO_WRITEV
#define HAVE_TLS_EXTENSIONS
#define HAVE_SUPPORTED_CURVES
#if !defined(_WIN32)
#define HAVE_SYS_TIME_H
#endif
#define HAVE_SNI
#define HAVE_ALPN
#define HAVE_SESSION_TICKET
#define HAVE_HKDF
#define WC_RSA_PSS
#define WC_RSA_BLINDING
#define WOLFSSL_KEY_GEN

#define HAVE_AESGCM
#define WOLFSSL_AES_COUNTER
#define HAVE_CHACHA
#define HAVE_POLY1305
#define HAVE_CHACHA_POLY
#define HAVE_ONE_TIME_AUTH

#define HAVE_ECC
#define HAVE_ECC_KEYGEN
#define HAVE_ECC_SIGN
#define HAVE_ECC_VERIFY
#define HAVE_ECC_KEY_IMPORT
#define HAVE_ECC_KEY_EXPORT
#define HAVE_COMP_KEY
#define ECC_USER_CURVES
#undef NO_ECC256
#define ECC_TIMING_RESISTANT
#define ECC_SHAMIR
#define HAVE_CURVE25519

#define WOLFSSL_SHA384
#define WOLFSSL_SHA512
#define HAVE_HASHDRBG
#define WOLFSSL_ASN_TEMPLATE

#define WOLFSSL_SP_MATH_ALL
#if defined(_MSC_VER)
#define WOLFSSL_HAVE_MAX
#define WOLFSSL_HAVE_MIN
#define WOLFSSL_SP_NO_DYN_STACK
#endif

#define WOLFSSL_SYS_CA_CERTS
#if defined(__APPLE__)
#define WOLFSSL_APPLE_NATIVE_CERT_VALIDATION
#endif

#define NO_DH
#define NO_DSA
#define NO_MD4
#define NO_RC4
#define NO_DES3
#define NO_DES3_TLS_SUITES
#define NO_PSK
#define NO_PWDBASED

int h2_wolfssl_strcasecmp(const char *first, const char *second);
#define XSTRCASECMP(first, second) h2_wolfssl_strcasecmp((first), (second))

int h2_wolfssl_generate_seed(unsigned char *out, unsigned int len);
#define CUSTOM_RAND_GENERATE_SEED h2_wolfssl_generate_seed

#endif
