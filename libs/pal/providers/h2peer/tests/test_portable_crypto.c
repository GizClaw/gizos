#include "utils.h"

// These tests use assertions for both checks and the operations under test.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <string.h>

typedef struct fake_crypto {
    h2_pal_result_t result;
    size_t calls;
} fake_crypto_t;

static h2_pal_result_t fake_md5(
    void *user,
    const uint8_t *input,
    size_t input_len,
    uint8_t out[H2_PAL_CRYPTO_MD5_SIZE]) {
    fake_crypto_t *fake = (fake_crypto_t *)user;
    (void)input;
    (void)input_len;
    fake->calls++;
    memset(out, 0x5au, H2_PAL_CRYPTO_MD5_SIZE);
    return fake->result;
}

static h2_pal_result_t fake_hmac_sha1(
    void *user,
    const uint8_t *key,
    size_t key_len,
    const uint8_t *input,
    size_t input_len,
    uint8_t out[H2_PAL_CRYPTO_SHA1_SIZE]) {
    fake_crypto_t *fake = (fake_crypto_t *)user;
    (void)key;
    (void)key_len;
    (void)input;
    (void)input_len;
    fake->calls++;
    memset(out, 0xa5u, H2_PAL_CRYPTO_SHA1_SIZE);
    return fake->result;
}

int main(void) {
    fake_crypto_t fake = {.result = H2_PAL_OK};
    const h2_pal_crypto_vtable_t vtable = {
        .md5 = fake_md5,
        .hmac_sha1 = fake_hmac_sha1,
    };
    const h2_pal_crypto_api_t api = {
        .user = &fake,
        .vtable = &vtable,
    };
    uint8_t md5[H2_PAL_CRYPTO_MD5_SIZE] = {0};
    uint8_t hmac[H2_PAL_CRYPTO_SHA1_SIZE] = {0};
    assert(utils_get_md5(&api, "input", 5u, md5) == H2_PAL_OK);
    assert(utils_get_hmac_sha1(
               &api, "input", 5u, "key", 3u, hmac) == H2_PAL_OK);
    assert(fake.calls == 2u);
    assert(md5[0] == 0x5au);
    assert(hmac[0] == 0xa5u);

    fake.result = H2_PAL_ERR_IO;
    assert(utils_get_md5(&api, "input", 5u, md5) == H2_PAL_ERR_IO);
    assert(utils_get_hmac_sha1(
               &api, "input", 5u, "key", 3u, hmac) == H2_PAL_ERR_IO);
    assert(utils_get_md5(NULL, "input", 5u, md5) ==
           H2_PAL_ERR_UNSUPPORTED);
    assert(utils_get_hmac_sha1(
               NULL, "input", 5u, "key", 3u, hmac) ==
           H2_PAL_ERR_UNSUPPORTED);
    assert(utils_get_md5(&api, NULL, 1u, md5) == H2_PAL_ERR_INVALID_ARG);
    assert(utils_get_hmac_sha1(
               &api, "input", 5u, NULL, 1u, hmac) ==
           H2_PAL_ERR_INVALID_ARG);
    return 0;
}
