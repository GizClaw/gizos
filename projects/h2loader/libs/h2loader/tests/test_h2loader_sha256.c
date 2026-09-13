#include "h2_loader_sha256.h"

#include <assert.h>
#include <string.h>

static void check_result(h2_loader_sha256_t *sha, const char *expected) {
    uint8_t digest[32];
    char hex[65];
    const uint8_t cleared[sizeof(*sha)] = {0};
    h2_loader_sha256_finish(sha, digest);
    h2_loader_sha256_hex(digest, hex);
    assert(strcmp(hex, expected) == 0);
    assert(memcmp(sha, cleared, sizeof(*sha)) == 0);
}

static void check_vector(const char *message, const char *expected) {
    const size_t length = strlen(message);
    /* Every split crosses a different input and padding boundary. */
    for (size_t split = 0; split <= length; ++split) {
        h2_loader_sha256_t sha;
        h2_loader_sha256_init(&sha);
        h2_loader_sha256_update(&sha, NULL, 0);
        h2_loader_sha256_update(&sha, (const uint8_t *)message, split);
        h2_loader_sha256_update(
            &sha, (const uint8_t *)message + split, length - split);
        check_result(&sha, expected);
    }
}

int main(void) {
    check_vector("",
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    check_vector("abc",
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    check_vector("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");

    /* Independently generated with Ruby Digest::SHA256, around padding and
     * compression block boundaries. */
    const struct {
        size_t length;
        const char *digest;
    } boundaries[] = {
        {55, "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318"},
        {63, "7d3e74a05d7db15bce4ad9ec0658ea98e3f06eeecf16b4c6fff2da457ddc2f34"},
        {64, "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb"},
        {65, "635361c48bb9eab14198e76ea8ab7f1a41685d6ad62aa9146d301d4f17eb0ae0"},
    };
    for (size_t i = 0; i < sizeof(boundaries) / sizeof(boundaries[0]); ++i) {
        char message[66];
        memset(message, 'a', boundaries[i].length);
        message[boundaries[i].length] = '\0';
        check_vector(message, boundaries[i].digest);
    }

    uint8_t block[1000];
    memset(block, 'a', sizeof(block));
    h2_loader_sha256_t long_sha;
    h2_loader_sha256_t other_sha;
    h2_loader_sha256_init(&long_sha);
    h2_loader_sha256_init(&other_sha);
    for (size_t i = 0; i < 1000; ++i) {
        h2_loader_sha256_update(&long_sha, block, sizeof(block));
        if (i < 3) {
            const uint8_t letter = (uint8_t)('a' + i);
            h2_loader_sha256_update(&other_sha, &letter, 1);
        }
    }
    check_result(&long_sha,
        "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    check_result(&other_sha,
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

    h2_loader_sha256_init(&long_sha);
    check_result(&long_sha,
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    return 0;
}
