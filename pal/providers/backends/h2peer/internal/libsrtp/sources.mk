H2_LIBSRTP_UPSTREAM_SOURCES := \
  crypto/cipher/cipher.c \
  crypto/cipher/cipher_test_cases.c \
  crypto/cipher/null_cipher.c \
  crypto/hash/auth.c \
  crypto/hash/auth_test_cases.c \
  crypto/hash/null_auth.c \
  crypto/kernel/crypto_kernel.c \
  crypto/kernel/err.c \
  crypto/kernel/key.c \
  crypto/math/datatypes.c \
  crypto/replay/rdb.c \
  crypto/replay/rdbx.c \
  srtp/srtp.c

H2_LIBSRTP_SOURCES := \
  src/h2_libsrtp.c \
  src/h2_libsrtp_alloc.c \
  src/h2_libsrtp_crypto.c
