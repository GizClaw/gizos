#ifndef H2_WOLFCRYPT_USER_SETTINGS_H
#define H2_WOLFCRYPT_USER_SETTINGS_H

#ifndef WOLFSSL_USER_SETTINGS
#define WOLFSSL_USER_SETTINGS
#endif

#define WOLFCRYPT_ONLY
#define SINGLE_THREADED
#define NO_FILESYSTEM
#define NO_WRITEV
#define NO_MAIN_DRIVER

#define NO_MD4
#define NO_SHA3
#define NO_SHA512
#define NO_BLAKE2

#define HAVE_CURVE25519
#define HAVE_CHACHA
#define HAVE_POLY1305
#define HAVE_CHACHA_POLY
#define HAVE_AESGCM
#define WOLFSSL_AES_COUNTER
#define HAVE_HKDF

#define NO_DES3
#define NO_DH
#define NO_DSA
#define NO_RSA
#define NO_HC128
#define NO_RABBIT
#define NO_RC4
#define NO_PWDBASED
#define NO_ASN
#define NO_CERTS
#define NO_SESSION_CACHE

#if defined(_MSC_VER)
#define WOLFSSL_HAVE_MAX
#define WOLFSSL_HAVE_MIN
#endif

#define HAVE_ECC
#define HAVE_ECC_KEYGEN
#define HAVE_ECC_SIGN
#define HAVE_ECC_VERIFY
#define HAVE_ECC_KEY_IMPORT
#define HAVE_ECC_KEY_EXPORT
#define HAVE_COMP_KEY
#define ECC_TIMING_RESISTANT
#define ECC_SHAMIR
#define NO_ECC192
#define NO_ECC224
#define NO_ECC384
#define NO_ECC521
#define NO_ECC_DHE
#define NO_ECC_CDH

#define WOLFSSL_SP_MATH
#define WOLFSSL_SP_SMALL
#define WOLFSSL_HAVE_SP_ECC
#define SP_WORD_SIZE 32

int h2_wolfcrypt_strcasecmp(const char *first, const char *second);
#define XSTRCASECMP(first, second) h2_wolfcrypt_strcasecmp((first), (second))

int h2_wolfcrypt_generate_seed(unsigned char *out, unsigned int len);
#define CUSTOM_RAND_GENERATE_SEED h2_wolfcrypt_generate_seed

#endif
