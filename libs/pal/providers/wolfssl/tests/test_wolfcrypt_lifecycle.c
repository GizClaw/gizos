#include "h2_wolfcrypt_crypto.h"
#include "h2/pal/h2_pal_unsupported.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static int test_entropy(void *user, uint8_t *out, size_t len) {
    uint8_t *counter = (uint8_t *)user;
    size_t index;
    for (index = 0u; index < len; ++index) {
        out[index] = (*counter)++;
    }
    return H2_PAL_OK;
}

static int failing_entropy(void *user, uint8_t *out, size_t len) {
    (void)user;
    if (len != 0u) {
        out[0] = 0xa5u;
    }
    return H2_PAL_ERR_TIMEOUT;
}

int main(void) {
    uint8_t counter = 1u;
    h2_wolfcrypt_crypto_config_t config = {
        .entropy_user = &counter,
        .entropy = test_entropy,
    };

    h2_wolfcrypt_crypto_deinit();
    assert(h2_wolfcrypt_crypto_api() == h2_pal_unsupported_crypto_api());
    assert(h2_wolfcrypt_crypto_init(NULL) == H2_PAL_ERR_INVALID_ARG);
    assert(h2_wolfcrypt_crypto_init(&config) == H2_PAL_OK);
    assert(h2_wolfcrypt_crypto_api() != h2_pal_unsupported_crypto_api());
    assert(h2_wolfcrypt_crypto_init(&config) == H2_PAL_ERR_INVALID_STATE);
    h2_wolfcrypt_crypto_deinit();
    h2_wolfcrypt_crypto_deinit();
    assert(h2_wolfcrypt_crypto_api() == h2_pal_unsupported_crypto_api());

    config.entropy_user = NULL;
    config.entropy = failing_entropy;
    assert(h2_wolfcrypt_crypto_init(&config) == H2_PAL_OK);
    uint8_t random = 0xa5u;
    h2_pal_p256_keypair_t keypair;
    h2_pal_p256_private_key_t private_key = {0};
    h2_pal_p256_signature_t signature;
    h2_pal_x25519_keypair_t x25519_keypair;
    private_key.bytes[31] = 1u;
    assert(h2_pal_crypto_random(
               h2_wolfcrypt_crypto_api(), &random, 1u) == H2_PAL_ERR_TIMEOUT);
    assert(random == 0u);
    memset(&keypair, 0xa5, sizeof(keypair));
    assert(h2_pal_crypto_p256_keypair_generate(
               h2_wolfcrypt_crypto_api(), &keypair) == H2_PAL_ERR_IO);
    size_t index;
    const uint8_t *keypair_bytes = (const uint8_t *)&keypair;
    for (index = 0u; index < sizeof(keypair); ++index) {
        assert(keypair_bytes[index] == 0u);
    }
    assert(h2_pal_crypto_ecdsa_p256_sha256_sign(
               h2_wolfcrypt_crypto_api(), &private_key,
               NULL, 0u, &signature) == H2_PAL_ERR_IO);
    assert(h2_pal_crypto_x25519_keypair_generate(
               h2_wolfcrypt_crypto_api(), &x25519_keypair) ==
           H2_PAL_ERR_TIMEOUT);
    h2_wolfcrypt_crypto_deinit();
    return 0;
}
