#ifndef H2_WOLFCRYPT_CRYPTO_INTERNAL_H
#define H2_WOLFCRYPT_CRYPTO_INTERNAL_H

#include "h2_wolfcrypt_crypto.h"

int h2_wolfcrypt_entropy_fill(uint8_t *out, size_t len);
int h2_wolfcrypt_generate_seed(unsigned char *out, unsigned int len);
void h2_wolfcrypt_secure_zero(void *data, size_t len);

#endif
